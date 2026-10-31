/* seekdb <-> hipVS bridge: plain-C CAGRA kNN; all cuVS/ROCm deps live here.
 * Index build: exact GPU kNN (hipBLAS GEMM tiles + top-K selection) + CAGRA prune/merge
 * (seekdb_cuvs_graph.hip), wrapped with cuvsCagraIndexFromArgs; cuvsCagraBuild
 * (NN-descent) remains the fallback. */
#include "seekdb_cuvs_bridge.h"
#include "seekdb_cuvs_graph.h"
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#include <dlpack/dlpack.h>
#ifdef __HIP_PLATFORM_AMD__
#include <cuvs/cuda_runtime.h>
#else
#include <cuda_runtime.h>
#endif
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* CAGRA graph parameters (same as the NN-descent build below). */
#define SK_GRAPH_DEGREE 128
#define SK_KNN_DEGREE   256

typedef struct {
  cuvsResources_t        res;
  cuvsCagraIndexParams_t ip;
  cuvsCagraIndex_t       idx;
  long n, dim;
  /* device buffers referenced by idx when it was built by the GPU graph path */
  void* d_dataset;
  void* d_graph;
  /* single-query search state, reused across calls (guarded by mu) */
  pthread_mutex_t mu;
  cuvsCagraSearchParams_t sp;
  size_t sp_default_itopk;
  float* d_q; unsigned* d_ids; float* d_dist;
  float* h_q; unsigned* h_ids; float* h_dist;
  long cap_k;
} sk_cuvs_index;

static sk_cuvs_index* sk_new_index(void)
{
  sk_cuvs_index* h = (sk_cuvs_index*)calloc(1, sizeof(sk_cuvs_index));
  if (h && pthread_mutex_init(&h->mu, NULL) != 0) { free(h); h = NULL; }
  return h;
}

