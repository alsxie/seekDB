# Contest submission: V7 query checkpoint

Author：FDE WorkMate

## Base and scope

- Repository: `alsxie/seekDB`; submission branch: `amd-hipvs-v7-query`.
- Parent: the previously scored `amd-hipvs-v6` commit `d4842d89`.
- Frozen ancestor: `634888d9b037153fa143d2c913ba4b25503e1d62`.
- Production code changes only `tools/hipvs_bridge/seekdb_cuvs_bridge.c`.
  Observer, SQL, snapshot format, graph construction and CPU VSAG path are unchanged.
- Same six exported C entry points. No additional dependency or network access.

## Query implementation

Each index lazily starts one joinable worker with a 32 MB pthread stack. Every eligible
single-row search submits a synchronous request; the caller waits until device work and
result copies finish. Caller pointers remain valid until completion. The existing index
mutex serializes clients, while a separate mutex/condition variable transfers request
ownership. No query result or query-dependent search state is cached.

HIP calls run on the long-lived worker instead of a fresh caller thread, avoiding repeated
HIP thread initialization. Device/pinned input and output buffers are reused, and partial
allocation failures release those buffers before the original per-call fallback. Index
release stops and joins the worker before destroying the GPU index and resources.
Concurrent handle destruction and search is not supported, as with the original ABI.

Single-row CAGRA search uses the organizer's unmodified hipVS `MULTI_CTA` implementation,
`itopk_size = max(default, 4 * topk, 1024)`, `search_width = 4`, and `HASH` mode. There is no
iteration cap. These settings apply to every eligible single query, not a test phase or
dataset. Multi-row search retains the original path. Invalid zero/negative shapes, null
query/output pointers and topk greater than index size return an error.

## Build and reproduce

Use the fixed contest build procedure:

```bash
HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/rocm \
  tools/hipvs_bridge/build.sh "$OUT/libseekdb_cuvs_bridge.so"
cmake -S . -B build_release -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DOB_ENABLE_UNITY=ON -DOB_USE_LLD=ON \
  -DOB_BUILD_CUVS=ON -DOB_BUILD_CUVS_TRACE=OFF \
  -DCUVS_BRIDGE_LIB="$OUT/libseekdb_cuvs_bridge.so"
cmake --build build_release --target seekdb
```

The bridge script compiled successfully with the supplied ROCm 7.2.4/hipVS image.
No observer source changed; development tests use the frozen V5 observer with the rebuilt
bridge. A complete observer source rebuild was not repeated for this checkpoint.
Official evaluation still builds observer from source.

Additional real-GPU regression test:

```bash
hipcc -O2 -std=c++17 -Itools/hipvs_bridge \
  tools/hipvs_bridge/test_search_worker.cpp \
  -L"$OUT" -lseekdb_cuvs_bridge -lpthread -o test_search_worker
LD_LIBRARY_PATH="$OUT:/opt/hipvs/lib:/opt/rocm/lib" \
  ./test_search_worker <serialized-cagra-index> 250000 128
```

It checks invalid arguments, snapshot shape mismatch, four load/search/free cycles,
four concurrent clients, topk 1/10/100/129/256, result ranges/distinctness, finite L2
distances, null distance output, buffer growth and batch calls after single queries.
Both 128d and 256d tests passed.

## Development measurements, 2026-11-02

Real gfx1100 GPU, ROCm 7.2.4, seekdb configured with 24 CPUs. Each dataset has 250K rows;
4096 sequential single-row SQL queries are run separately for Top-10 and Top-100.
Build is offline `CREATE VECTOR INDEX ... lib=cuvs` plus the first approximate query.
This is one paired development run, not an official score or a confidence interval.

| Dataset | Version | Build (s) | QPS@10 | QPS@100 | Recall@10 | Recall@100 |
|---|---|---:|---:|---:|---:|---:|
| SIFT 128d | Frozen V5 | 46.707 | 141.8 | 138.7 | 1.0000 | 1.0000 |
| SIFT 128d | V6 scored checkpoint | 6.101 | 148.4 | 142.8 | 0.9999 | 1.0000 |
| SIFT 128d | V7 query checkpoint | 5.984 | 1010.1 | 843.5 | 1.0000 | 1.0000 |
| NYT 256d proxy | Frozen V5 | 47.647 | 130.3 | 128.4 | 0.4089 | 0.5308 |
| NYT 256d proxy | V6 scored checkpoint | 9.100 | 138.8 | 135.9 | 0.4093 | 0.5299 |
| NYT 256d proxy | V7 query checkpoint | 8.714 | 1071.1 | 975.7 | 0.4097 | 0.5303 |

All SQL queries returned exactly k distinct IDs. NYT is angular data indexed with L2;
its absolute recall is low for both versions. It is only a regression proxy and does
not establish compliance with the hidden dataset's absolute recall thresholds.

A separate complete build/query VRAM sampling pass (about 0.4 seconds per sample including
SMI overhead) recorded V5/candidate peaks of 698/701 MB on SIFT and 821/823 MB on NYT;
ratios are 1.0043 and 1.0024, below 1.25. Sampling is not an allocation-level proof.

Official GPU smoke: all 12 cases PASS, including VSAG, filter/delete fallback, batch
queries, GPU device access and no-GPU fallback. DDL persistence smoke: PASS with graph
reload after restart, `cuvs_serve`, no rebuild, CPU reload and no stale IDs. An initial
repeat failed at startup because previous smoke block files filled the workspace;
after approved cleanup, the clean repeat passed all cases.

API serialize/deserialize tests also passed on both datasets. No persisted format or
serialized/deserialized log text changed.

## Scoring limits and next work

V6 previously earned 15 points officially. Its development build ratios were already
higher than the full-score line, so these local ratios do not explain that score.
V7 attacks query overhead; its graph builder is unchanged. Hidden recall and official
build/query ratios remain to be measured by the grader. Do not interpret these proxy
measurements as a guarantee of 20 points.

Keep the old branch and commit as the rollback reference. Further build experiments
should use another branch after obtaining the official build/query/gates breakdown.
