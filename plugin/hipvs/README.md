# seekDB hipVS HNSW build plugin

Author：FDE WorkMate

Optional plugin that builds seekDB HNSW indexes on AMD Radeon / Instinct GPUs with hipVS (CAGRA graph, converted to a full HNSW with a GPU-built hierarchy). The default seekDB build has no ROCm dependency; every failure falls back to the existing CPU build.

## How it hooks into seekDB

1. `CREATE VECTOR INDEX` / DDL rebuild feeds rows through `ObPluginVectorIndexAdaptor::add_snap_index`. When `obvectorutil::prefer_bulk_build()` reports that the backend wants the whole dataset (GPU enabled, plugin loadable, dense FP32 `hnsw`, `l2`/`ip`, no extra-info), rows are cached instead of being added batch by batch.
2. `check_snap_hnswsq_index()` (already called before snapshot serialization by both the DDL pipeline and the async rebuild task) creates the index and calls `build_index` once with all rows.
3. `obvsag::build_index` asks the plugin for an hnswlib stream, validates it completely (header, element layout, every adjacency list, hierarchy, labels forming a permutation of the input rows, no trailing bytes), rewrites each element's row label to its vector id and imports it through VSAG `Deserialize` using a seekable in-memory stream. Elements may appear in any internal order; the stored label, not the position, identifies the row.
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

### Build against the prebuilt hipVS 1.0.0 packages

Verified in `rocm/dev-ubuntu-24.04:7.2.3-complete` without a GPU (no source changes needed). AMD PyPI ships the C++ libraries and CMake packages; unpack the wheels instead of building hipVS:

```bash
idx=https://pypi.amd.com/rocm-7.2.3/simple
pip download --no-deps -d wheels --index-url $idx \
  amd-libhipvs==1.0.0 amd-libraft==1.0.0 amd-libhipmm==4.0.0 amd-rocmds-logger==1.0.0
for w in wheels/*.whl; do unzip -q -o "$w" -d deps; done
apt-get install -y libopenblas-dev   # cuvs::cuvs references /usr/include/openblas and /usr/lib64/libopenblas.so
ln -sf /usr/lib/x86_64-linux-gnu/libopenblas.so /usr/lib64/libopenblas.so
```

The raft package also needs two header-only libraries that the wheels do not contain:
`libhipcxx` 3.0.2 (`ROCm/libhipcxx`, `cmake -S . -B b && cmake --install b --prefix $PWD/../dep-prefix`) and `cuco` 0.4.0
(`ROCm/hipCollections`, branch `release/rocmds-26.03`). hipCollections downloads its CMake helpers at configure
time; offline, a `cuco-config.cmake` that defines an INTERFACE `cuco::cuco` target on `include/` and links
`libhipcxx::libhipcxx` is sufficient. Then:

```bash
cmake -S plugin/hipvs -B build/hipvs -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_HIP_ARCHITECTURES=gfx1100 \
  -DCMAKE_CXX_COMPILER=/opt/rocm/llvm/bin/clang++ -DCMAKE_HIP_COMPILER=/opt/rocm/llvm/bin/clang++ \
  -DCMAKE_PREFIX_PATH="$PWD/dep-prefix;deps/libcuvs/lib64/cmake;deps/libraft/lib64/cmake;deps/librmm/lib64/cmake;deps/rapids_logger/lib64/cmake;/opt/rocm"
cmake --build build/hipvs -j
```

