/*
 * Copyright (c) 2026 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Author：FDE WorkMate
//
// C ABI between seekDB and the optional GPU HNSW build plugin.
// The plugin only builds a graph for row_count vectors and returns an hnswlib
// stream whose labels are row numbers; seekDB owns validation, label mapping and
// import into VSAG.

#pragma once

#include <stddef.h>
#include <stdint.h>

#define SEEKDB_HIPVS_PLUGIN_ABI_VERSION 2U

#ifdef __cplusplus
extern "C" {
#endif

enum SeekdbHipvsMetric {
  SEEKDB_HIPVS_METRIC_L2 = 0,
  SEEKDB_HIPVS_METRIC_INNER_PRODUCT = 1,
};

enum SeekdbHipvsResult {
  SEEKDB_HIPVS_OK = 0,
  SEEKDB_HIPVS_INVALID_REQUEST = 1,
  SEEKDB_HIPVS_INSUFFICIENT_DEVICE_MEMORY = 2,
  SEEKDB_HIPVS_BUILD_ERROR = 3,
};

struct SeekdbHipvsBuildRequest {
  uint32_t abi_version;
  uint32_t struct_size;
  const float *vectors;  // host memory, row-major [row_count, dim]
  int64_t row_count;
  int32_t dim;
  int32_t max_degree;    // HNSW M
  int32_t ef_construction;
  int32_t metric;        // SeekdbHipvsMetric
  int32_t num_threads;   // host threads for hierarchy construction, 0 = auto
  int32_t reserved;
};

struct SeekdbHipvsBuildStats {
  uint32_t struct_size;
  uint32_t reserved;
  int64_t device_free_bytes;
  int64_t device_required_bytes;
  int64_t cagra_build_us;
  int64_t hierarchy_build_us;
  int64_t serialize_us;
};

struct SeekdbHipvsBuffer {
  void *data;
  size_t size;
  void (*release)(void *data);
};

typedef uint32_t (*SeekdbHipvsGetAbiVersionFn)(void);
typedef int (*SeekdbHipvsBuildHnswFn)(const struct SeekdbHipvsBuildRequest *request,
                                      struct SeekdbHipvsBuffer *output,
                                      struct SeekdbHipvsBuildStats *stats,
                                      char *error_message,
                                      size_t error_message_size);

#ifdef __cplusplus
}
#endif
