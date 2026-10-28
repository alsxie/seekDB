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
#include "sql/resolver/ddl/ob_ai_column_resolver.h"
#include "query/vector/ob_vector_index_util.h"
#include "lib/utility/ob_print_utils.h"

namespace oceanbase
{
using namespace common;
using namespace share;
using namespace share::schema;
namespace sql
{

const ParseNode *AiColumnResolver::get_ai_column_node(const ParseNode *column_def)
{
  const ParseNode *ai_node = NULL;
  const ParseNode *attrs = NULL;
  if (NULL != column_def && T_COLUMN_DEFINITION == column_def->type_
      && column_def->num_child_ > 2 && NULL != (attrs = column_def->children_[2])) {
    for (int64_t i = 0; NULL == ai_node && i < attrs->num_child_; ++i) {
      if (NULL != attrs->children_[i] && T_AI_COLUMN_DEFINITION == attrs->children_[i]->type_) {
        ai_node = attrs->children_[i];
      }
    }
  }
  return ai_node;
}

int AiColumnResolver::collect_column_refs(const ParseNode *node, ObIArray<ObString> &names)
{
  int ret = OB_SUCCESS;
  if (NULL == node) {
  } else if (T_COLUMN_REF == node->type_) {
    const ParseNode *col = node->num_child_ >= 3 ? node->children_[2] : NULL;
    if (OB_ISNULL(col) || T_STAR == col->type_ || NULL != node->children_[0]) {
      // database qualified or star references can not name a column of the same row
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN expression with qualified or star column reference");
    } else {
      ObString name(static_cast<int32_t>(col->str_len_), col->str_value_);
      bool found = false;
      for (int64_t i = 0; !found && i < names.count(); ++i) {
        found = 0 == names.at(i).case_compare(name);
      }
      if (!found && OB_FAIL(names.push_back(name))) {
      }
    }
  } else if (T_SELECT == node->type_) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "subquery in AI COLUMN expression");
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < node->num_child_; ++i) {
      ret = collect_column_refs(node->children_[i], names);
    }
  }
  return ret;
}

int AiColumnResolver::resolve_options(const ParseNode *options, AiColumnDef &def)
{
  int ret = OB_SUCCESS;
  def.sync_mode_ = ObString::make_string(AiColumnUtil::SYNC_MODE_ASYNC);
  def.max_retries_ = AiColumnUtil::DEFAULT_MAX_RETRIES;
  for (int64_t i = 0; OB_SUCC(ret) && NULL != options && i < options->num_child_; ++i) {
    const ParseNode *opt = options->children_[i];
    const ParseNode *name_node = NULL;
    const ParseNode *value = NULL;
    if (OB_ISNULL(opt) || opt->num_child_ < 2 || OB_ISNULL(name_node = opt->children_[0])
        || OB_ISNULL(value = opt->children_[1])) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      ObString name(static_cast<int32_t>(name_node->str_len_), name_node->str_value_);
      if (0 == name.case_compare("sync_mode")) {
        ObString mode(static_cast<int32_t>(value->str_len_), value->str_value_);
        if ((T_VARCHAR != value->type_ && T_CHAR != value->type_ && T_IDENT != value->type_)
            || 0 != mode.case_compare(AiColumnUtil::SYNC_MODE_ASYNC)) {
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN sync_mode other than 'async'");
        }
      } else if (0 == name.case_compare("max_retries")) {
        if (T_INT != value->type_ || value->value_ < 0
            || value->value_ > AiColumnUtil::MAX_MAX_RETRIES) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "AI COLUMN max_retries, expect integer in [0, 100]");
        } else {
          def.max_retries_ = value->value_;
        }
      } else {
        ret = OB_NOT_SUPPORTED;
        LOG_WARN("unknown ai column option", K(ret), K(name));
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "unknown AI COLUMN option");
      }
    }
  }
  return ret;
}

int AiColumnResolver::get_vector_dim(const ObColumnSchemaV2 &column, int64_t &dim)
{
  int ret = OB_SUCCESS;
  dim = 0;
  if (ob_is_collection_sql_type(column.get_data_type())
      && OB_FAIL(ObVectorIndexUtil::get_vector_dim_from_extend_type_info(
          column.get_extended_type_info(), dim))) {
    // not a dense vector column, no dimension to check
    dim = 0;
    ret = OB_SUCCESS;
  }
  return ret;
}

