# seekDB hipVS HNSW build plugin

Author：FDE WorkMate

Optional plugin that builds seekDB HNSW indexes on AMD Radeon / Instinct GPUs with hipVS (CAGRA graph, converted to a full HNSW with a GPU-built hierarchy). The default seekDB build has no ROCm dependency; every failure falls back to the existing CPU build.

## How it hooks into seekDB

1. `CREATE VECTOR INDEX` / DDL rebuild feeds rows through `ObPluginVectorIndexAdaptor::add_snap_index`. When `obvectorutil::prefer_bulk_build()` reports that the backend wants the whole dataset (GPU enabled, plugin loadable, dense FP32 `hnsw`, `l2`/`ip`, no extra-info), rows are cached instead of being added batch by batch.
2. `check_snap_hnswsq_index()` (already called before snapshot serialization by both the DDL pipeline and the async rebuild task) creates the index and calls `build_index` once with all rows.
3. `obvsag::build_index` asks the plugin for an hnswlib stream, validates it completely (header, element layout, every adjacency list, hierarchy, no trailing bytes), rewrites row labels to vector ids and imports it through VSAG `Deserialize` using a seekable in-memory stream.
4. Search, filtering, persistence and incremental updates keep using the existing VSAG HNSW path.

The plugin is loaded once per process and never unloaded. GPU builds are bounded by a process-wide semaphore. The plugin checks free device memory before building and serializes into a `memfd`, so nothing is written to disk.

## Requirements

- Linux x86_64, AMD GPU supported by ROCm 7.2.x
- AMD-Ecosystem/hipVS `release/rocmds-26.03` installed so its `cuvs` CMake package is discoverable
- CMake 3.31+

## Build

```bash
cmake -S plugin/hipvs -B build/hipvs -DCMAKE_BUILD_TYPE=Release
cmake --build build/hipvs -j
```

## Configuration

Read once when the observer process starts.

| Variable | Default | Meaning |
| --- | --- | --- |
| `SEEKDB_HIPVS_ENABLE` | off | `1` / `true` / `on` enables GPU builds |
| `SEEKDB_HIPVS_LIBRARY` | `libseekdb_hipvs.so` | plugin path passed to `dlopen` |
| `SEEKDB_HIPVS_MIN_ROWS` | `10000` | smaller snapshots use the CPU build |
| `SEEKDB_HIPVS_MAX_CONCURRENCY` | `1` | concurrent GPU builds per process (1-64) |

```bash
export SEEKDB_HIPVS_ENABLE=1
export SEEKDB_HIPVS_LIBRARY="$PWD/build/hipvs/libseekdb_hipvs.so"
```

## Fallback matrix

| Condition | Result |
| --- | --- |
| disabled, plugin missing, ABI mismatch | rows added incrementally, CPU path unchanged |
| `cosine`, sparse, SQ/BQ/HGRAPH, extra-info | CPU path unchanged |
| rows < `SEEKDB_HIPVS_MIN_ROWS` | cached rows built by CPU `build_index` |
| insufficient GPU memory, plugin error | CPU `build_index`, `hipVS HNSW build failed` warning |
| stream fails validation or VSAG import | CPU `build_index`, warning with the reason |

## Logs

- `[OBVSAG] HNSW build complete path=hipvs|cpu size dim elapsed_us`: same clock and scope for both paths.
- `[OBVSAG] hipVS HNSW stages`: `queue_wait_us`, `plugin_us`, `cagra_build_us`, `hierarchy_build_us`, `serialize_us`, `validate_us`, `import_us`, device free and estimated bytes.

## Benchmark and acceptance

Run on the same host with and without `SEEKDB_HIPVS_ENABLE`, 3 runs each, median reported.

- Datasets (ann-benchmarks HDF5 with ground truth): SIFT-1M (128d, l2), GIST-1M (960d, l2), one inner-product dataset.
- Measure end-to-end `CREATE VECTOR INDEX` wall time and the `HNSW build complete` `elapsed_us`; sample VRAM with `rocm-smi`.
- Recall@10 at `ef_search` 64 / 128 / 256 computed from SQL results against ground truth. Acceptance: GPU recall within 1 percentage point of CPU at the same `ef_search` (from-CAGRA graphs shift slightly along the recall/QPS curve, per hipVS docs).
- Restart the observer and re-run queries; run DML after the import; run `mysql_test/test_suite/vector_index`.

| Dataset | Rows | Dim | CPU build (s) | hipVS build (s) | Speedup | Recall@10 CPU / GPU (ef=128) | Peak VRAM |
| --- | --- | --- | --- | --- | --- | --- | --- |
| SIFT-1M | 1,000,000 | 128 | | | | | |
| GIST-1M | 1,000,000 | 960 | | | | | |

## Tests without a GPU

- `//unittest/oblib:test_hnswlib_stream`: stream validation, relabeling, rejection of corrupted streams, seekable buffer semantics VSAG relies on.
- `//unittest/oblib:test_hipvs_builder`: configuration parsing, early exits, mock plugin success, failure / out-of-memory / corrupt / empty / ABI mismatch fallbacks, and the concurrency bound.
