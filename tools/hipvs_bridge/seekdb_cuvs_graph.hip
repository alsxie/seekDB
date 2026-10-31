// Author：FDE WorkMate
// GPU CAGRA graph construction for the seekdb hipVS bridge (see seekdb_cuvs_graph.h).
//
// Exact kNN graph: the dataset is multiplied with itself in tiles of query rows on the GPU
// (hipBLAS SGEMM, key = |x|^2 - 2 q.x, i.e. the squared L2 distance minus a per-row
// constant), and a selection kernel extracts the K smallest keys of every row (the row
// itself excluded): a threshold taken from a sorted sample of the row collects ~5.5 K
// candidates in shared memory, which are then sorted by (key, id). Rows where the
// sample threshold does not bracket K..capacity candidates use an exact radix select
// (ties at the K-th key resolved by id), so the result is always the exact top-K.
// hipBLAS is resolved at run time (libcuvs already depends on libhipblas), so building
// the bridge needs no hipBLAS development files; without it the hipVS brute-force kNN
// is used.
//
// Pruning and merge follow cuvs::neighbors::cagra::detail::graph::optimize():
//  * detour count of edge A->B (rank kAB) = number of pairs (kAD < kAB, kDB) with
//    knn[knn[A][kAD]][kDB] == B. hipVS scans A's list for every candidate (O(K^3) per
//    node); here B is looked up in a shared-memory hash of A's list (O(K^2) per node);
//  * keep the `degree` edges with the fewest detours (counts saturate at 255), ties
//    broken by kNN rank, skipping invalid and duplicate entries;
//  * reverse graph built rank by rank, then reverse edges are inserted after the first
//    degree/2 (protected) forward edges, exactly like the hipVS host loop.
//
// Device memory during seekdb_cuvs_graph_build_index: the kNN rows are copied to host
// memory as they are produced and the dataset is released while pruning, so the peak is
// max(dataset + distance tile, kNN graph + pruned graph) instead of their sum.
#include "seekdb_cuvs_graph.h"

#include <hip/hip_runtime.h>
#include <cuvs/neighbors/brute_force.h>
#include <dlfcn.h>
#include <dlpack/dlpack.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

