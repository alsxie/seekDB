# seekDB hipVS HNSW build plugin

This optional plugin uses AMD hipVS on Radeon/Instinct GPUs to build a CAGRA graph and convert it into a full GPU-hierarchy HNSW index. It rewrites hnswlib external labels to seekDB vector IDs and returns the serialized index through a versioned ABI. seekDB then validates and imports the stream into its existing VSAG HNSW handler, so search, filtering, persistence, and incremental updates continue through the established path.

The default seekDB build has no ROCm dependency. Missing libraries, unsupported metrics, small datasets, plugin failures, or an incompatible serialized stream all fall back to the existing CPU build.

## Requirements

- Linux x86_64
- AMD Radeon or Instinct GPU supported by ROCm
- ROCm 7.2.3
- AMD-Ecosystem/hipVS `release/rocmds-26.03`
- CMake 3.31+

## Build

Install hipVS so its `cuvs` CMake package is discoverable, then run:

```bash
cmake -S plugin/hipvs -B build/hipvs -DCMAKE_BUILD_TYPE=Release
cmake --build build/hipvs -j
```

## Enable

```bash
export SEEKDB_HIPVS_ENABLE=1
export SEEKDB_HIPVS_LIBRARY="$PWD/build/hipvs/libseekdb_hipvs.so"
export SEEKDB_HIPVS_MIN_ROWS=10000
```

Acceleration applies to dense FP32 HNSW indexes using `l2` or `ip`, with no extra-info payload. Other index forms use the CPU implementation.

Run the same bulk-load or `CREATE VECTOR INDEX` workload once without `SEEKDB_HIPVS_ENABLE` and once with it. The seekDB log reports `CPU HNSW build complete` or `hipVS HNSW build success` with elapsed microseconds. Compare those timings and run the existing vector-index query suite to verify recall and persistence.
