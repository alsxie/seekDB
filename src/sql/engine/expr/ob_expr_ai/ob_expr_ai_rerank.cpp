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

#define USING_LOG_PREFIX SQL_ENG
#include "ob_expr_ai_rerank.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"
#include "lib/utility/utility.h"
#include "common/json_type/ob_json_common.h"
#include <algorithm>
 
using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{

ObExprAIRerank::ObExprAIRerank(common::ObIAllocator &alloc)
: ObFuncExprOperator(alloc, 
                    T_FUN_SYS_AI_RERANK, 
                    N_AI_RERANK, 
                    MORE_THAN_ZERO,
                    NOT_VALID_FOR_GENERATED_COL, 
                    NOT_ROW_DIMENSION) 
{
}

ObExprAIRerank::~ObExprAIRerank() 
{
}

int ObExprAIRerank::calc_result_typeN(ObExprResType &type,
                                    ObExprResType *types_stack,
                                    int64_t param_num,
                                    common::ObExprTypeCtx &type_ctx) const {
  UNUSED(type_ctx);
  UNUSED(types_stack);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num < 3 || param_num > 4)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    if (!ob_is_string_tc(types_stack[MODEL_IDX].get_type())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, model key must be string type");
    } else if (!ob_is_string_tc(types_stack[QUERY_IDX].get_type())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, query must be string type");
    } else {
      types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      types_stack[QUERY_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    }
    ObObjType in_type = types_stack[DOCUMENTS_IDX].get_type();
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObJsonExprHelper::is_valid_for_json(types_stack, DOCUMENTS_IDX, N_AI_RERANK))) {
    } else if (ob_is_string_type(in_type) && types_stack[DOCUMENTS_IDX].get_collation_type() != CS_TYPE_BINARY) {
      if (types_stack[DOCUMENTS_IDX].get_charset_type() != CHARSET_UTF8MB4) {
        types_stack[DOCUMENTS_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      }
    }
    if (OB_FAIL(ret)) {
    } else if (param_num == 4) {
      ObObjType doc_key_type = types_stack[DOC_KEY_IDX].get_type();
      if (doc_key_type == ObNullType) {
      } else if (doc_key_type == ObJsonType) {
      } else if (ob_is_string_type(doc_key_type)) {
        types_stack[DOC_KEY_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      } else {  
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, doc key must be string type");
      }
    }

    if (OB_SUCC(ret)) {
      type.set_json();
      type.set_length((ObAccuracy::DDL_DEFAULT_ACCURACY[ObJsonType]).get_length());
    }
  }
  return ret;
}

int ObExprAIRerank::eval_ai_rerank(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res) 
{
  INIT_SUCC(ret);
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_query = nullptr;
  ObDatum *arg_documents = nullptr;
  ObDatum *arg_doc_key = nullptr;
  if (OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_query, arg_documents, arg_doc_key))) {
  } else if (arg_model_id->is_null() || arg_query->is_null() || arg_documents->is_null()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, model id or query or documents is null");
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_RERANK));
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo endpoint_info;
    query::ObIAiEndpointResolver *endpoint_resolver =
        ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
    ObString model_id = arg_model_id->get_string();
    ObString query = arg_query->get_string();
    ObString doc_key;
    bool has_doc_key = false;
    ObJsonArray *document_array = nullptr;
    ObJsonArray *texts = nullptr;
    ObJsonArray *result_array = nullptr;
    ObIJsonBase *j_base = nullptr;
    ObArray<RerankItem> items;
    bool is_null_result = false;
    if (OB_FAIL(get_doc_key(expr, ctx, temp_allocator, arg_doc_key, has_doc_key, doc_key))) {
    } else if (OB_FAIL(ObJsonExprHelper::get_json_doc(expr, ctx, temp_allocator, DOCUMENTS_IDX,
                                                      j_base, is_null_result))) {
    } else if (OB_ISNULL(j_base)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (j_base->json_type() != ObJsonNodeType::J_ARRAY) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array is not array");
    } else if (FALSE_IT(document_array = static_cast<ObJsonArray *>(j_base))) {
    } else if (OB_FAIL(check_documents(document_array, has_doc_key))) {
    } else if (has_doc_key) {
      if (OB_FAIL(get_doc_array_from_documents_array_with_key(temp_allocator, document_array, doc_key, texts))) {
      }
    } else {
      texts = document_array;
    }

    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, model_id, info))) {
    } else if (OB_ISNULL(info) || OB_ISNULL(endpoint_resolver)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(ObAIFuncUtils::check_info_type_rerank(info))) {
    } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(model_id, temp_allocator, endpoint_info))) {
    } else if (OB_FAIL(rerank_in_batches(temp_allocator, *info, endpoint_info, query, texts, items))) {
    } else if (OB_FAIL(build_result(temp_allocator, document_array, has_doc_key, items, result_array))) {
    } else {
      ObString raw_str;
      if (OB_FAIL(result_array->get_raw_binary(raw_str, &temp_allocator))) {
      } else if (OB_FAIL(ObJsonExprHelper::pack_json_str_res(expr, ctx, res, raw_str))) {
      }
    }
  }
  return ret;
}

