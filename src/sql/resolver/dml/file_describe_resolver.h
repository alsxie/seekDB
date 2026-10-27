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
//
// DuckDB style `DESCRIBE <query>` for queries over file tables. The inner
// query is resolved to obtain its output schema, which is then exposed as a
// regular result set with columns
//   column_name, column_type, null, key, default, extra.

#pragma once

#include "sql/resolver/dml/ob_select_resolver.h"

namespace oceanbase
{
namespace sql
{

class FileDescribeResolver : public ObSelectResolver
{
public:
  explicit FileDescribeResolver(ObResolverParams &params) : ObSelectResolver(params) {}
  virtual ~FileDescribeResolver() {}
  // `parse_tree` is a T_EXPLAIN node.
  virtual int resolve(const ParseNode &parse_tree) override;

  // True for `DESCRIBE/DESC <select>` whose query reads a file table.
  static bool is_file_describe(const ParseNode &parse_tree);
  static bool contains_file_table(const ParseNode *node, const int64_t depth);
private:
  int build_describe_sql(const ObSelectStmt &stmt, common::ObSqlString &sql);
  int get_column_desc(const ObSelectStmt &stmt, const ObRawExpr *expr,
                      common::ObSqlString &type_name, bool &nullable);
  int append_literal(common::ObSqlString &sql, const common::ObString &str);
};

}  // namespace sql
}  // namespace oceanbase