static double sk_now_ms(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* SEEKDB_CUVS_GPU_GRAPH=0 selects the cuvsCagraBuild (NN-descent) build for A/B checks. */
static int sk_gpu_graph_enabled(long n, long dim)
{
  const char* e = getenv("SEEKDB_CUVS_GPU_GRAPH");
  if (e && e[0] == '0') return 0;
  return dim > 0 && n >= seekdb_cuvs_graph_min_rows(SK_KNN_DEGREE) && n < 0xFFFFFFFFL;
}

static void sk_release_search_state(sk_cuvs_index* h)
{
  if (h->sp) cuvsCagraSearchParamsDestroy(h->sp);
  if (h->d_q) (void)hipFree(h->d_q);
  if (h->d_ids) (void)hipFree(h->d_ids);
  if (h->d_dist) (void)hipFree(h->d_dist);
  if (h->h_q) (void)hipHostFree(h->h_q);
  if (h->h_ids) (void)hipHostFree(h->h_ids);
  if (h->h_dist) (void)hipHostFree(h->h_dist);
  h->sp = 0; h->d_q = 0; h->d_ids = 0; h->d_dist = 0; h->h_q = 0; h->h_ids = 0; h->h_dist = 0;
  h->cap_k = 0;
}

void* seekdb_cuvs_build(const float* base, long n, long dim)
{
  sk_cuvs_index* h = sk_new_index();
  if (!h) return NULL;
  if (cuvsResourcesCreate(&h->res) != CUVS_SUCCESS) { pthread_mutex_destroy(&h->mu); free(h); return NULL; }

  DLManagedTensor dset; long ds[2] = {n, dim};
  dset.dl_tensor.data = (void*)base;
  dset.dl_tensor.device.device_type = kDLCPU; dset.dl_tensor.device.device_id = 0;
  dset.dl_tensor.ndim = 2;
  dset.dl_tensor.dtype.code = kDLFloat; dset.dl_tensor.dtype.bits = 32; dset.dl_tensor.dtype.lanes = 1;
  dset.dl_tensor.shape = ds; dset.dl_tensor.strides = 0; dset.dl_tensor.byte_offset = 0;

  if (cuvsCagraIndexParamsCreate(&h->ip) != CUVS_SUCCESS) goto fail_res;
  h->ip->build_algo = NN_DESCENT;
  h->ip->graph_degree = SK_GRAPH_DEGREE;
  h->ip->intermediate_graph_degree = SK_KNN_DEGREE;
  h->ip->nn_descent_niter = 100;
  if (cuvsCagraIndexCreate(&h->idx)      != CUVS_SUCCESS) goto fail_ip;
  if (sk_gpu_graph_enabled(n, dim)) {
    seekdb_cuvs_graph_stats st;
    memset(&st, 0, sizeof(st));
    const double t0 = sk_now_ms();
    const int grc = seekdb_cuvs_graph_build_index(h->res, base, n, dim, SK_KNN_DEGREE, SK_GRAPH_DEGREE,
                                                  h->idx, &h->d_dataset, &h->d_graph, &st);
    if (grc == 0) {
      fprintf(stderr,
              "[seekdb_cuvs] GPU CAGRA graph n=%ld dim=%ld knn=%d degree=%d upload_ms=%.1f knn_ms=%.1f "
              "prune_ms=%.1f reverse_ms=%.1f merge_ms=%.1f wrap_ms=%.1f stage_ms=%.1f total_ms=%.1f "
              "short_rows=%u knn_path=%u tile_rows=%u exact_rows=%u\n",
              n, dim, SK_KNN_DEGREE, SK_GRAPH_DEGREE, st.upload_ms, st.knn_ms, st.prune_ms,
              st.reverse_ms, st.merge_ms, st.wrap_ms, st.stage_ms, sk_now_ms() - t0, st.short_rows,
              st.knn_path, st.tile_rows, st.exact_rows);
      h->n = n; h->dim = dim;
      return h;
    }
    fprintf(stderr, "[seekdb_cuvs] GPU CAGRA graph build failed (rc=%d), using cuvsCagraBuild\n", grc);
    cuvsCagraIndexDestroy(h->idx);
    h->idx = 0;
    if (cuvsCagraIndexCreate(&h->idx) != CUVS_SUCCESS) goto fail_ip;
  }
  if (cuvsCagraBuild(h->res, h->ip, &dset, h->idx) != CUVS_SUCCESS) goto fail_idx;
  h->n = n; h->dim = dim;
  return h;

fail_idx: cuvsCagraIndexDestroy(h->idx);
fail_ip:  cuvsCagraIndexParamsDestroy(h->ip);
fail_res: cuvsResourcesDestroy(h->res); pthread_mutex_destroy(&h->mu); free(h);
  fprintf(stderr, "[seekdb_cuvs] build failed: %s\n", cuvsGetLastErrorText());
  return NULL;
}

/* itopk/search_width exactly as the per-call parameters below */
static void sk_set_search_params(cuvsCagraSearchParams_t sp, size_t default_itopk, long topk)
{
  size_t required_itopk = (size_t)topk * 4;
  if (required_itopk < 512) required_itopk = 512;
  sp->itopk_size = default_itopk < required_itopk ? required_itopk : default_itopk;
  sp->search_width = 4;
}

/* One query with buffers kept in the handle: no device allocation and a single stream
 * synchronisation per call. Returns -1 when the buffers cannot be set up (the caller then
 * uses the per-call path), otherwise the same codes as seekdb_cuvs_search. */
static int sk_search_one(sk_cuvs_index* h, const float* query, long topk,
                         unsigned int* out_ids, float* out_dist)
{
  cudaStream_t stream;
  if (cuvsStreamGet(h->res, &stream) != CUVS_SUCCESS) return -1;
  if (!h->sp) {
    if (cuvsCagraSearchParamsCreate(&h->sp) != CUVS_SUCCESS) { h->sp = 0; return -1; }
    h->sp_default_itopk = h->sp->itopk_size;
  }
  if (!h->d_q) {
    if (hipMalloc((void**)&h->d_q, sizeof(float) * h->dim) != hipSuccess) { h->d_q = 0; return -1; }
    if (hipHostMalloc((void**)&h->h_q, sizeof(float) * h->dim, 0) != hipSuccess) { h->h_q = 0; return -1; }
  }
  if (topk > h->cap_k) {
    const long cap = topk < 128 ? 128 : topk;
    if (h->d_ids) (void)hipFree(h->d_ids);
    if (h->d_dist) (void)hipFree(h->d_dist);
    if (h->h_ids) (void)hipHostFree(h->h_ids);
    if (h->h_dist) (void)hipHostFree(h->h_dist);
    h->d_ids = 0; h->d_dist = 0; h->h_ids = 0; h->h_dist = 0; h->cap_k = 0;
    if (hipMalloc((void**)&h->d_ids, sizeof(unsigned) * cap) != hipSuccess) { h->d_ids = 0; return -1; }
    if (hipMalloc((void**)&h->d_dist, sizeof(float) * cap) != hipSuccess) { h->d_dist = 0; return -1; }
    if (hipHostMalloc((void**)&h->h_ids, sizeof(unsigned) * cap, 0) != hipSuccess) { h->h_ids = 0; return -1; }
    if (hipHostMalloc((void**)&h->h_dist, sizeof(float) * cap, 0) != hipSuccess) { h->h_dist = 0; return -1; }
    h->cap_k = cap;
  }
  memcpy(h->h_q, query, sizeof(float) * h->dim);
  if (hipMemcpyAsync(h->d_q, h->h_q, sizeof(float) * h->dim, hipMemcpyHostToDevice, stream) != hipSuccess)
    return 13;
  {
    DLManagedTensor qt, nt, dt; long qs[2] = {1, h->dim}, ks[2] = {1, topk};
    memset(&qt, 0, sizeof(qt));
    qt.dl_tensor.data = h->d_q; qt.dl_tensor.device.device_type = kDLCUDA; qt.dl_tensor.device.device_id = 0;
    qt.dl_tensor.ndim = 2; qt.dl_tensor.dtype.code = kDLFloat; qt.dl_tensor.dtype.bits = 32;
    qt.dl_tensor.dtype.lanes = 1; qt.dl_tensor.shape = qs;
    nt = qt; nt.dl_tensor.data = h->d_ids; nt.dl_tensor.dtype.code = kDLUInt; nt.dl_tensor.shape = ks;
    dt = qt; dt.dl_tensor.data = h->d_dist; dt.dl_tensor.dtype.code = kDLFloat; dt.dl_tensor.shape = ks;
    sk_set_search_params(h->sp, h->sp_default_itopk, topk);
    cuvsFilter f; f.type = NO_FILTER; f.addr = (uintptr_t)0;
    if (cuvsCagraSearch(h->res, h->sp, h->idx, &qt, &nt, &dt, f) != CUVS_SUCCESS) return 15;
  }
  if (hipMemcpyAsync(h->h_ids, h->d_ids, sizeof(unsigned) * topk, hipMemcpyDeviceToHost, stream) != hipSuccess)
    return 16;
  if (out_dist &&
      hipMemcpyAsync(h->h_dist, h->d_dist, sizeof(float) * topk, hipMemcpyDeviceToHost, stream) != hipSuccess)
    return 16;
  if (hipStreamSynchronize(stream) != hipSuccess) return 16;
  memcpy(out_ids, h->h_ids, sizeof(unsigned) * topk);
  if (out_dist) memcpy(out_dist, h->h_dist, sizeof(float) * topk);
  return 0;
}

int seekdb_cuvs_search(void* handle, const float* query, long nq, long topk,
                       unsigned int* out_ids, float* out_dist)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return 1;
  int rc = 0;
  float* qd = 0; unsigned* nd = 0; float* dd = 0; cuvsCagraSearchParams_t sp = 0;
  pthread_mutex_lock(&h->mu);
  if (nq == 1 && topk > 0 && query && out_ids) {
    rc = sk_search_one(h, query, topk, out_ids, out_dist);
    if (rc >= 0) goto done;
    sk_release_search_state(h);
    rc = 0;
  }
  if (cuvsRMMAlloc(h->res, (void**)&qd, sizeof(float)*nq*h->dim)  != CUVS_SUCCESS) { rc=10; goto done; }
  if (cuvsRMMAlloc(h->res, (void**)&nd, sizeof(unsigned)*nq*topk) != CUVS_SUCCESS) { rc=11; goto done; }
  if (cuvsRMMAlloc(h->res, (void**)&dd, sizeof(float)*nq*topk)    != CUVS_SUCCESS) { rc=12; goto done; }
  if (cudaMemcpy(qd, query, sizeof(float)*nq*h->dim, cudaMemcpyDefault) != cudaSuccess) { rc=13; goto done; }
  {
    DLManagedTensor qt, nt, dt; long qs[2]={nq,h->dim}, nsh[2]={nq,topk}, dsh[2]={nq,topk};
    qt.dl_tensor.data=qd; qt.dl_tensor.device.device_type=kDLCUDA; qt.dl_tensor.device.device_id=0;
    qt.dl_tensor.ndim=2; qt.dl_tensor.dtype.code=kDLFloat; qt.dl_tensor.dtype.bits=32; qt.dl_tensor.dtype.lanes=1;
    qt.dl_tensor.shape=qs; qt.dl_tensor.strides=0; qt.dl_tensor.byte_offset=0;
    nt=qt; nt.dl_tensor.data=nd; nt.dl_tensor.dtype.code=kDLUInt; nt.dl_tensor.shape=nsh;
    dt=qt; dt.dl_tensor.data=dd; dt.dl_tensor.dtype.code=kDLFloat; dt.dl_tensor.shape=dsh;
    if (cuvsCagraSearchParamsCreate(&sp) != CUVS_SUCCESS) { rc=14; goto done; }
    {
      size_t required_itopk = (size_t)topk * 4;
      if (required_itopk < 512) required_itopk = 512;
      if (sp->itopk_size < required_itopk) sp->itopk_size = required_itopk;
      sp->search_width = 4;
    }
    cuvsFilter f; f.type=NO_FILTER; f.addr=(uintptr_t)0;
    if (cuvsCagraSearch(h->res, sp, h->idx, &qt, &nt, &dt, f) != CUVS_SUCCESS) { rc=15; goto done; }
    if (cudaMemcpy(out_ids, nd, sizeof(unsigned)*nq*topk, cudaMemcpyDefault) != cudaSuccess) { rc=16; goto done; }
    if (out_dist) cudaMemcpy(out_dist, dd, sizeof(float)*nq*topk, cudaMemcpyDefault);
  }
done:
  if (sp) cuvsCagraSearchParamsDestroy(sp);
  if (dd) cuvsRMMFree(h->res, dd, sizeof(float)*nq*topk);
  if (nd) cuvsRMMFree(h->res, nd, sizeof(unsigned)*nq*topk);
  if (qd) cuvsRMMFree(h->res, qd, sizeof(float)*nq*h->dim);
  pthread_mutex_unlock(&h->mu);
  if (rc) fprintf(stderr, "[seekdb_cuvs] search rc=%d: %s\n", rc, cuvsGetLastErrorText());
  return rc;
}