int ObExprAIRerank::get_doc_key(const ObExpr &expr, ObEvalCtx &ctx, MultimodeAlloctor &allocator,
                                const ObDatum *arg_doc_key, bool &has_doc_key, ObString &doc_key)
{
  INIT_SUCC(ret);
  has_doc_key = false;
  doc_key.reset();
  if (OB_ISNULL(arg_doc_key) || arg_doc_key->is_null()) {
    // a missing or NULL doc key means the documents are plain strings
  } else if (ObJsonType == expr.args_[DOC_KEY_IDX]->datum_meta_.type_) {
    ObIJsonBase *key_base = nullptr;
    bool is_null = false;
    if (OB_FAIL(ObJsonExprHelper::get_json_doc(expr, ctx, allocator, DOC_KEY_IDX, key_base, is_null))) {
    } else if (is_null || OB_ISNULL(key_base) || key_base->json_type() == ObJsonNodeType::J_NULL) {
    } else if (key_base->json_type() != ObJsonNodeType::J_STRING) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, doc key must be string type");
    } else {
      doc_key.assign_ptr(key_base->get_data(), static_cast<int32_t>(key_base->get_data_length()));
      has_doc_key = true;
    }
  } else {
    doc_key = arg_doc_key->get_string();
    has_doc_key = true;
  }
  if (OB_SUCC(ret) && has_doc_key && doc_key.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, doc key is empty");
  }
  return ret;
}

int ObExprAIRerank::check_documents(ObJsonArray *document_array, const bool has_doc_key)
{
  INIT_SUCC(ret);
  if (OB_ISNULL(document_array)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (document_array->element_count() == 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array is empty");
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < document_array->element_count(); i++) {
    ObJsonNode *doc_node = document_array->get_value(i);
    if (OB_ISNULL(doc_node)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (has_doc_key && doc_node->json_type() != ObJsonNodeType::J_OBJECT) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array element is not object");
    } else if (!has_doc_key && doc_node->json_type() != ObJsonNodeType::J_STRING) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array element is not string");
    }
  }
  return ret;
}

int ObExprAIRerank::get_doc_array_from_documents_array_with_key(ObIAllocator &allocator, ObJsonArray *document_object_array, ObString &doc_key, ObJsonArray *&doc_array)
{
  INIT_SUCC(ret);
  ObJsonArray *doc_array_obj = nullptr;
  if (OB_ISNULL(document_object_array)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (doc_key.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, doc_array_obj))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < document_object_array->element_count(); i++) {
      ObJsonNode *doc_node = document_object_array->get_value(i);
      ObJsonNode *doc_value_node = nullptr;
      if (OB_ISNULL(doc_node) || doc_node->json_type() != ObJsonNodeType::J_OBJECT) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array element is not object");
      } else if (OB_ISNULL(doc_value_node = static_cast<ObJsonObject *>(doc_node)->get_value(doc_key))) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, the value for given doc key is null");
      } else if (doc_value_node->json_type() != ObJsonNodeType::J_STRING) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, the value for given doc key is not string");
      } else if (OB_FAIL(doc_array_obj->append(doc_value_node))) {
      }
    }
  }
  if (OB_SUCC(ret)) {
    doc_array = doc_array_obj;
  }
  return ret;
}