namespace {

constexpr uint32_t kInvalid = 0xFFFFFFFFu;
constexpr int kMaxKnn       = 256;  // largest kNN degree handled by the prune kernel
constexpr int kHashBits     = 9;    // 512-slot table = 2 x kMaxKnn
constexpr int kHashSize     = 1 << kHashBits;
constexpr int kMaxDeg       = 256;  // largest output degree handled by the merge kernel
constexpr int kQueryChunk   = 256;  // brute-force rows per call (distance tile = chunk x n floats)

// GEMM kNN selection
constexpr int kSelBlock      = 256;   // threads per selected row
constexpr uint32_t kSelCap   = 4096;  // candidate capacity per row (shared memory)
constexpr uint32_t kSelSamp  = 2048;  // samples per row for the candidate threshold
constexpr int kSelAttempts   = 5;     // threshold retries before the exact radix select
constexpr int kSelUnroll     = 8;     // float4 groups loaded per thread before use
constexpr int64_t kStageBytes = 8LL << 20;    // device staging of kNN rows copied to host
constexpr int64_t kMinTileBytes = 32LL << 20;  // smallest distance tile
constexpr int64_t kMaxTileRows = 1024;

// hipBLAS entry points used by the GEMM kNN (hipblas.h: handle = void*, enums = int).
constexpr int kBlasSuccess = 0;    // HIPBLAS_STATUS_SUCCESS
constexpr int kBlasOpN     = 111;  // HIPBLAS_OP_N
constexpr int kBlasOpT     = 112;  // HIPBLAS_OP_T
struct BlasApi {
  int (*create)(void** handle);
  int (*destroy)(void* handle);
  int (*set_stream)(void* handle, hipStream_t stream);
  int (*sgemm)(void* handle, int trans_a, int trans_b, int m, int n, int k, const float* alpha,
               const float* a, int lda, const float* b, int ldb, const float* beta, float* c,
               int ldc);
};

bool load_blas(BlasApi* api)
{
  static const char* const kNames[] = {"libhipblas.so.3", "libhipblas.so", "libhipblas.so.2"};
  void* h = nullptr;
  for (const char* name : kNames) {
    if ((h = dlopen(name, RTLD_NOW | RTLD_LOCAL)) != nullptr) break;
  }
  if (h == nullptr) return false;  // kept loaded: libcuvs depends on it anyway
  api->create     = reinterpret_cast<decltype(api->create)>(dlsym(h, "hipblasCreate"));
  api->destroy    = reinterpret_cast<decltype(api->destroy)>(dlsym(h, "hipblasDestroy"));
  api->set_stream = reinterpret_cast<decltype(api->set_stream)>(dlsym(h, "hipblasSetStream"));
  api->sgemm      = reinterpret_cast<decltype(api->sgemm)>(dlsym(h, "hipblasSgemm"));
  return api->create && api->destroy && api->set_stream && api->sgemm;
}

double now_ms()
{
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

__device__ __forceinline__ uint32_t slot_of(uint32_t v)
{
  return (v * 2654435761u) >> (32 - kHashBits);
}

void tensor2d(DLManagedTensor* t, const void* data, int64_t* shape, DLDataTypeCode code, int bits)
{
  memset(t, 0, sizeof(*t));
  t->dl_tensor.data               = const_cast<void*>(data);
  t->dl_tensor.device.device_type = kDLCUDA;
  t->dl_tensor.device.device_id   = 0;
  t->dl_tensor.ndim               = 2;
  t->dl_tensor.dtype.code         = code;
  t->dl_tensor.dtype.bits         = bits;
  t->dl_tensor.dtype.lanes        = 1;
  t->dl_tensor.shape              = shape;
}

}  // namespace

// Brute-force rows (int64 [rows x kq], nearest first, normally starting with the row
// itself) -> kNN rows [K] without the self edge; out-of-range ids become kInvalid.
__global__ void seekdb_cagra_knn_rows_kernel(const int64_t* __restrict__ nb, uint32_t kq,
                                             uint64_t row0, uint32_t K, uint64_t n,
                                             uint32_t* __restrict__ knn)
{
  __shared__ uint32_t s_self;
  const uint32_t r    = blockIdx.x;
  const uint64_t self = row0 + r;
  const int64_t* src  = nb + (uint64_t)r * kq;
  if (threadIdx.x == 0) s_self = kq;
  __syncthreads();
  for (uint32_t j = threadIdx.x; j < kq; j += blockDim.x)
    if (src[j] == (int64_t)self) atomicMin(&s_self, j);
  __syncthreads();
  const uint32_t sp = s_self;
  uint32_t* dst     = knn + self * K;
  for (uint32_t j = threadIdx.x; j < kq; j += blockDim.x) {
    if (j == sp) continue;
    const uint32_t o = j > sp ? j - 1 : j;
    if (o >= K) continue;
    const int64_t v = src[j];
    dst[o]          = (v >= 0 && (uint64_t)v < n) ? (uint32_t)v : kInvalid;
  }
}

// Squared L2 norm of every row, one wavefront per row.
__global__ void __launch_bounds__(256)
  seekdb_knn_norms_kernel(const float* __restrict__ x, uint32_t n, uint32_t dim, float* __restrict__ out)
{
  const uint32_t lane = threadIdx.x % warpSize;
  const uint64_t row  = ((uint64_t)blockIdx.x * blockDim.x + threadIdx.x) / warpSize;
  if (row >= n) return;
  const float* r = x + row * dim;
  float s        = 0.0f;
  for (uint32_t k = lane; k < dim; k += warpSize) {
    const float v = r[k];
    s += v * v;
  }
  for (int off = warpSize / 2; off > 0; off >>= 1) s += __shfl_down(s, off);
  if (lane == 0) out[row] = s;
}

// Order-preserving map of a float key to uint32 (NaN sorts last).
__device__ __forceinline__ uint32_t sel_key(float f)
{
  const uint32_t b = __float_as_uint(f);
  if ((b & 0x7FFFFFFFu) > 0x7F800000u) return 0xFFFFFFFFu;
  return (b & 0x80000000u) ? ~b : (b | 0x80000000u);
}

// Ascending bitonic sort of s[0..P) (P a power of two) by the whole block.
template <typename T>
__device__ void sel_sort(T* s, uint32_t P)
{
  for (uint32_t k = 2; k <= P; k <<= 1) {
    for (uint32_t j = k >> 1; j > 0; j >>= 1) {
      for (uint32_t i = threadIdx.x; i < P; i += blockDim.x) {
        const uint32_t ixj = i ^ j;
        if (ixj > i) {
          const T a     = s[i];
          const T b     = s[ixj];
          const bool up = (i & k) == 0;
          if ((a > b) == up) {
            s[i]   = b;
            s[ixj] = a;
          }
        }
      }
      __syncthreads();
    }
  }
}

// Wavefront-aggregated append of (key << 32 | id) to the candidate list; must be reached
// by every lane of the wavefront (the callers keep their loops uniform).
__device__ __forceinline__ void sel_append(bool pred, uint32_t key, uint32_t id, uint64_t* sC,
                                           uint32_t* sCnt)
{
  const unsigned long long m = __ballot(pred);
  if (m == 0) return;
  const uint32_t lane   = __lane_id();
  const uint32_t leader = (uint32_t)__ffsll((long long)m) - 1;
  uint32_t base         = 0;
  if (lane == leader) base = atomicAdd(sCnt, (uint32_t)__popcll(m));
  base = __shfl(base, (int)leader);
  if (pred) {
    const uint32_t p = base + (uint32_t)__popcll(m & ((1ull << lane) - 1ull));
    if (p < kSelCap) sC[p] = ((uint64_t)key << 32) | id;
  }
}

// Appends every element of the row (except the row itself) whose key is <= t. The loads of
// kSelUnroll float4 groups are issued before any of them is used (latency bound otherwise).
__device__ void sel_collect(const float* __restrict__ row, const float* __restrict__ xn, uint32_t n,
                            uint32_t self, uint32_t t, uint64_t* sC, uint32_t* sCnt)
{
  constexpr int U    = kSelUnroll;
  const uint32_t n4  = n >> 2;
  const float4* row4 = reinterpret_cast<const float4*>(row);
  const float4* xn4  = reinterpret_cast<const float4*>(xn);
  for (uint32_t b = 0; b < n4; b += blockDim.x * U) {
    float4 a[U], c[U];
#pragma unroll
    for (int u = 0; u < U; ++u) {
      const uint32_t g = b + u * blockDim.x + threadIdx.x;
      if (g < n4) {
        a[u] = row4[g];
        c[u] = xn4[g];
      } else {
        a[u] = make_float4(0.f, 0.f, 0.f, 0.f);
        c[u] = a[u];
      }
    }
#pragma unroll
    for (int u = 0; u < U; ++u) {
      const uint32_t g  = b + u * blockDim.x + threadIdx.x;
      const bool valid  = g < n4;
      const uint32_t i  = g * 4;
      const uint32_t k0 = sel_key(a[u].x + c[u].x), k1 = sel_key(a[u].y + c[u].y);
      const uint32_t k2 = sel_key(a[u].z + c[u].z), k3 = sel_key(a[u].w + c[u].w);
      sel_append(valid && k0 <= t && i != self, k0, i, sC, sCnt);
      sel_append(valid && k1 <= t && i + 1 != self, k1, i + 1, sC, sCnt);
      sel_append(valid && k2 <= t && i + 2 != self, k2, i + 2, sC, sCnt);
      sel_append(valid && k3 <= t && i + 3 != self, k3, i + 3, sC, sCnt);
    }
  }
  for (uint32_t b = n4 * 4; b < n; b += blockDim.x) {
    const uint32_t i = b + threadIdx.x;
    const bool valid = i < n && i != self;
    const uint32_t k = valid ? sel_key(row[i] + xn[i]) : 0xFFFFFFFFu;
    sel_append(valid && k <= t, k, i, sC, sCnt);
  }
}

// One block per tile row q (dataset row row0 + q): the K nearest other rows, nearest first
// (ties by id), from the key row tile[q][0..n) + xn[0..n).
__global__ void __launch_bounds__(kSelBlock)
  seekdb_knn_select_kernel(const float* __restrict__ tile, uint64_t ldt, const float* __restrict__ xn,
                           uint32_t n, uint32_t row0, uint32_t K, uint32_t r0,
                           uint32_t* __restrict__ out, uint32_t* __restrict__ n_exact)
{
  __shared__ uint64_t sC[kSelCap];
  __shared__ uint32_t sS[kSelSamp];
  __shared__ uint32_t sHist[256];
  __shared__ uint32_t sW[64];
  __shared__ uint32_t sCnt, sPrefix, sNeed, sTot;
  const uint32_t q    = blockIdx.x;
  const uint32_t tid  = threadIdx.x;
  const uint32_t self = row0 + q;
  const float* row    = tile + (uint64_t)q * ldt;
  uint32_t cnt        = 0;
  bool ok             = false;

  if (n - 1 <= kSelCap) {
    // every other row is a candidate
    if (tid == 0) sCnt = 0;
    __syncthreads();
    sel_collect(row, xn, n, self, 0xFFFFFFFFu, sC, &sCnt);
    __syncthreads();
    cnt = sCnt;
    ok  = cnt >= K && cnt <= kSelCap;
  } else {
    for (uint32_t j = tid; j < kSelSamp; j += blockDim.x) {
      const uint32_t i = (uint32_t)(((uint64_t)j * n) / kSelSamp);
      sS[j]            = (i == self) ? 0xFFFFFFFFu : sel_key(row[i] + xn[i]);
    }
    __syncthreads();
    sel_sort(sS, kSelSamp);
    uint32_t r = r0;
    for (int attempt = 0; attempt < kSelAttempts; ++attempt) {
      const uint32_t t = sS[r - 1];
      if (tid == 0) sCnt = 0;
      __syncthreads();
      sel_collect(row, xn, n, self, t, sC, &sCnt);
      __syncthreads();
      cnt = sCnt;
      __syncthreads();
      if (cnt >= K && cnt <= kSelCap) {
        ok = true;
        break;
      }
      if (cnt < K) {
        if (r == kSelSamp) break;
        r = min(kSelSamp, r * 2);
      } else {
        if (r == 1) break;
        r = r / 2;
      }
    }
  }

  if (!ok) {
    // exact radix select of the K-th smallest key, 8 bits per pass
    uint32_t prefix = 0, mask = 0, need = K;
    for (int shift = 24; shift >= 0; shift -= 8) {
      for (uint32_t h = tid; h < 256; h += blockDim.x) sHist[h] = 0;
      __syncthreads();
      for (uint32_t i = tid; i < n; i += blockDim.x) {
        if (i == self) continue;
        const uint32_t k = sel_key(row[i] + xn[i]);
        if ((k & mask) == prefix) atomicAdd(&sHist[(k >> shift) & 255u], 1u);
      }
      __syncthreads();
      if (tid == 0) {
        uint32_t cum = 0, b = 0;
        for (; b < 255; ++b) {
          if (cum + sHist[b] >= need) break;
          cum += sHist[b];
        }
        sNeed   = need - cum;
        sPrefix = prefix | (b << shift);
      }
      __syncthreads();
      need   = sNeed;
      prefix = sPrefix;
      mask |= 255u << shift;
      __syncthreads();
    }
    // all keys below the K-th key, then `need` of the keys equal to it in id order
    if (tid == 0) sCnt = 0;
    __syncthreads();
    for (uint32_t b = 0; b < n; b += blockDim.x) {
      const uint32_t i = b + tid;
      const bool valid = i < n && i != self;
      const uint32_t k = valid ? sel_key(row[i] + xn[i]) : 0xFFFFFFFFu;
      sel_append(valid && k < prefix, k, i, sC, &sCnt);
    }
    __syncthreads();
    const uint32_t lt = min(sCnt, K);
    const uint32_t nw = (blockDim.x + warpSize - 1) / warpSize;
    const uint32_t w  = tid / warpSize;
    const uint32_t lane = __lane_id();
    uint32_t taken    = 0;
    for (uint32_t b = 0; b < n && lt + taken < K; b += blockDim.x) {
      const uint32_t i = b + tid;
      const bool f     = i < n && i != self && sel_key(row[i] + xn[i]) == prefix;
      const unsigned long long m = __ballot(f);
      if (lane == 0) sW[w] = (uint32_t)__popcll(m);
      __syncthreads();
      if (tid == 0) {
        uint32_t acc = 0;
        for (uint32_t k = 0; k < nw; ++k) {
          const uint32_t c = sW[k];
          sW[k]            = acc;
          acc += c;
        }
        sTot = acc;
      }
      __syncthreads();
      const uint32_t pos = lt + taken + sW[w] + (uint32_t)__popcll(m & ((1ull << lane) - 1ull));
      if (f && pos < K) sC[pos] = ((uint64_t)prefix << 32) | i;
      taken += sTot;
      __syncthreads();
    }
    cnt = K;
    if (tid == 0) atomicAdd(n_exact, 1u);
  }

  uint32_t P = 1;
  while (P < cnt) P <<= 1;
  for (uint32_t i = cnt + tid; i < P; i += blockDim.x) sC[i] = ~0ull;
  __syncthreads();
  sel_sort(sC, P);
  uint32_t* o = out + (uint64_t)q * K;
  for (uint32_t k = tid; k < K; k += blockDim.x) o[k] = (uint32_t)(sC[k] & 0xFFFFFFFFull);
}

// One block per node: 2-hop detour counting + rank-based selection of D edges.
__global__ void __launch_bounds__(256)
  seekdb_cagra_prune_kernel(const uint32_t* __restrict__ knn, uint32_t n, uint32_t K, uint32_t D,
                            uint32_t* __restrict__ out, uint32_t* __restrict__ n_short)
{
  __shared__ uint32_t sL[kMaxKnn];
  __shared__ uint32_t sCnt[kMaxKnn];
  __shared__ uint32_t sKey[kHashSize];
  __shared__ uint32_t sPos[kHashSize];
  __shared__ uint16_t sOrder[kMaxKnn];
  __shared__ uint8_t sSel[kHashSize];
  const uint32_t a = blockIdx.x, tid = threadIdx.x, nt = blockDim.x;
  const uint32_t* La = knn + (uint64_t)a * K;
  for (uint32_t k = tid; k < K; k += nt) {
    sL[k]   = La[k];
    sCnt[k] = 0;
  }
  for (uint32_t h = tid; h < kHashSize; h += nt) {
    sKey[h] = kInvalid;
    sPos[h] = kInvalid;
    sSel[h] = 0;
  }
  __syncthreads();
  // hash of A's list: id -> smallest rank
  for (uint32_t k = tid; k < K; k += nt) {
    const uint32_t v = sL[k];
    if (v >= n) continue;
    uint32_t h = slot_of(v);
    while (true) {
      const uint32_t prev = atomicCAS(&sKey[h], kInvalid, v);
      if (prev == kInvalid || prev == v) {
        atomicMin(&sPos[h], k);
        break;
      }
      h = (h + 1) & (kHashSize - 1);
    }
  }
  __syncthreads();
  // detours A -> D -> B with rank(D) < rank(B)
  for (uint32_t kAD = 0; kAD + 1 < K; ++kAD) {
    const uint32_t d = sL[kAD];
    if (d >= n) continue;
    const uint32_t* Ld = knn + (uint64_t)d * K;
    for (uint32_t kDB = tid; kDB < K; kDB += nt) {
      const uint32_t c = Ld[kDB];
      if (c >= n) continue;
      uint32_t h = slot_of(c);
      while (true) {
        const uint32_t key = sKey[h];
        if (key == c) {
          const uint32_t p = sPos[h];
          if (p > kAD) atomicAdd(&sCnt[p], 1u);
          break;
        }
        if (key == kInvalid) break;
        h = (h + 1) & (kHashSize - 1);
      }
    }
  }
  __syncthreads();
  // stable order by (min(detours, 255), rank)
  for (uint32_t k = tid; k < K; k += nt) {
    const uint32_t ck = min(sCnt[k], 255u);
    uint32_t r        = 0;
    for (uint32_t j = 0; j < K; ++j) {
      const uint32_t cj = min(sCnt[j], 255u);
      r += (cj < ck) || (cj == ck && j < k);
    }
    sOrder[r] = (uint16_t)k;
  }
  __syncthreads();
  if (tid == 0) {
    uint32_t* o = out + (uint64_t)a * D;
    uint32_t pk = 0;
    for (uint32_t r = 0; r < K && pk < D; ++r) {
      const uint32_t v = sL[sOrder[r]];
      if (v >= n) continue;
      uint32_t h = slot_of(v);
      while (sKey[h] != v) h = (h + 1) & (kHashSize - 1);
      if (sSel[h]) continue;
      sSel[h] = 1;
      o[pk++] = v;
    }
    if (pk < D) {
      // not reachable with an exact kNN graph (n >= 2 * (K + 1)); keeps the graph valid
      atomicAdd(n_short, 1u);
      for (uint32_t j = pk; j < D; ++j) o[j] = pk ? o[j % pk] : (uint32_t)((a + 1 + j) % n);
    }
  }
}

// Reverse edges of rank k: rev[dst][pos] = src.
__global__ void seekdb_cagra_reverse_kernel(const uint32_t* __restrict__ g, uint32_t n, uint32_t D,
                                            uint32_t k, uint32_t* __restrict__ rev,
                                            uint32_t* __restrict__ rev_cnt)
{
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= n) return;
  const uint32_t dst = g[(uint64_t)i * D + k];
  if (dst >= n) return;
  const uint32_t pos = atomicAdd(&rev_cnt[dst], 1u);
  if (pos < D) rev[(uint64_t)dst * D + pos] = i;
}