int AiColumnResolver::resolve_definition(ObIAllocator &allocator,
                                         const ParseNode &ai_node,
                                         ObColumnSchemaV2 &column,
                                         AiColumnDef &def,
                                         ObIArray<ObString> &dep_names)
{
  int ret = OB_SUCCESS;
  const ParseNode *expr = ai_node.num_child_ > 0 ? ai_node.children_[0] : NULL;
  const ParseNode *options = ai_node.num_child_ > 1 ? ai_node.children_[1] : NULL;
  const ParseNode *func_name_node = NULL;
  const ParseNode *arg_list = NULL;
  ObString func_name;
  ObSEArray<ObString, 4> arg_texts;
  ObString parsed_name;
  def.reset();
  dep_names.reset();
  if (OB_ISNULL(expr) || OB_ISNULL(expr->str_value_) || expr->str_len_ <= 0) {
    ret = OB_ERR_UNEXPECTED;
  } else if (T_FUN_SYS != expr->type_ || expr->num_child_ < 2
             || OB_ISNULL(func_name_node = expr->children_[0])
             || OB_ISNULL(arg_list = expr->children_[1])) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN expression other than AI_COMPLETE / AI_EMBED call");
  } else {
    func_name.assign_ptr(func_name_node->str_value_, static_cast<int32_t>(func_name_node->str_len_));
    const ParseNode *model_node = arg_list->num_child_ > 0 ? arg_list->children_[0] : NULL;
    const bool is_complete = 0 == func_name.case_compare(AiColumnUtil::FUNC_AI_COMPLETE);
    const bool is_embed = 0 == func_name.case_compare(AiColumnUtil::FUNC_AI_EMBED);
    if (!is_complete && !is_embed) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN expression other than AI_COMPLETE / AI_EMBED call");
    } else if (arg_list->num_child_ < 2 || arg_list->num_child_ > 3) {
      ret = OB_ERR_PARAM_SIZE;
      LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name.length(), func_name.ptr());
    } else if (OB_ISNULL(model_node)
               || (T_VARCHAR != model_node->type_ && T_CHAR != model_node->type_)) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN model name other than a string literal");
    } else if (column.is_autoincrement() || column.is_generated_column()
               || column.is_rowkey_column() || !column.is_nullable()) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN that is not a plain nullable column");
    } else if (is_embed && 3 == arg_list->num_child_) {
      // the requested embedding dimension must agree with the declared vector dimension
      const ParseNode *dim_node = arg_list->children_[2];
      int64_t col_dim = 0;
      if (OB_FAIL(get_vector_dim(column, col_dim))) {
      } else if (NULL != dim_node && T_INT == dim_node->type_ && col_dim > 0
                 && dim_node->value_ != col_dim) {
        ret = OB_ERR_INVALID_VECTOR_DIM;
        LOG_USER_ERROR(OB_ERR_INVALID_VECTOR_DIM, static_cast<uint32_t>(col_dim),
                       static_cast<uint32_t>(dim_node->value_));
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(collect_column_refs(arg_list, dep_names))) {
    } else if (dep_names.count() > AiColumnMeta::MAX_DEPENDENCY_COUNT) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN with more than 64 dependency columns");
    } else if (OB_FAIL(resolve_options(options, def))) {
    } else if (OB_FAIL(ob_write_string(allocator,
                                       ObString(static_cast<int32_t>(expr->str_len_), expr->str_value_),
                                       def.original_expr_))) {
    } else if (OB_FAIL(AiColumnUtil::split_call_args(def.original_expr_, parsed_name, arg_texts))) {
      LOG_WARN("fail to split ai column expression", K(ret), K(def.original_expr_));
    } else if (OB_UNLIKELY(arg_texts.count() != arg_list->num_child_)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("argument count mismatch", K(ret), K(arg_texts), K(arg_list->num_child_));
    } else if (OB_FAIL(AiColumnUtil::build_normalized_expr(
        allocator, ObString::make_string(is_complete ? AiColumnUtil::FUNC_AI_COMPLETE
                                                     : AiColumnUtil::FUNC_AI_EMBED),
        arg_texts, column.get_data_type(), def.normalized_expr_))) {
    } else if (OB_FAIL(ob_write_string(allocator, column.get_column_name_str(), def.column_name_))) {
    } else if (OB_FAIL(ob_write_string(allocator,
                                       ObString::make_string(is_complete ? AiColumnUtil::FUNC_AI_COMPLETE
                                                                         : AiColumnUtil::FUNC_AI_EMBED),
                                       def.function_name_))) {
    } else if (OB_FAIL(ob_write_string(allocator,
                                       ObString(static_cast<int32_t>(model_node->str_len_),
                                                model_node->str_value_),
                                       def.model_name_))) {
    } else if (OB_FAIL(ob_write_string(allocator,
                                       ObString::make_string(ob_obj_type_str(column.get_data_type())),
                                       def.result_type_))) {
    } else {
      ObSqlString deps;
      for (int64_t i = 0; OB_SUCC(ret) && i < dep_names.count(); ++i) {
        ret = deps.append_fmt("%s%.*s", 0 == i ? "" : ",", dep_names.at(i).length(),
                              dep_names.at(i).ptr());
      }
      if (OB_SUCC(ret) && OB_FAIL(ob_write_string(allocator, deps.string(), def.dependency_names_))) {
      } else {
        def.spec_version_ = AiColumnUtil::calc_spec_version(def.normalized_expr_);
        column.add_column_flag(AI_RESULT_COLUMN_FLAG);
      }
    }
  }
  return ret;
}