int ObExprAIRerank::construct_batch_document_array(ObIAllocator &allocator, ObJsonArray *document_array, int64_t start_idx, int64_t end_idx, ObJsonArray *&batch_document_array)
{
  INIT_SUCC(ret);
  ObJsonArray *result_array = nullptr;
  if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, result_array))) {
  } else {
    for (int64_t i = start_idx; OB_SUCC(ret) && i < end_idx; i++) {
      if (OB_FAIL(result_array->append(document_array->get_value(i)))) {
      }
    }
  }
  if (OB_SUCC(ret)) {
    batch_document_array = result_array;
  }
  return ret;
}

int ObExprAIRerank::rerank_in_batches(ObIAllocator &allocator, const ObAIFuncExprInfo &info,
                                      const share::ObAiModelEndpointInfo &endpoint_info, ObString &query,
                                      ObJsonArray *texts, ObIArray<RerankItem> &items)
{
  INIT_SUCC(ret);
  ObAIFuncModel model(allocator, info, endpoint_info);
  ObArray<bool> seen;
  const int64_t doc_cnt = OB_ISNULL(texts) ? 0 : texts->element_count();
  items.reuse();
  if (OB_ISNULL(texts)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(seen.prepare_allocate(doc_cnt))) {
  } else if (OB_FAIL(items.reserve(doc_cnt))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < doc_cnt; i++) {
    seen.at(i) = false;
  }
  for (int64_t start = 0; OB_SUCC(ret) && start < doc_cnt; start += MAX_BATCH_DOCS) {
    const int64_t end = std::min(start + MAX_BATCH_DOCS, doc_cnt);
    ObJsonArray *batch_docs = nullptr;
    ObJsonArray *batch_result = nullptr;
    if (OB_FAIL(construct_batch_document_array(allocator, texts, start, end, batch_docs))) {
    } else if (OB_FAIL(model.call_rerank(query, batch_docs, batch_result))) {
      LOG_WARN("failed to call rerank", K(ret), K(start), K(end));
    } else if (OB_FAIL(collect_batch_result(batch_result, start, end - start, seen, items))) {
      LOG_WARN("invalid rerank response", K(ret), K(start), K(end));
    }
  }
  if (OB_SUCC(ret)) {
    std::sort(items.get_data(), items.get_data() + items.count());
  }
  return ret;
}

int ObExprAIRerank::collect_batch_result(ObJsonArray *batch_result, const int64_t base_idx,
                                         const int64_t batch_cnt, ObIArray<bool> &seen,
                                         ObIArray<RerankItem> &items)
{
  INIT_SUCC(ret);
  if (OB_ISNULL(batch_result)) {
    ret = OB_INVALID_DATA;
  } else if (batch_result->element_count() != batch_cnt) {
    // every document of the batch must be scored exactly once
    ret = OB_INVALID_DATA;
    LOG_WARN("rerank result count mismatch", K(ret), K(batch_cnt), K(batch_result->element_count()));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < batch_cnt; i++) {
    ObJsonNode *node = batch_result->get_value(i);
    ObJsonNode *index_node = nullptr;
    ObJsonNode *score_node = nullptr;
    int64_t index = 0;
    double score = 0;
    if (OB_ISNULL(node) || node->json_type() != ObJsonNodeType::J_OBJECT) {
      ret = OB_INVALID_DATA;
    } else if (OB_ISNULL(index_node = static_cast<ObJsonObject *>(node)->get_value(INDEX_KEY))
               || OB_ISNULL(score_node = static_cast<ObJsonObject *>(node)->get_value(SCORE_KEY))) {
      ret = OB_INVALID_DATA;
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_index_value(index_node, index))) {
      ret = OB_INVALID_DATA;
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_number_value(score_node, score))) {
      ret = OB_INVALID_DATA;
    } else if (index < 0 || index >= batch_cnt || seen.at(base_idx + index)) {
      ret = OB_INVALID_DATA;
      LOG_WARN("rerank index out of range or duplicated", K(ret), K(index), K(batch_cnt));
    } else if (OB_FAIL(items.push_back(RerankItem(base_idx + index, score)))) {
    } else {
      seen.at(base_idx + index) = true;
    }
  }
  return ret;
}