void seekdb_cuvs_free(void* handle)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return;
  sk_release_search_state(h);
  cuvsCagraIndexDestroy(h->idx);
  /* the index only viewed these buffers; release them after the index */
  seekdb_cuvs_graph_release(h->d_graph);
  seekdb_cuvs_graph_release(h->d_dataset);
  cuvsCagraIndexParamsDestroy(h->ip);
  cuvsResourcesDestroy(h->res);
  pthread_mutex_destroy(&h->mu);
  free(h);
}

int seekdb_cuvs_serialize(void* handle, const char* filename)
{
  sk_cuvs_index* index = (sk_cuvs_index*)handle;
  if (!index || !filename) return 1;
  if (cuvsCagraSerialize(index->res, filename, index->idx, true) != CUVS_SUCCESS) {
    fprintf(stderr, "[seekdb_cuvs] serialize failed: %s\n", cuvsGetLastErrorText());
    return 1;
  }
  return 0;
}

void* seekdb_cuvs_deserialize(const char* filename, long n, long dim)
{
  if (!filename || n <= 0 || dim <= 0) return NULL;
  sk_cuvs_index* index = sk_new_index();
  if (!index) return NULL;
  if (cuvsResourcesCreate(&index->res) != CUVS_SUCCESS) goto fail;
  if (cuvsCagraIndexParamsCreate(&index->ip) != CUVS_SUCCESS) goto fail_res;
  if (cuvsCagraIndexCreate(&index->idx) != CUVS_SUCCESS) goto fail_ip;
  if (cuvsCagraDeserialize(index->res, filename, index->idx) != CUVS_SUCCESS) goto fail_idx;
  {
    int64_t actual_n = 0;
    int64_t actual_dim = 0;
    if (cuvsCagraIndexGetSize(index->idx, &actual_n) != CUVS_SUCCESS ||
        cuvsCagraIndexGetDims(index->idx, &actual_dim) != CUVS_SUCCESS ||
        actual_n != n || actual_dim != dim) {
      fprintf(stderr, "[seekdb_cuvs] snapshot shape mismatch: %ld x %ld, expected %ld x %ld\n",
              (long)actual_n, (long)actual_dim, n, dim);
      goto fail_idx;
    }
  }
  index->n = n;
  index->dim = dim;
  return index;
fail_idx:
  cuvsCagraIndexDestroy(index->idx);
fail_ip:
  cuvsCagraIndexParamsDestroy(index->ip);
fail_res:
  cuvsResourcesDestroy(index->res);
fail:
  fprintf(stderr, "[seekdb_cuvs] deserialize failed: %s\n", cuvsGetLastErrorText());
  pthread_mutex_destroy(&index->mu);
  free(index);
  return NULL;
}

int seekdb_cuvs_cagra_knn(const float* base, long n, long dim,
                          const float* query, long nq, long topk,
                          unsigned int* out_ids)
{
  void* h = seekdb_cuvs_build(base, n, dim);
  if (!h) return 10;
  int rc = seekdb_cuvs_search(h, query, nq, topk, out_ids, NULL);
  seekdb_cuvs_free(h);
  return rc;
}