int AiColumnResolver::check_dependencies(const ObTableSchema &table,
                                         const ObIArray<ObColumnSchemaV2 *> &new_columns,
                                         const AiColumnDef &def,
                                         const ObIArray<ObString> &dep_names)
{
  int ret = OB_SUCCESS;
  if (dep_names.count() <= 0) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN expression without any column reference");
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < dep_names.count(); ++i) {
    const ObString &name = dep_names.at(i);
    const ObColumnSchemaV2 *dep = table.get_column_schema(name);
    for (int64_t j = 0; NULL == dep && j < new_columns.count(); ++j) {
      if (NULL != new_columns.at(j)
          && 0 == new_columns.at(j)->get_column_name_str().case_compare(name)) {
        dep = new_columns.at(j);
      }
    }
    if (0 == name.case_compare(def.column_name_)) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN depending on itself");
    } else if (OB_ISNULL(dep) || dep->is_hidden()) {
      ret = OB_ERR_BAD_FIELD_ERROR;
      LOG_USER_ERROR(OB_ERR_BAD_FIELD_ERROR, name.length(), name.ptr(),
                     table.get_table_name_str().length(), table.get_table_name_str().ptr());
    } else if (dep->is_ai_result_column()) {
      // chained AI columns are not supported; reported with the generic message
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("AI COLUMN depending on another AI COLUMN", K(ret), K(dep->get_column_name_str()));
    }
  }
  return ret;
}

int AiColumnResolver::build_hidden_columns(ObIAllocator &allocator,
                                           const ObColumnSchemaV2 &result_column,
                                           AiColumnDef &def,
                                           ObIArray<ObColumnSchemaV2> &hidden_columns)
{
  int ret = OB_SUCCESS;
  ObString *names[AiColumnUtil::HIDDEN_KIND_COUNT] = {&def.state_column_name_,
                                                      &def.request_column_name_,
                                                      &def.result_spec_column_name_};
  const int64_t defaults[AiColumnUtil::HIDDEN_KIND_COUNT] = {AI_ROW_STATE_PENDING, 0, 0};
  hidden_columns.reset();
  for (int64_t i = 0; OB_SUCC(ret) && i < AiColumnUtil::HIDDEN_KIND_COUNT; ++i) {
    ObColumnSchemaV2 col;
    ObObj default_value;
    default_value.set_int(defaults[i]);
    if (OB_FAIL(AiColumnUtil::gen_hidden_column_name(allocator, result_column.get_column_name_str(),
                                                     static_cast<AiColumnUtil::HiddenKind>(i),
                                                     *names[i]))) {
    } else if (OB_FAIL(col.set_column_name(*names[i]))) {
    } else {
      col.set_table_id(result_column.get_table_id());
      col.set_data_type(ObIntType);
      col.set_collation_type(CS_TYPE_BINARY);
      col.set_charset_type(CHARSET_BINARY);
      col.set_accuracy(ObAccuracy::DDL_DEFAULT_ACCURACY[ObIntType]);
      col.set_nullable(false);
      col.set_is_hidden(true);
      col.set_rowkey_position(0);
      col.add_column_flag(AI_HIDDEN_COLUMN_FLAG);
      if (OB_FAIL(col.set_orig_default_value(default_value))) {
      } else if (OB_FAIL(col.set_cur_default_value(default_value, false))) {
      } else if (OB_FAIL(hidden_columns.push_back(col))) {
      }
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
