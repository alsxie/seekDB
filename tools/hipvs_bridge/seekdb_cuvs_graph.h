/* Author：FDE WorkMate
 *
 * GPU CAGRA graph construction used by the seekdb hipVS bridge.
 *
 * Every step runs on the GPU, on the stream of the given cuvsResources_t:
 *   1. exact kNN graph (L2), self edges removed, neighbours ordered nearest first:
 *      hipBLAS SGEMM distance tiles + an exact top-K selection kernel (the hipVS
 *      brute-force kNN is the fallback);
 *   2. CAGRA rank-based pruning: count the 2-hop detours of every kNN edge and keep
 *      the `graph_degree` edges with the fewest detours (the rule of
 *      cuvs::neighbors::cagra::optimize);
 *   3. CAGRA reverse-edge merge (the rule of cuvs::neighbors::cagra::optimize with
 *      guarantee_connectivity = false, which is what cuvsCagraBuild uses).
 * The resulting graph and the device copy of the dataset are wrapped into a regular
 * hipVS CAGRA index with cuvsCagraIndexFromArgs, so queries keep using the fixed
 * hipVS cuvsCagraSearch.
 */
#ifndef SEEKDB_CUVS_GRAPH_H
#define SEEKDB_CUVS_GRAPH_H
#include <stdint.h>
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  double upload_ms;    /* host -> device copy of the dataset */
  double knn_ms;       /* exact kNN graph */
  double prune_ms;     /* detour counting + rank selection */
  double reverse_ms;   /* reverse graph */
  double merge_ms;     /* reverse-edge merge */
  double wrap_ms;      /* cuvsCagraIndexFromArgs */
  uint32_t short_rows; /* rows that had fewer than graph_degree usable kNN edges (padded) */
  double stage_ms;     /* kNN rows to host and back, dataset re-upload after pruning */
  uint32_t knn_path;   /* 1 = hipBLAS GEMM + exact selection, 2 = hipVS brute force */
  uint32_t exact_rows; /* GEMM path: rows that needed the radix-select fallback */
  uint32_t tile_rows;  /* GEMM path: query rows per distance tile */
} seekdb_cuvs_graph_stats;

/* Smallest dataset the GPU graph builder accepts for a given kNN degree. */
int64_t seekdb_cuvs_graph_min_rows(int knn_degree);

/* Builds a CAGRA index for the host dataset base[n x dim] (row-major fp32, L2) into
 * `index` (created by the caller with cuvsCagraIndexCreate). knn_degree <= 256 and
 * graph_degree <= knn_degree. Returns 0 on success; the index then references two device
 * buffers, returned in *d_dataset and *d_graph, that the caller releases with
 * seekdb_cuvs_graph_release() after cuvsCagraIndexDestroy(). On failure nothing is
 * allocated and a non-zero code is returned. */
int seekdb_cuvs_graph_build_index(cuvsResources_t res, const float* base, int64_t n, int64_t dim,
                                  int knn_degree, int graph_degree, cuvsCagraIndex_t index,
                                  void** d_dataset, void** d_graph, seekdb_cuvs_graph_stats* st);

/* Exact kNN graph of a device dataset: *d_knn = device uint32 [n x knn_degree]. */
int seekdb_cuvs_graph_knn(cuvsResources_t res, const float* d_dataset, int64_t n, int64_t dim,
                          int knn_degree, int query_chunk, uint32_t** d_knn,
                          seekdb_cuvs_graph_stats* st);

/* Same graph with hipBLAS GEMM tiles + exact selection (tile_rows <= 0: automatic).
 * host_knn != NULL: rows are written to host memory [n x knn_degree] and *d_knn is unused;
 * otherwise *d_knn = device uint32 [n x knn_degree]. */
int seekdb_cuvs_graph_knn_gemm(cuvsResources_t res, const float* d_dataset, int64_t n,
                               int64_t dim, int knn_degree, int64_t tile_rows,
                               uint32_t* host_knn, uint32_t** d_knn, seekdb_cuvs_graph_stats* st);

/* CAGRA prune + reverse-edge merge of a device kNN graph [n x knn_degree] (nearest first,
 * no self edges, invalid entries = 0xFFFFFFFF): *d_graph = device uint32 [n x graph_degree]. */
int seekdb_cuvs_graph_optimize(cuvsResources_t res, const uint32_t* d_knn, int64_t n,
                               int knn_degree, int graph_degree, uint32_t** d_graph,
                               seekdb_cuvs_graph_stats* st);

/* Frees a device buffer returned by the functions above. */
void seekdb_cuvs_graph_release(void* device_ptr);

#ifdef __cplusplus
}
#endif
#endif
