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

#include "lib/vector/ob_hipvs_builder.h"
#include <cstdlib>
#include <iostream>
#include <string>

using oceanbase::common::obvsag::HipvsBuildStatus;
using oceanbase::common::obvsag::try_build_hipvs_hnsw;

namespace {

void set_environment(const char *name, const char *value)
{
#if defined(_WIN32)
  _putenv_s(name, value == nullptr ? "" : value);
#else
  if (value == nullptr) {
    unsetenv(name);
  } else {
    setenv(name, value, 1);
  }
#endif
}

bool expect(bool condition, const char *message)
{
  if (!condition) {
    std::cerr << message << std::endl;
  }
  return condition;
}

} // namespace

int main()
{
  const float vectors[] = {0.0f, 1.0f};
  const int64_t ids[] = {7};
  std::string serialized;
  std::string error;

  set_environment("SEEKDB_HIPVS_ENABLE", nullptr);
  bool success = expect(
      try_build_hipvs_hnsw(vectors, ids, 1, 2, 16, 100, "l2", serialized, error) ==
          HipvsBuildStatus::NOT_REQUESTED,
      "disabled hipVS build must not be requested");
  success &= expect(serialized.empty() && error.empty(),
                    "disabled hipVS build must not produce output");

  set_environment("SEEKDB_HIPVS_ENABLE", "1");
  set_environment("SEEKDB_HIPVS_MIN_ROWS", "0");
  serialized.clear();
  error.clear();
  success &= expect(
      try_build_hipvs_hnsw(vectors, ids, 1, 2, 16, 100, "cosine", serialized, error) ==
          HipvsBuildStatus::FAILED,
      "unsupported metric must fail before plugin loading");
  success &= expect(serialized.empty() && !error.empty(),
                    "unsupported metric must return a diagnostic");

  set_environment("SEEKDB_HIPVS_ENABLE", nullptr);
  set_environment("SEEKDB_HIPVS_MIN_ROWS", nullptr);
  return success ? 0 : 1;
}