At runtime `libseekdb_hipvs.so` needs `libcuvs.so`, `libraft.so`, `librmm.so`, `librapids_logger.so`, ROCm and
`libopenblas.so.0` on the loader path (`LD_LIBRARY_PATH` of the seekdb process). If any is missing, `dlopen` fails,
seekDB logs `hipVS enabled but plugin unavailable` once and keeps building on the CPU. Without a usable GPU the
plugin returns `SEEKDB_HIPVS_BUILD_ERROR` (`hipMemGetInfo failed`) and seekDB also falls back to the CPU.

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
| disabled | rows added incrementally, CPU path unchanged |
| enabled but plugin missing or ABI mismatch | rows added incrementally, one `plugin unavailable` warning per process |
| `cosine`, sparse, SQ/BQ/HGRAPH, extra-info | CPU path unchanged |
| rows < `SEEKDB_HIPVS_MIN_ROWS` | cached rows built by CPU `build_index` |
| insufficient GPU memory, plugin error | CPU `build_index`, `hipVS HNSW build failed` warning |
| stream fails validation or VSAG import | CPU `build_index`, warning with the reason |

## Logs

- `[OBVSAG] HNSW build complete path=hipvs|cpu size dim elapsed_us`: same clock and scope for both paths.
- `[OBVSAG] hipVS HNSW stages`: `queue_wait_us`, `plugin_us`, `cagra_build_us`, `hierarchy_build_us`, `serialize_us`, `validate_us`, `import_us`, device free and estimated bytes.
- `[OBVSAG] hipVS enabled but plugin unavailable`: logged once with the `dlopen` / ABI reason.

These are INFO / WARN lines; start the observer with `--log-level INFO` to see the completion lines.

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

- `//unittest/oblib:test_hnswlib_stream`: stream validation, relabeling (including elements stored out of input order), rejection of corrupted streams and non-permutation labels, seekable buffer semantics VSAG relies on.
- `//unittest/oblib:test_hipvs_builder`: configuration parsing, early exits, mock plugin success, failure / out-of-memory / corrupt / empty / ABI mismatch fallbacks, and the concurrency bound.

## End-to-end pipeline check without a GPU

`tools/hnswlib_reference_plugin.cpp` implements the same plugin ABI with upstream hnswlib v0.8.0 (multithreaded, so elements are stored out of input order like a parallel GPU build). Pointing `SEEKDB_HIPVS_LIBRARY` at it exercises the full seekDB side: bulk snapshot collection, validation, relabeling, VSAG import, persistence and DML. `tools/hnsw_build_bench.py` drives load / build / recall / DML over SQL.

```bash
SEEKDB_HIPVS_ENABLE=1 SEEKDB_HIPVS_LIBRARY=/path/libseekdb_hnswlib_ref.so seekdb --log-level INFO ...
python3 tools/hnsw_build_bench.py load  --port P --rows 100000 --metric l2
python3 tools/hnsw_build_bench.py build --port P --metric l2
python3 tools/hnsw_build_bench.py query --port P --rows 100000 --metric l2 --ef_search 16 32 64 128
python3 tools/hnsw_build_bench.py dml   --port P --metric l2
```

Results on an 8-core x86_64 host without a GPU (100k x 128d Gaussian-mixture data, M=16, ef_construction=200, 200 queries, recall@10 against numpy brute force):

| Metric | Path | `CREATE VECTOR INDEX` (s) | ef=16 | ef=32 | ef=64 | ef=128 |
| --- | --- | --- | --- | --- | --- | --- |
| l2 | CPU (switch off) | 28.8 | 0.9760 | 0.9995 | 0.9995 | 0.9995 |
| l2 | reference plugin | 6.9 | 0.9775 | 0.9995 | 0.9995 | 0.9995 |
| ip | CPU (switch off) | 30.2 | 0.9745 | 0.9990 | 1.0000 | 1.0000 |
| ip | reference plugin | 6.7 | 0.9765 | 0.9990 | 1.0000 | 1.0000 |

The build-time gap comes from multithreaded hnswlib versus seekDB's single-threaded incremental insert; it is not a GPU number. Also verified: recall unchanged after observer restart; insert / delete visible after import; plugin failure, out-of-memory, corrupt stream, ABI mismatch, missing library and `SEEKDB_HIPVS_MIN_ROWS` all fall back to CPU with the index usable; `mysql_test/test_suite/vector_index` gives the same results with the switch off and with the reference plugin forced on every HNSW build (`SEEKDB_HIPVS_MIN_ROWS=1`).
