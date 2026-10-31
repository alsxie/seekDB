# Contest submission notes

Author：FDE WorkMate

- Base: `zihaomu/seekdb` branch `amd-oceanbase-official-contest-base-v5`, commit
  `634888d9b037153fa143d2c913ba4b25503e1d62` (official AMD contest baseline V5).
- Scope: only the hipVS bridge (`tools/hipvs_bridge/`) changes. The observer, the SQL
  behaviour, the `lib=vsag` path, the persisted snapshot format and the CAGRA search
  parameters (itopk = max(4 x topk, 512), search_width = 4) are those of V5.
- Build: follow [README.md](README.md) (`tools/hipvs_bridge/build.sh`, then
  `./build.sh release -DOB_BUILD_CUVS=ON -DCUVS_BRIDGE_LIB=...`). The bridge script still
  needs only `HIPVS_ROOT` and `ROCM_ROOT`; it now also compiles
  `seekdb_cuvs_graph.hip` (HIP kernels, gfx1100, symbols kept internal). The library
  exports the same six `seekdb_cuvs_*` functions as V5.

## Index build

`seekdb_cuvs_build` keeps the V5 CAGRA degrees (intermediate 256, final 128) but no
longer runs NN-descent. The graph is built on the GPU in three steps:

1. Exact kNN graph (256 neighbours per row): the dataset is multiplied with itself in
   tiles of rows (hipBLAS SGEMM, key = |x|^2 - 2 q.x) and a selection kernel keeps the
   256 smallest keys of every row, the row itself excluded. A threshold taken from a
   sorted sample of the row collects about 1.5K candidates in shared memory; rows where
   the threshold misses use an exact radix select. Ties are broken by row id.
2. Pruning and reverse-edge merge in HIP kernels that follow hipVS
   `cagra::detail::graph::optimize()`: detour counting, rank-based selection of 128
   edges, reverse graph, reverse edges inserted after the first 64 forward edges.
3. Graph and dataset are wrapped with `cuvsCagraIndexFromArgs`, so search,
   serialization and deserialization are the stock hipVS CAGRA code.

Device memory stays at the V5 level: kNN rows are copied to host memory as they are
produced, the dataset is released while pruning and uploaded again for the index, and
the distance tile is sized so that the kNN step needs no more memory than the later
steps.

hipBLAS is resolved at run time (libcuvs already depends on `libhipblas.so.3`). If any
step fails, the bridge logs it and builds with `cuvsCagraBuild` (NN-descent) exactly like
V5; `SEEKDB_CUVS_GPU_GRAPH=0` forces that path.

Single-row queries reuse the search parameters and the device and pinned buffers held in
the index handle (one stream synchronisation per query). Results are identical.

## Measurements

Radeon PRO W7900D dev instance, 250K rows, offline `CREATE VECTOR INDEX ... lib=cuvs`
(timed DDL), then 4096 SQL queries each for Top-10 and Top-100. VRAM is the peak of the
whole GPU, sampled during the whole run (load, build, queries).

| data | version | build | VRAM peak | Recall@10 | Recall@100 | QPS@10 | QPS@100 |
|---|---|---|---|---|---|---|---|
| SIFT 128d | V5 | 47.6 s | 700 MB | 1.0000 | 1.0000 | 141.6 | 136.8 |
| SIFT 128d | this | 6.0 s | 705 MB | 1.0000 | 1.0000 | 146.3 | 141.3 |
| NYT 256d | V5 | 47.1 s | 822 MB | 0.4085 | 0.5305 | 126.4 | 125.1 |
| NYT 256d | this | 8.7 s | 827 MB | 0.4093 | 0.5299 | 139.2 | 138.6 |

NYT is angular data indexed with L2, so both versions have low absolute recall there; it
is used for the 256d comparison only.

The kNN graph matches the hipVS brute-force kNN (SIFT: 99.9985 % of the neighbour sets,
same nearest neighbour in every row, no self edges or duplicates). The official
`validation/run_gpu_smoke.sh` and `validation/run_ddl_persistence_smoke.sh` pass with
this bridge.
