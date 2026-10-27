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

#pragma once

#include <stddef.h>
#include <stdint.h>

#define SEEKDB_HIPVS_PLUGIN_ABI_VERSION 1U

#ifdef __cplusplus
extern "C" {
#endif

enum SeekdbHipvsMetric {
  SEEKDB_HIPVS_METRIC_L2 = 0,
  SEEKDB_HIPVS_METRIC_INNER_PRODUCT = 1,
};

struct SeekdbHipvsBuildRequest {
  uint32_t abi_version;
  const float *vectors;
  const int64_t *ids;
  int64_t row_count;
  int32_t dim;
  int32_t max_degree;
  int32_t ef_construction;
  int32_t metric;
  int32_t num_threads;
};

struct SeekdbHipvsBuffer {
  void *data;
  size_t size;
  void (*release)(void *data);
};

typedef uint32_t (*SeekdbHipvsGetAbiVersionFn)(void);
typedef int (*SeekdbHipvsBuildHnswFn)(const struct SeekdbHipvsBuildRequest *request,
                                      struct SeekdbHipvsBuffer *output,
                                      char *error_message,
                                      size_t error_message_size);

#ifdef __cplusplus
}
#endif
