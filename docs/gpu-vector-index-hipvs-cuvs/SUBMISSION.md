# Contest submission notes

Author：FDE WorkMate

- Base: `zihaomu/seekdb` branch `amd-oceanbase-official-contest-base-v5`, commit
  `634888d9b037153fa143d2c913ba4b25503e1d62` (official AMD contest baseline V5).
- Build: unchanged, follow [README.md](README.md) (`tools/hipvs_bridge/build.sh`,
  then `./build.sh release -DOB_BUILD_CUVS=ON -DCUVS_BRIDGE_LIB=...`). The
  dependency list, CMake files and the bridge (`tools/hipvs_bridge`) are not
  modified.

## Change: shorter critical path of the lib=cuvs index build

Only `src/oblib/lib/vector/ob_vsag_adaptor.cpp` changes. The GPU work itself
(CAGRA `NN_DESCENT`, `graph_degree=128`, `intermediate_graph_degree=256`,
`nn_descent_niter=100`), the search path and the SQL behaviour are the same as
V5, so recall is unchanged.

1. Pipelined snapshot serialization. In V5, `CREATE VECTOR INDEX ... lib=cuvs`
   builds CAGRA on the GPU, writes the graph file, and only then streams ids,
   vectors and graph into the index snapshot (synchronous LOB appends). The ids
   and vectors do not depend on the GPU result, so they are now streamed while
   the GPU builds and serializes the graph on its worker thread. The graph
   section follows after the join. The snapshot format becomes `OBCUVS03`
   (graph length moved behind the vectors); `OBCUVS02` snapshots still load.
2. Asynchronous GPU warm-up. The first `lib=cuvs` rows ingested by the process
   build a CAGRA index on 2048 synthetic rows and run one search on a detached
   thread (bridge build/search/free only), so
   HIP runtime start-up and hipVS kernel loading overlap the DDL table scan
   instead of delaying the first real build. GPU builds, searches and snapshot
   load/store wait for an in-flight warm-up, so they never overlap it. Failures
   are ignored (no visible GPU); `OB_CUVS_WARMUP=0` disables it.
3. The graph is copied between the staging file and the snapshot stream in
   4 MB chunks instead of 64 KB.
4. Start-up with older bridge builds. The observer is linked with BIND_NOW.
   The hipVS runtime image ships `/seekdb_workspace/bridge/libseekdb_cuvs_bridge.so`
   (exports only `build/search/free/cagra_knn`) and puts that directory first
   on `LD_LIBRARY_PATH` and in `ld.so.conf`, so V5 dies at start-up with
   `undefined symbol: seekdb_cuvs_serialize`, whichever bridge it was linked
   against. `seekdb_cuvs_serialize`, `seekdb_cuvs_deserialize` and
   `seekdb_cuvs_cagra_knn` are now weak references. With such a bridge the
   snapshot stores ids/vectors with `graph_bytes=0` and CAGRA is rebuilt on
   the GPU from the vectors when the snapshot loads. With the bridge from
   `tools/hipvs_bridge` nothing changes (`graph_io=true`, graph persisted).
   The log lines add `graph_io` (serialize) and `load_graph` (deserialize).

The `serialized GPU CAGRA snapshot` log line keeps its fields and adds
`gpu_us` (GPU build + graph serialization), `stream_us` (ids/vectors streaming
done in parallel), `warmup_state` and `warmup_us`; `build_us` is the wall time
of the overlapped part.

## Validation (no GPU available)

No AMD GPU was available for this change. It was validated with a CPU mock of
`libseekdb_cuvs_bridge.so` (same C ABI, exact brute-force search, configurable
latency):

- The observer references exactly the same bridge symbols as V5 (`nm -D`).
  It is linked BIND_NOW, so an extra symbol would stop start-up on a bridge
  that does not export it.
- Release build with `-DOB_BUILD_CUVS=ON -DOB_BUILD_CUVS_TRACE=ON` (`-Werror`)
  succeeds; the adaptor also compiles with cuVS off and with the tracer off.
- `run_ddl_persistence_smoke.sh` and `run_gpu_smoke.sh` pass, as they do for V5
  with the same mock.
- A snapshot written by V5 (`OBCUVS02`) loads with this build without a rebuild,
  and approximate top-10 equals exact top-10.
- With an injected GPU build failure, the DDL result, the query results and the
  restart behaviour are the same as V5.
- DDL time with the mock (128-d, simulated 3 s one-off GPU start-up and 40 us
  of build time per row, one run each):

| rows    | start-up | V5     | this change |
|---------|----------|--------|-------------|
| 200,000 | 3 s      | 19.8 s | 16.8 s      |
| 200,000 | 0        | 16.7 s | 16.4 s      |
| 10,000  | 3 s      | 4.8 s  | 4.7 s       |

These numbers show that the overlap works. They are not GPU measurements.