// One block per node: insert reverse edges (last first) right after the protected half.
__global__ void __launch_bounds__(256)
  seekdb_cagra_merge_kernel(uint32_t* __restrict__ g, const uint32_t* __restrict__ rev,
                            const uint32_t* __restrict__ rev_cnt, uint32_t n, uint32_t D)
{
  __shared__ uint32_t sOut[kMaxDeg];
  __shared__ uint32_t sPos;
  const uint32_t i = blockIdx.x, tid = threadIdx.x;
  uint32_t* gi       = g + (uint64_t)i * D;
  const uint32_t* ri = rev + (uint64_t)i * D;
  if (tid < D) sOut[tid] = gi[tid];
  const uint32_t prot = D / 2;
  uint32_t kr         = min(rev_cnt[i], D);
  __syncthreads();
  while (kr) {
    kr -= 1;
    const uint32_t v = ri[kr];
    if (v >= n) continue;
    if (tid == 0) sPos = D;
    __syncthreads();
    if (tid < D && sOut[tid] == v) atomicMin(&sPos, tid);
    __syncthreads();
    const uint32_t pos = sPos;
    if (pos < prot) {
      __syncthreads();
      continue;
    }
    const uint32_t last = prot + ((pos >= D) ? (D - prot - 1) : (pos - prot));
    const bool mv       = tid > prot && tid <= last;
    uint32_t prev       = 0;
    if (mv) prev = sOut[tid - 1];
    __syncthreads();
    if (mv) sOut[tid] = prev;
    if (tid == prot) sOut[prot] = v;
    __syncthreads();
  }
  if (tid < D) gi[tid] = sOut[tid];
}

