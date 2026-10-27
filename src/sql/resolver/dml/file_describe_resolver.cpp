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

#define USING_LOG_PREFIX SQL_RESV

#include "sql/resolver/dml/file_describe_resolver.h"
#include "sql/parser/ob_parser.h"
#include "sql/engine/basic/file_sql/file_sql_common.h"
#include "common/sql_mode/ob_sql_mode.h"

namespace oceanbase
{
using namespace common;
namespace sql
{

static const int64_t FILE_DESCRIBE_EXPLAIN_CHILD_STMT = 4;
static const int64_t FILE_DESCRIBE_MAX_DEPTH = 256;

bool FileDescribeResolver::contains_file_table(const ParseNode *node, const int64_t depth)
{
  bool found = false;
  if (OB_ISNULL(node) || depth > FILE_DESCRIBE_MAX_DEPTH) {
  } else if (T_FILE_TABLE_EXPRESSION == node->type_) {
    found = true;
  } else {
    for (int64_t i = 0; !found && i < node->num_child_; ++i) {
      found = contains_file_table(node->children_[i], depth + 1);
    }
  }
  return found;
}

bool FileDescribeResolver::is_file_describe(const ParseNode &parse_tree)
{
  const ParseNode *child = NULL;
  return T_EXPLAIN == parse_tree.type_
         && 1 == parse_tree.param_num_
         && parse_tree.num_child_ > FILE_DESCRIBE_EXPLAIN_CHILD_STMT
         && NULL != (child = parse_tree.children_[FILE_DESCRIBE_EXPLAIN_CHILD_STMT])
         && T_SELECT == child->type_
         && contains_file_table(child, 0);
}

int FileDescribeResolver::append_literal(ObSqlString &sql, const ObString &str)
{
  int ret = OB_SUCCESS;
  bool no_backslash_escapes = false;
  if (OB_NOT_NULL(session_info_)) {
    IS_NO_BACKSLASH_ESCAPES(session_info_->get_sql_mode(), no_backslash_escapes);
  }
  if (OB_FAIL(sql.append("'"))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < str.length(); ++i) {
    const char c = str.ptr()[i];
    if ('\'' == c) {
      ret = sql.append("''");
    } else if ('\\' == c && !no_backslash_escapes) {
      ret = sql.append("\\\\");
    } else {
      ret = sql.append(&c, 1);
    }
  }
  if (OB_SUCC(ret)) {
    ret = sql.append("'");
  }
  return ret;
}

int FileDescribeResolver::get_column_desc(const ObSelectStmt &stmt, const ObRawExpr *expr,
                                          ObSqlString &type_name, bool &nullable)
{
  int ret = OB_SUCCESS;
  bool done = false;
  nullable = true;
  type_name.reuse();
  if (OB_ISNULL(expr)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (expr->is_column_ref_expr()) {
    // A file column keeps the logical type and nullability inferred from the file.
    const ObColumnRefRawExpr *col = static_cast<const ObColumnRefRawExpr*>(expr);
    const TableItem *table = stmt.get_table_item_by_id(col->get_table_id());
    if (OB_NOT_NULL(table) && table->is_json_table() && OB_NOT_NULL(table->json_table_def_)
        && MulModeTableType::OB_FILE_TABLE_TYPE == table->json_table_def_->table_type_
        && !table->json_table_def_->namespace_arr_.empty()) {
      file_sql::FileScanDesc desc;
      const int64_t idx = static_cast<int64_t>(col->get_column_id()) - OB_APP_MIN_COLUMN_ID;
      if (OB_FAIL(desc.deserialize(table->json_table_def_->namespace_arr_.at(0)))) {
        LOG_WARN("failed to deserialize file scan desc", K(ret));
      } else if (idx >= 0 && idx < desc.columns_.count()) {
        const file_sql::FileColumnDesc &fc = desc.columns_.at(idx);
        const file_sql::FileColumnType t = file_sql::FileColumnType::NULL_TYPE == fc.type_
                                           ? file_sql::FileColumnType::VARCHAR : fc.type_;
        nullable = fc.nullable_;
        done = true;
        ret = type_name.append(file_sql::get_file_column_type_name(t));
      }
    }
  }
  if (OB_SUCC(ret) && !done) {
    const ObRawExprResType &rt = expr->get_result_type();
    const char *name = NULL;
    nullable = !rt.has_result_flag(NOT_NULL_FLAG);
    switch (rt.get_type_class()) {
      case ObNullTC: name = "NULL"; break;
      case ObIntTC:
        name = ObTinyIntType == rt.get_type() ? "TINYINT"
               : ObSmallIntType == rt.get_type() ? "SMALLINT"
               : (ObMediumIntType == rt.get_type() || ObInt32Type == rt.get_type()) ? "INTEGER"
               : "BIGINT";
        break;
      case ObUIntTC: name = "UBIGINT"; break;
      case ObFloatTC: name = "FLOAT"; break;
      case ObDoubleTC: name = "DOUBLE"; break;
      case ObNumberTC:
      case ObDecimalIntTC:
        ret = type_name.append_fmt("DECIMAL(%d,%d)", MAX(rt.get_precision(), 1), MAX(rt.get_scale(), 0));
        break;
      case ObDateTC:
      case ObMySQLDateTC: name = "DATE"; break;
      case ObDateTimeTC:
      case ObMySQLDateTimeTC:
        name = ObTimestampType == rt.get_type() ? "TIMESTAMP WITH TIME ZONE" : "DATETIME";
        break;
      case ObTimeTC: name = "TIME"; break;
      case ObYearTC: name = "INTEGER"; break;
      case ObJsonTC: name = "JSON"; break;
      case ObStringTC:
      case ObTextTC:
        name = CS_TYPE_BINARY == rt.get_collation_type() ? "BLOB" : "VARCHAR";
        break;
      default: name = "VARCHAR"; break;
    }
    if (OB_SUCC(ret) && NULL != name) {
      ret = type_name.append(name);
    }
  }
  return ret;
}

int FileDescribeResolver::build_describe_sql(const ObSelectStmt &stmt, ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  ObSqlString type_name;
  bool nullable = true;
  if (OB_UNLIKELY(stmt.get_select_item_size() <= 0)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(sql.append("SELECT column_name, column_type, `null`, `key`, `default`, extra"
                                " FROM ("))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < stmt.get_select_item_size(); ++i) {
    const SelectItem &item = stmt.get_select_item(i);
    if (OB_FAIL(get_column_desc(stmt, item.expr_, type_name, nullable))) {
      LOG_WARN("failed to describe column", K(ret), K(i));
    } else if (OB_FAIL(sql.append_fmt("%sSELECT %ld AS __file_desc_ord, ", 0 == i ? "" : " UNION ALL ", i))) {
    } else if (OB_FAIL(append_literal(sql, item.alias_name_))) {
    } else if (OB_FAIL(sql.append(" AS column_name, "))) {
    } else if (OB_FAIL(append_literal(sql, type_name.string()))) {
    } else if (OB_FAIL(sql.append_fmt(" AS column_type, '%s' AS `null`, NULL AS `key`,"
                                      " NULL AS `default`, NULL AS extra",
                                      nullable ? "YES" : "NO"))) {
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(sql.append(") __file_desc ORDER BY __file_desc_ord"))) {
  }
  return ret;
}

int FileDescribeResolver::resolve(const ParseNode &parse_tree)
{
  int ret = OB_SUCCESS;
  const ParseNode *child = NULL;
  ObSqlString sql;
  if (OB_UNLIKELY(!is_file_describe(parse_tree))
      || OB_ISNULL(child = parse_tree.children_[FILE_DESCRIBE_EXPLAIN_CHILD_STMT])) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("not a file describe statement", K(ret));
  } else if (OB_ISNULL(session_info_) || OB_ISNULL(params_.allocator_)) {
    ret = OB_NOT_INIT;
  } else {
    HEAP_VAR(ObSelectResolver, inner, params_) {
      const ObSelectStmt *inner_stmt = NULL;
      if (OB_FAIL(inner.resolve(*child))) {
        LOG_WARN("failed to resolve described query", K(ret));
      } else if (OB_ISNULL(inner_stmt = inner.get_select_stmt())) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(build_describe_sql(*inner_stmt, sql))) {
        LOG_WARN("failed to build describe sql", K(ret));
      }
    }
  }
  if (OB_SUCC(ret)) {
    ParseResult result;
    ObParser parser(*params_.allocator_, session_info_->get_sql_mode());
    ObString sql_str;
    if (OB_FAIL(ob_write_string(*params_.allocator_, sql.string(), sql_str))) {
    } else if (OB_FAIL(parser.parse(sql_str, result))) {
      LOG_WARN("failed to parse describe sql", K(ret), K(sql_str));
    } else if (OB_ISNULL(result.result_tree_) || 1 != result.result_tree_->num_child_
               || OB_ISNULL(result.result_tree_->children_)
               || OB_ISNULL(result.result_tree_->children_[0])) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(ObSelectResolver::resolve(*result.result_tree_->children_[0]))) {
      LOG_WARN("failed to resolve describe sql", K(ret));
    }
  }
  return ret;
}

}  // namespace sql
}  // namespace oceanbase
