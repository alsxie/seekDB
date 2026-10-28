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

#pragma once

#include "lib/container/ob_iarray.h"
#include "lib/string/ob_string.h"
#include "share/ai_service/ob_ai_service_struct.h"
#include "share/schema/ob_column_schema.h"
#include "share/schema/ob_table_schema.h"
#include "sql/parser/parse_node.h"

namespace oceanbase
{
namespace sql
{

// Resolves "col type AI COLUMN (AI_FUNC(...)) [WITH (...)]" column definitions.
class AiColumnResolver
{
public:
  // Returns the T_AI_COLUMN_DEFINITION attribute of a T_COLUMN_DEFINITION, or NULL.
  static const ParseNode *get_ai_column_node(const ParseNode *column_def);
  // Validates the AI column expression and options of an already resolved result column and
  // fills %def. Dependency column names are returned in %dep_names.
  static int resolve_definition(common::ObIAllocator &allocator,
                                const ParseNode &ai_node,
                                share::schema::ObColumnSchemaV2 &column,
                                share::AiColumnDef &def,
                                common::ObIArray<common::ObString> &dep_names);
  // Every dependency must be an existing, non AI, non hidden column of %table, or one of
  // %new_columns (columns added by the same statement).
  static int check_dependencies(const share::schema::ObTableSchema &table,
                                const common::ObIArray<share::schema::ObColumnSchemaV2 *> &new_columns,
                                const share::AiColumnDef &def,
                                const common::ObIArray<common::ObString> &dep_names);
  // Builds the hidden state / request / spec columns of one AI column. Column ids are left to
  // the caller.
  static int build_hidden_columns(common::ObIAllocator &allocator,
                                  const share::schema::ObColumnSchemaV2 &result_column,
                                  share::AiColumnDef &def,
                                  common::ObIArray<share::schema::ObColumnSchemaV2> &hidden_columns);
private:
  static int collect_column_refs(const ParseNode *node,
                                 common::ObIArray<common::ObString> &names);
  static int resolve_options(const ParseNode *options, share::AiColumnDef &def);
  static int get_vector_dim(const share::schema::ObColumnSchemaV2 &column, int64_t &dim);
};

} // namespace sql
} // namespace oceanbase
