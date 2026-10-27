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

#include <stdint.h>
#include <string>

namespace oceanbase {
namespace common {
namespace obvsag {

enum class HipvsBuildStatus {
  NOT_REQUESTED,
  BUILT,
  FAILED,
};

HipvsBuildStatus try_build_hipvs_hnsw(const float *vectors,
                                      const int64_t *ids,
                                      int64_t row_count,
                                      int dim,
                                      int max_degree,
                                      int ef_construction,
                                      const char *metric,
                                      std::string &serialized_index,
                                      std::string &error_message);

} // namespace obvsag
} // namespace common
} // namespace oceanbase