#define SKG_HIP(x)                                                                   \
  do {                                                                               \
    hipError_t e_ = (x);                                                             \
    if (e_ != hipSuccess) {                                                          \
      fprintf(stderr, "[seekdb_cuvs] %s failed: %s\n", #x, hipGetErrorString(e_));   \
      rc = __LINE__;                                                                 \
      goto done;                                                                     \
    }                                                                                \
  } while (0)
#define SKG_CUVS(x)                                                                  \
  do {                                                                               \
    if ((x) != CUVS_SUCCESS) {                                                       \
      fprintf(stderr, "[seekdb_cuvs] %s failed: %s\n", #x, cuvsGetLastErrorText());  \
      rc = __LINE__;                                                                 \
      goto done;                                                                     \
    }                                                                                \
  } while (0)

namespace {

// kNN [n x K] -> pruned graph [n x D]
int prune_graph(cuvsResources_t res, const uint32_t* d_knn, uint32_t N, uint32_t K, uint32_t D,
                uint32_t** d_graph, seekdb_cuvs_graph_stats* st)
{
  int rc            = 0;
  uint32_t* g       = nullptr;
  uint32_t* d_short = nullptr;
  hipStream_t stream;
  double t0;
  *d_graph = nullptr;
  SKG_CUVS(cuvsStreamGet(res, &stream));
  t0 = now_ms();
  SKG_HIP(hipMalloc(&g, sizeof(uint32_t) * (size_t)N * D));
  SKG_HIP(hipMalloc(&d_short, sizeof(uint32_t)));
  SKG_HIP(hipMemsetAsync(d_short, 0, sizeof(uint32_t), stream));
  hipLaunchKernelGGL(seekdb_cagra_prune_kernel, dim3(N), dim3(256), 0, stream, d_knn, N, K, D, g,
                     d_short);
  SKG_HIP(hipGetLastError());
  SKG_HIP(hipStreamSynchronize(stream));
  if (st) {
    SKG_HIP(hipMemcpy(&st->short_rows, d_short, sizeof(uint32_t), hipMemcpyDeviceToHost));
    st->prune_ms = now_ms() - t0;
  }
  *d_graph = g;
  g        = nullptr;
done:
  if (d_short) (void)hipFree(d_short);
  if (g) (void)hipFree(g);
  return rc;
}

// in-place CAGRA reverse-edge merge of a pruned graph [n x D]
int merge_reverse(cuvsResources_t res, uint32_t* g, uint32_t N, uint32_t D,
                  seekdb_cuvs_graph_stats* st)
{
  int rc            = 0;
  uint32_t* rev     = nullptr;
  uint32_t* rev_cnt = nullptr;
  hipStream_t stream;
  double t0;
  SKG_CUVS(cuvsStreamGet(res, &stream));
  t0 = now_ms();
  SKG_HIP(hipMalloc(&rev, sizeof(uint32_t) * (size_t)N * D));
  SKG_HIP(hipMalloc(&rev_cnt, sizeof(uint32_t) * (size_t)N));
  SKG_HIP(hipMemsetAsync(rev, 0xff, sizeof(uint32_t) * (size_t)N * D, stream));
  SKG_HIP(hipMemsetAsync(rev_cnt, 0, sizeof(uint32_t) * (size_t)N, stream));
  for (uint32_t k = 0; k < D; ++k)
    hipLaunchKernelGGL(seekdb_cagra_reverse_kernel, dim3((N + 255) / 256), dim3(256), 0, stream,
                       g, N, D, k, rev, rev_cnt);
  SKG_HIP(hipGetLastError());
  SKG_HIP(hipStreamSynchronize(stream));
  if (st) st->reverse_ms = now_ms() - t0;
  t0 = now_ms();
  hipLaunchKernelGGL(seekdb_cagra_merge_kernel, dim3(N), dim3((D + 63) / 64 * 64), 0, stream, g,
                     rev, rev_cnt, N, D);
  SKG_HIP(hipGetLastError());
  SKG_HIP(hipStreamSynchronize(stream));
  if (st) st->merge_ms = now_ms() - t0;
done:
  if (rev) (void)hipFree(rev);
  if (rev_cnt) (void)hipFree(rev_cnt);
  return rc;
}

bool degrees_ok(int64_t n, int knn_degree, int graph_degree)
{
  return n > 0 && n < (int64_t)kInvalid && knn_degree > 0 && knn_degree <= kMaxKnn &&
         graph_degree > 0 && graph_degree <= kMaxDeg && graph_degree <= knn_degree;
}

}  // namespace

extern "C" int64_t seekdb_cuvs_graph_min_rows(int knn_degree)
{
  return 2 * ((int64_t)knn_degree + 1);
}

extern "C" void seekdb_cuvs_graph_release(void* device_ptr)
{
  if (device_ptr) (void)hipFree(device_ptr);
}

extern "C" int seekdb_cuvs_graph_optimize(cuvsResources_t res, const uint32_t* d_knn, int64_t n,
                                          int knn_degree, int graph_degree, uint32_t** d_graph,
                                          seekdb_cuvs_graph_stats* st)
{
  uint32_t* g = nullptr;
  int rc;
  *d_graph = nullptr;
  if (!degrees_ok(n, knn_degree, graph_degree)) return 1;
  rc = prune_graph(res, d_knn, (uint32_t)n, (uint32_t)knn_degree, (uint32_t)graph_degree, &g, st);
  if (rc == 0) rc = merge_reverse(res, g, (uint32_t)n, (uint32_t)graph_degree, st);
  if (rc == 0) {
    *d_graph = g;
  } else if (g) {
    (void)hipFree(g);
  }
  return rc;
}

extern "C" int seekdb_cuvs_graph_knn(cuvsResources_t res, const float* d_ds, int64_t n,
                                     int64_t dim, int knn_degree, int query_chunk,
                                     uint32_t** d_knn, seekdb_cuvs_graph_stats* st)
{
  int rc                   = 0;
  uint32_t* knn            = nullptr;
  int64_t* nb              = nullptr;
  float* dist              = nullptr;
  cuvsBruteForceIndex_t bf = nullptr;
  hipStream_t stream;
  double t0;
  const int64_t kq = (int64_t)knn_degree + 1;
  int64_t chunk    = query_chunk > 0 ? query_chunk : kQueryChunk;
  *d_knn           = nullptr;
  if (!degrees_ok(n, knn_degree, 1) || n < seekdb_cuvs_graph_min_rows(knn_degree) || dim <= 0)
    return 1;
  if (chunk > n) chunk = n;
  SKG_CUVS(cuvsStreamGet(res, &stream));
  t0 = now_ms();
  SKG_HIP(hipMalloc(&knn, sizeof(uint32_t) * (size_t)n * knn_degree));
  SKG_HIP(hipMalloc(&nb, sizeof(int64_t) * (size_t)chunk * kq));
  SKG_HIP(hipMalloc(&dist, sizeof(float) * (size_t)chunk * kq));
  SKG_CUVS(cuvsBruteForceIndexCreate(&bf));
  {
    DLManagedTensor dt;
    int64_t ds[2] = {n, dim};
    tensor2d(&dt, d_ds, ds, kDLFloat, 32);
    SKG_CUVS(cuvsBruteForceBuild(res, &dt, L2Expanded, 0.0f, bf));
  }
  for (int64_t s = 0; s < n; s += chunk) {
    const int64_t c = (n - s) < chunk ? (n - s) : chunk;
    DLManagedTensor qt, nt, et;
    int64_t qs[2] = {c, dim}, ks[2] = {c, kq};
    tensor2d(&qt, d_ds + s * dim, qs, kDLFloat, 32);
    tensor2d(&nt, nb, ks, kDLInt, 64);
    tensor2d(&et, dist, ks, kDLFloat, 32);
    cuvsFilter f;
    f.type = NO_FILTER;
    f.addr = 0;
    SKG_CUVS(cuvsBruteForceSearch(res, bf, &qt, &nt, &et, f));
    hipLaunchKernelGGL(seekdb_cagra_knn_rows_kernel, dim3((uint32_t)c), dim3(128), 0, stream, nb,
                       (uint32_t)kq, (uint64_t)s, (uint32_t)knn_degree, (uint64_t)n, knn);
    SKG_HIP(hipGetLastError());
  }
  SKG_HIP(hipStreamSynchronize(stream));
  if (st) st->knn_ms = now_ms() - t0;
  *d_knn = knn;
  knn    = nullptr;
done:
  if (bf) (void)cuvsBruteForceIndexDestroy(bf);
  if (nb) (void)hipFree(nb);
  if (dist) (void)hipFree(dist);
  if (knn) (void)hipFree(knn);
  return rc;
}

extern "C" int seekdb_cuvs_graph_knn_gemm(cuvsResources_t res, const float* d_ds, int64_t n,
                                          int64_t dim, int knn_degree, int64_t tile_rows,
                                          uint32_t* host_knn, uint32_t** d_knn,
                                          seekdb_cuvs_graph_stats* st)
{
  int rc             = 0;
  BlasApi blas{};
  void* hb           = nullptr;
  float* tile        = nullptr;
  float* xn          = nullptr;
  uint32_t* d_out    = nullptr;
  uint32_t* d_exact  = nullptr;
  hipStream_t stream;
  double t0;
  int dev = 0, ws = 64;
  int64_t c = 0, B = 0, blk0 = 0, rpb = 0;
  uint32_t r0     = 1;
  const int64_t K = knn_degree;
  const int64_t ldt = (n + 3) & ~(int64_t)3;  // 16-byte aligned rows of the distance tile
  const float alpha = -2.0f, beta = 0.0f;
  if (d_knn) *d_knn = nullptr;
  if (!degrees_ok(n, knn_degree, 1) || n < seekdb_cuvs_graph_min_rows(knn_degree) || dim <= 0 ||
      n > 0x7FFFFFFFLL || dim > 0x7FFFFFFFLL || (host_knn == nullptr && d_knn == nullptr))
    return 1;
  c = tile_rows > 0 ? tile_rows : (128LL << 20) / (ldt * (int64_t)sizeof(float));
  if (c > kMaxTileRows) c = kMaxTileRows;
  if (c < 8) c = 8;
  if (c > n) c = n;
  B = host_knn ? (kStageBytes / (K * (int64_t)sizeof(uint32_t))) / c * c : n;
  if (B < c) B = c;
  if (B > n) B = n;
  {
    // sample rank whose key selects ~5.5 K candidates (bounded by the capacity)
    int64_t T = K * 11 / 2;
    if (T < 512) T = 512;
    if (T > 1536) T = 1536;
    int64_t r = (T * (int64_t)kSelSamp + (n - 2)) / (n - 1);
    if (r < 1) r = 1;
    if (r > (int64_t)kSelSamp) r = kSelSamp;
    r0 = (uint32_t)r;
  }
  SKG_CUVS(cuvsStreamGet(res, &stream));
  t0 = now_ms();
  SKG_HIP(hipGetDevice(&dev));
  SKG_HIP(hipDeviceGetAttribute(&ws, hipDeviceAttributeWarpSize, dev));
  if (!load_blas(&blas)) {
    fprintf(stderr, "[seekdb_cuvs] hipBLAS not available: %s\n", dlerror());
    rc = __LINE__;
    goto done;
  }
  if (blas.create(&hb) != kBlasSuccess) {
    hb = nullptr;
    fprintf(stderr, "[seekdb_cuvs] hipblasCreate failed\n");
    rc = __LINE__;
    goto done;
  }
  if (blas.set_stream(hb, stream) != kBlasSuccess) {
    rc = __LINE__;
    goto done;
  }
  SKG_HIP(hipMalloc(&tile, sizeof(float) * (size_t)c * (size_t)ldt));
  SKG_HIP(hipMalloc(&xn, sizeof(float) * (size_t)ldt));
  SKG_HIP(hipMalloc(&d_out, sizeof(uint32_t) * (size_t)B * (size_t)K));
  SKG_HIP(hipMalloc(&d_exact, sizeof(uint32_t)));
  SKG_HIP(hipMemsetAsync(d_exact, 0, sizeof(uint32_t), stream));
  SKG_HIP(hipMemsetAsync(xn, 0, sizeof(float) * (size_t)ldt, stream));
  rpb = 256 / (ws > 0 && ws <= 256 ? ws : 64);
  hipLaunchKernelGGL(seekdb_knn_norms_kernel, dim3((uint32_t)((n + rpb - 1) / rpb)), dim3(256), 0,
                     stream, d_ds, (uint32_t)n, (uint32_t)dim, xn);
  SKG_HIP(hipGetLastError());
  for (int64_t s = 0; s < n; s += c) {
    const int64_t cc = (n - s) < c ? (n - s) : c;
    // tile[q][i] = -2 <x_(s+q), x_i>, column-major (n x cc) with leading dimension ldt
    if (blas.sgemm(hb, kBlasOpT, kBlasOpN, (int)n, (int)cc, (int)dim, &alpha, d_ds, (int)dim,
                   d_ds + s * dim, (int)dim, &beta, tile, (int)ldt) != kBlasSuccess) {
      fprintf(stderr, "[seekdb_cuvs] hipblasSgemm failed\n");
      rc = __LINE__;
      goto done;
    }
    hipLaunchKernelGGL(seekdb_knn_select_kernel, dim3((uint32_t)cc), dim3(kSelBlock), 0, stream,
                       tile, (uint64_t)ldt, xn, (uint32_t)n, (uint32_t)s, (uint32_t)K, r0,
                       d_out + (size_t)(s - blk0) * (size_t)K, d_exact);
    SKG_HIP(hipGetLastError());
    if (host_knn && (s + cc - blk0 == B || s + cc == n)) {
      SKG_HIP(hipMemcpyAsync(host_knn + (size_t)blk0 * (size_t)K, d_out,
                             sizeof(uint32_t) * (size_t)(s + cc - blk0) * (size_t)K,
                             hipMemcpyDeviceToHost, stream));
      SKG_HIP(hipStreamSynchronize(stream));
      blk0 = s + cc;
    }
  }
  SKG_HIP(hipStreamSynchronize(stream));
  if (st) {
    SKG_HIP(hipMemcpy(&st->exact_rows, d_exact, sizeof(uint32_t), hipMemcpyDeviceToHost));
    st->knn_ms    = now_ms() - t0;
    st->tile_rows = (uint32_t)c;
    st->knn_path  = 1;
  }
  if (host_knn == nullptr) {
    *d_knn = d_out;
    d_out  = nullptr;
  }
done:
  if (hb) (void)blas.destroy(hb);
  if (d_exact) (void)hipFree(d_exact);
  if (d_out) (void)hipFree(d_out);
  if (xn) (void)hipFree(xn);
  if (tile) (void)hipFree(tile);
  return rc;
}

extern "C" int seekdb_cuvs_graph_build_index(cuvsResources_t res, const float* base, int64_t n,
                                             int64_t dim, int knn_degree, int graph_degree,
                                             cuvsCagraIndex_t index, void** d_dataset,
                                             void** d_graph, seekdb_cuvs_graph_stats* st)
{
  int rc          = 0;
  float* ds       = nullptr;
  uint32_t* knn   = nullptr;
  uint32_t* g     = nullptr;
  void* h_knn     = MAP_FAILED;
  hipStream_t stream;
  double t0;
  int64_t tile_rows      = 0;
  const size_t ds_bytes  = sizeof(float) * (size_t)n * (size_t)dim;
  const size_t knn_bytes = sizeof(uint32_t) * (size_t)n * (size_t)knn_degree;
  *d_dataset = nullptr;
  *d_graph   = nullptr;
  if (base == nullptr || dim <= 0 || !degrees_ok(n, knn_degree, graph_degree) ||
      n < seekdb_cuvs_graph_min_rows(knn_degree))
    return 1;
  SKG_CUVS(cuvsStreamGet(res, &stream));
  t0 = now_ms();
  SKG_HIP(hipMalloc(&ds, ds_bytes));
  SKG_HIP(hipMemcpyAsync(ds, base, ds_bytes, hipMemcpyHostToDevice, stream));
  SKG_HIP(hipStreamSynchronize(stream));
  if (st) st->upload_ms = now_ms() - t0;
  {
    // Distance tile sized so that this phase needs no more device memory than the later
    // phases: pruning (kNN graph + pruned graph) or the finished index plus search
    // workspace (dataset + graph + 128 MB).
    const int64_t ldt = (n + 3) & ~(int64_t)3;
    const int64_t g_bytes = (int64_t)sizeof(uint32_t) * n * graph_degree;
    int64_t later         = (int64_t)knn_bytes + g_bytes;
    if ((int64_t)ds_bytes + g_bytes + (128LL << 20) > later) later = (int64_t)ds_bytes + g_bytes + (128LL << 20);
    int64_t budget = later - (int64_t)ds_bytes - kStageBytes;
    if (budget < kMinTileBytes) budget = kMinTileBytes;
    tile_rows = budget / (ldt * (int64_t)sizeof(float));
    if (tile_rows >= 64) tile_rows = tile_rows / 32 * 32;
  }
  // kNN rows go to host memory (outside the allocator of the host process) as they are made
  h_knn = mmap(nullptr, knn_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (h_knn != MAP_FAILED &&
      seekdb_cuvs_graph_knn_gemm(res, ds, n, dim, knn_degree, tile_rows, (uint32_t*)h_knn, nullptr,
                                 st) == 0) {
    // the dataset is not needed while pruning: release it, bring the kNN graph back
    t0 = now_ms();
    (void)hipFree(ds);
    ds = nullptr;
    SKG_HIP(hipMalloc(&knn, knn_bytes));
    SKG_HIP(hipMemcpyAsync(knn, h_knn, knn_bytes, hipMemcpyHostToDevice, stream));
    SKG_HIP(hipStreamSynchronize(stream));
    (void)munmap(h_knn, knn_bytes);
    h_knn = MAP_FAILED;
    if (st) st->stage_ms += now_ms() - t0;
  } else {
    fprintf(stderr, "[seekdb_cuvs] GEMM kNN unavailable, using the hipVS brute-force kNN\n");
    if (h_knn != MAP_FAILED) (void)munmap(h_knn, knn_bytes);
    h_knn = MAP_FAILED;
    rc    = seekdb_cuvs_graph_knn(res, ds, n, dim, knn_degree, kQueryChunk, &knn, st);
    if (rc) goto done;
    if (st) st->knn_path = 2;
  }
  rc = prune_graph(res, knn, (uint32_t)n, (uint32_t)knn_degree, (uint32_t)graph_degree, &g, st);
  if (rc) goto done;
  (void)hipFree(knn);  // the kNN graph is not needed after pruning
  knn = nullptr;
  rc  = merge_reverse(res, g, (uint32_t)n, (uint32_t)graph_degree, st);
  if (rc) goto done;
  if (ds == nullptr) {
    t0 = now_ms();
    SKG_HIP(hipMalloc(&ds, ds_bytes));
    SKG_HIP(hipMemcpyAsync(ds, base, ds_bytes, hipMemcpyHostToDevice, stream));
    SKG_HIP(hipStreamSynchronize(stream));
    if (st) st->stage_ms += now_ms() - t0;
  }
  t0 = now_ms();
  {
    DLManagedTensor gt, dt;
    int64_t gs[2] = {n, graph_degree}, dsz[2] = {n, dim};
    tensor2d(&gt, g, gs, kDLUInt, 32);
    tensor2d(&dt, ds, dsz, kDLFloat, 32);
    SKG_CUVS(cuvsCagraIndexFromArgs(res, L2Expanded, &gt, &dt, index));
  }
  SKG_HIP(hipStreamSynchronize(stream));
  if (st) st->wrap_ms = now_ms() - t0;
  *d_dataset = ds;
  *d_graph   = g;
  ds         = nullptr;
  g          = nullptr;
done:
  if (h_knn != MAP_FAILED) (void)munmap(h_knn, knn_bytes);
  if (knn) (void)hipFree(knn);
  if (g) (void)hipFree(g);
  if (ds) (void)hipFree(ds);
  return rc;
}
