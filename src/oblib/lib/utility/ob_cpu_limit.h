/*
 * Copyright (c) 2025 OceanBase.
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

// 作者：FDE WorkMate

#ifndef OCEANBASE_LIB_UTILITY_OB_CPU_LIMIT_H_
#define OCEANBASE_LIB_UTILITY_OB_CPU_LIMIT_H_

#include <stdint.h>

namespace oceanbase
{
namespace common
{

// CPU budget of the process.  Implemented in utility.cpp next to the cgroup memory limit and kept
// out of utility.h, which nearly every file includes.

// Returns the CPU limit of the current cgroup in milli-CPUs (CFS quota over period, the smallest
// one along the hierarchy), or 0 when no finite limit can be found.  The path arguments are
// primarily useful for inspecting a different procfs view and for tests.
int64_t get_cgroup_cpu_limit(
    const char *proc_cgroup_path = "/proc/self/cgroup",
    const char *proc_mountinfo_path = "/proc/self/mountinfo");

// Returns how many CPUs the process can keep busy at the same time: the online CPUs capped by the
// CPU affinity of the process and by the cgroup CPU limit rounded up.  Unlike get_cpu_num() it is
// evaluated on every call, so it follows limits changed while the process runs.
int64_t get_available_cpu_num();

} // namespace common
} // namespace oceanbase

#endif // OCEANBASE_LIB_UTILITY_OB_CPU_LIMIT_H_