int ObExprAIRerank::build_result(ObIAllocator &allocator, ObJsonArray *document_array,
                                 const bool has_doc_key, const ObIArray<RerankItem> &items,
                                 ObJsonArray *&result)
{
  INIT_SUCC(ret);
  ObJsonArray *result_array = nullptr;
  if (OB_ISNULL(document_array)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, result_array))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < items.count(); i++) {
    const RerankItem &item = items.at(i);
    ObJsonDouble *score_node = nullptr;
    ObJsonObject *obj = nullptr;
    if (OB_ISNULL(score_node = OB_NEWx(ObJsonDouble, &allocator, item.score_))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (has_doc_key) {
      // keep every field of the original document and attach the model score
      ObJsonNode *origin = document_array->get_value(item.index_);
      if (OB_ISNULL(origin)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_ISNULL(obj = static_cast<ObJsonObject *>(origin->clone(&allocator, true)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else if (OB_FAIL(obj->add(MODEL_SCORE_KEY, score_node))) {
      }
    } else {
      ObJsonInt *index_node = nullptr;
      if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, obj))) {
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, item.index_, index_node))) {
      } else if (OB_FAIL(obj->add(INDEX_KEY, index_node))) {
      } else if (OB_FAIL(obj->add(SCORE_KEY, score_node))) {
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(result_array->append(obj))) {
    }
  }
  if (OB_SUCC(ret)) {
    result = result_array;
  }
  return ret;
}

int ObExprAIRerank::construct_config_json(ObIAllocator &allocator, int64_t top_n, int64_t return_doc, ObJsonObject *&config_json)
{
  INIT_SUCC(ret);
  ObJsonObject *config_obj = nullptr;
  ObJsonInt *top_n_obj = nullptr;
  ObJsonBoolean *return_doc_obj = nullptr;
  bool return_doc_value = true;
  if (return_doc <= 0) {
    return_doc_value = false;
  }
  if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, config_obj))) {
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_boolean(allocator, return_doc_value, return_doc_obj))) {
  } else if (OB_FAIL(config_obj->add("return_documents", return_doc_obj))) {
  } else if (top_n > 0) {
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, top_n, top_n_obj))) {
    } else if (OB_FAIL(config_obj->add("top_n", top_n_obj))) {
    }
  }
  if (OB_SUCC(ret)) {
    config_json = config_obj;
  }
  return ret;
}

int ObExprAIRerank::cg_expr(ObExprCGCtx &expr_cg_ctx, 
                        const ObRawExpr &raw_expr,
                        ObExpr &rt_expr) const 
{
  int ret = OB_SUCCESS;
  // TODO: support schema version match in plan cache for ai func
  // const ObRawExpr *model_key = raw_expr.get_param_expr(0);
  // if (OB_NOT_NULL(model_key)
  //     && (model_key->is_static_scalar_const_expr() || model_key->is_const_expr())
  //     && model_key->get_expr_type() != T_OP_GET_USER_VAR &&
  //     OB_NOT_NULL(expr_cg_ctx.schema_guard_)) {
  //   ObIAllocator *allocator = expr_cg_ctx.allocator_;
  //   ObExecContext *exec_ctx = expr_cg_ctx.session_->get_cur_exec_ctx();
  //   bool got_data = false;
  //   ObObj const_data;
  //   ObAIFuncExprInfo *info = nullptr;
  //   if (OB_ISNULL(allocator)) {
  //     ret = OB_ERR_UNEXPECTED;
  //     LOG_WARN("allocator is null", K(ret));
  //   } else if (OB_FAIL(ObSQLUtils::calc_const_or_calculable_expr(exec_ctx,
  //                                                         model_key,
  //                                                         const_data,
  //                                                         got_data,
  //                                                         *allocator))) {
  //     LOG_WARN("failed to calc offset expr", K(ret));
  //   } else if (!got_data || const_data.is_null()) {
  //   } else {
  //     ObAIFuncExprInfo *info = OB_NEWx(ObAIFuncExprInfo, allocator, *allocator, T_FUN_SYS_AI_RERANK);
  //     if (OB_ISNULL(info)) {
  //       ret = OB_ALLOCATE_MEMORY_FAILED;
  //       LOG_WARN("failed to allocate memory for ai rerank info", K(ret));
  //     } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //       LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //     } else {
  //       rt_expr.extra_info_ = info;
  //     }
  //   }
  // }
  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIRerank::eval_ai_rerank;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
