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
// Common includes and small helpers shared by the expression unit tests.

#pragma once

#include <string>
#include <gtest/gtest.h>
#include "lib/allocator/page_arena.h"
#include "lib/string/ob_string.h"
#include "lib/string/ob_string_buffer.h"
#include "common/json_type/ob_json_tree.h"
#include "common/json_type/ob_json_base.h"
#include "common/json_type/ob_json_parse.h"

namespace oceanbase
{
namespace unittest
{

// parse json text into a mutable json tree, nullptr on failure
inline common::ObJsonNode *parse_json(common::ObIAllocator &allocator, const char *text)
{
  common::ObJsonNode *node = nullptr;
  if (OB_SUCCESS != common::ObJsonParser::get_tree(&allocator, common::ObString(text), node)) {
    node = nullptr;
  }
  return node;
}

inline common::ObJsonObject *parse_json_object(common::ObIAllocator &allocator, const char *text)
{
  common::ObJsonNode *node = parse_json(allocator, text);
  return (nullptr != node && common::ObJsonNodeType::J_OBJECT == node->json_type())
         ? static_cast<common::ObJsonObject *>(node) : nullptr;
}

// print a json value; strings are printed without quotes
inline std::string json_to_string(common::ObIAllocator &allocator, common::ObIJsonBase *node)
{
  std::string result;
  common::ObStringBuffer buf(&allocator);
  if (nullptr != node && OB_SUCCESS == node->print(buf, false)) {
    result.assign(buf.ptr(), buf.length());
  }
  return result;
}

inline std::string to_std_string(const common::ObString &str)
{
  return std::string(str.ptr(), str.length());
}

} // namespace unittest
} // namespace oceanbase
