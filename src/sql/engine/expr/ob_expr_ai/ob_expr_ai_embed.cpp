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
#include "ob_expr_ai_embed.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{
ObExprAIEmbed::ObExprAIEmbed(common::ObIAllocator &alloc)
    : ObFuncExprOperator(alloc, 
                        T_FUN_SYS_AI_EMBED, 
                        N_AI_EMBED, 
                        MORE_THAN_ZERO,
                        NOT_VALID_FOR_GENERATED_COL, 
                        NOT_ROW_DIMENSION) 
{
}

ObExprAIEmbed::~ObExprAIEmbed() 
{
}

int ObExprAIEmbed::calc_result_typeN(ObExprResType &type,
                                     ObExprResType *types_stack,
                                     int64_t param_num,
                                     common::ObExprTypeCtx &type_ctx) const 
{
  UNUSED(type_ctx);
  UNUSED(types_stack);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num > 3 || param_num < 2)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    types_stack[MODEL_IDX].set_calc_type(ObVarcharType);
    types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    // keep LOB content as LOB so that long documents are not truncated
    if (ob_is_text_tc(types_stack[CONTENT_IDX].get_type())) {
      types_stack[CONTENT_IDX].set_calc_type(ObLongTextType);
    } else {
      types_stack[CONTENT_IDX].set_calc_type(ObVarcharType);
    }
    types_stack[CONTENT_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (param_num == 3) {
      if (ob_is_integer_type(types_stack[DIM_IDX].get_type())
          || ObNullType == types_stack[DIM_IDX].get_type()) {
        types_stack[DIM_IDX].set_calc_type(ObIntType);
        types_stack[DIM_IDX].set_precision(10);
        types_stack[DIM_IDX].set_scale(0);
      } else {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be an integer, not a decimal or float");
      }
    }
    type.set_varchar();
    type.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    type.set_collation_level(CS_LEVEL_COERCIBLE);
  }
  return ret;
}

int ObExprAIEmbed::read_text_arg(const ObExpr &arg_expr, ObEvalCtx &ctx, const ObDatum &datum,
                                 ObIAllocator &allocator, ObString &str)
{
  int ret = OB_SUCCESS;
  str = datum.get_string();
  if (ob_is_text_tc(arg_expr.datum_meta_.type_)) {
    if (OB_FAIL(ObTextStringHelper::read_real_string_data(ctx.exec_ctx_, allocator, datum, arg_expr.datum_meta_,
                                                          arg_expr.obj_meta_.has_lob_header(), str))) {
      LOG_WARN("failed to read lob data", K(ret));
    }
  }
  return ret;
}

int ObExprAIEmbed::get_dimension(const ObDatum *arg_dim, int64_t &dim)
{
  int ret = OB_SUCCESS;
  dim = 0;
  if (OB_ISNULL(arg_dim) || arg_dim->is_null()) {
    // dimension is optional, the provider default is used
  } else if ((dim = arg_dim->get_int()) <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
  }
  return ret;
}

int ObExprAIEmbed::build_config(ObIAllocator &allocator, const int64_t dim, ObJsonObject *&config)
{
  int ret = OB_SUCCESS;
  config = nullptr;
  ObJsonInt *dim_json = nullptr;
  if (dim <= 0) {
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, config))) {
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, dim, dim_json))) {
  } else if (OB_FAIL(config->add("dimensions", dim_json))) {
  }
  return ret;
}

int ObExprAIEmbed::call_embedding(ObIAllocator &allocator, const ObString &model_id, const int64_t dim,
                                  ObArray<ObString> &contents, ObArray<ObString> &results)
{
  int ret = OB_SUCCESS;
  lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
  ObAIFuncExprInfo *info = nullptr;
  share::ObAiModelEndpointInfo endpoint_info;
  ObJsonObject *config = nullptr;
  query::ObIAiEndpointResolver *endpoint_resolver =
      ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
  results.reuse();
  if (OB_FAIL(build_config(allocator, dim, config))) {
  } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(allocator, model_id, info))) {
  } else if (OB_ISNULL(info) || OB_ISNULL(endpoint_resolver)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(model_id, allocator, endpoint_info))) {
  } else {
    ObAIFuncModel model(allocator, *info, endpoint_info);
    if (contents.count() == 1) {
      ObString result;
      if (OB_FAIL(model.call_dense_embedding(contents.at(0), config, result))) {
      } else if (OB_FAIL(results.push_back(result))) {
      }
    } else if (OB_FAIL(model.call_dense_embedding_vector_v2(contents, config, results))) {
    }
    if (OB_SUCC(ret) && results.count() != contents.count()) {
      ret = OB_INVALID_DATA;
      LOG_WARN("embedding result count mismatch", K(ret), K(contents.count()), K(results.count()));
    }
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res) 
{
  INIT_SUCC(ret);
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_content = nullptr;
  ObDatum *arg_dim = nullptr;
  if (expr.arg_cnt_ == 3 ? OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content, arg_dim))
                         : OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content))) {
  } else if (arg_model_id->is_null() || arg_model_id->get_string().empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id is null or empty");
  } else if (arg_content->is_null()) {
    // NULL input has no embedding
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    ObString content;
    int64_t dim = 0;
    ObArray<ObString> contents;
    ObArray<ObString> results;
    if (OB_FAIL(get_dimension(arg_dim, dim))) {
    } else if (OB_FAIL(read_text_arg(*expr.args_[CONTENT_IDX], ctx, *arg_content, temp_allocator, content))) {
    } else if (content.empty()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, input is empty");
    } else if (OB_FAIL(contents.push_back(content))) {
    } else if (OB_FAIL(call_embedding(temp_allocator, arg_model_id->get_string(), dim, contents, results))) {
    } else if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, res, results.at(0)))) {
    }
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed_batch(const ObExpr &expr, ObEvalCtx &ctx,
                                       const ObBitVector &skip, const int64_t batch_size)
{
  INIT_SUCC(ret);
  ObBitVector &eval_flags = expr.get_evaluated_flags(ctx);
  ObDatum *res_datums = expr.locate_batch_datums(ctx);
  for (int64_t i = 0; OB_SUCC(ret) && i < expr.arg_cnt_; i++) {
    if (OB_FAIL(expr.args_[i]->eval_batch(ctx, skip, batch_size))) {
      LOG_WARN("failed to eval args", K(ret), K(i));
    }
  }
  if (OB_SUCC(ret)) {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    ObDatumVector model_vec = expr.locate_param_datumvector(ctx, MODEL_IDX);
    ObDatumVector content_vec = expr.locate_param_datumvector(ctx, CONTENT_IDX);
    ObDatumVector dim_vec;
    if (expr.arg_cnt_ == 3) {
      dim_vec = expr.locate_param_datumvector(ctx, DIM_IDX);
    }
    // pending rows that need a provider call
    ObArray<int64_t> rows;
    ObArray<ObString> models;
    ObArray<int64_t> dims;
    ObArray<ObString> contents;
    for (int64_t i = 0; OB_SUCC(ret) && i < batch_size; i++) {
      if (skip.at(i) || eval_flags.at(i)) {
        continue;
      }
      const ObDatum *model_datum = model_vec.at(i);
      const ObDatum *content_datum = content_vec.at(i);
      int64_t dim = 0;
      ObString content;
      if (model_datum->is_null() || model_datum->get_string().empty()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id is null or empty");
      } else if (content_datum->is_null()) {
        res_datums[i].set_null();
        eval_flags.set(i);
      } else if (OB_FAIL(get_dimension(expr.arg_cnt_ == 3 ? dim_vec.at(i) : nullptr, dim))) {
      } else if (OB_FAIL(read_text_arg(*expr.args_[CONTENT_IDX], ctx, *content_datum, temp_allocator, content))) {
      } else if (content.empty()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, input is empty");
      } else if (OB_FAIL(rows.push_back(i))) {
      } else if (OB_FAIL(models.push_back(model_datum->get_string()))) {
      } else if (OB_FAIL(dims.push_back(dim))) {
      } else if (OB_FAIL(contents.push_back(content))) {
      }
    }
    // group rows with the same (model, dimension); each group is split into bounded chunks
    ObArray<bool> done;
    if (OB_SUCC(ret) && OB_FAIL(done.prepare_allocate(rows.count()))) {
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < rows.count(); i++) {
      done.at(i) = false;
    }
    ObEvalCtx::BatchInfoScopeGuard batch_info_guard(ctx);
    batch_info_guard.set_batch_size(batch_size);
    for (int64_t start = 0; OB_SUCC(ret) && start < rows.count(); start++) {
      if (done.at(start)) {
        continue;
      }
      int64_t next = start;
      while (OB_SUCC(ret) && next < rows.count()) {
        ObArray<int64_t> chunk_pos;
        ObArray<ObString> chunk_contents;
        ObArray<ObString> chunk_results;
        int64_t chunk_bytes = 0;
        for (int64_t j = next; OB_SUCC(ret) && j < rows.count()
             && chunk_pos.count() < MAX_BATCH_ROWS; j++) {
          if (done.at(j) || dims.at(j) != dims.at(start) || models.at(j) != models.at(start)) {
          } else if (!chunk_pos.empty() && chunk_bytes + contents.at(j).length() > MAX_BATCH_BYTES) {
            break;
          } else if (OB_FAIL(chunk_pos.push_back(j))) {
          } else if (OB_FAIL(chunk_contents.push_back(contents.at(j)))) {
          } else {
            chunk_bytes += contents.at(j).length();
            done.at(j) = true;
          }
        }
        if (OB_FAIL(ret) || chunk_pos.empty()) {
          break;
        } else if (OB_FAIL(call_embedding(temp_allocator, models.at(start), dims.at(start),
                                          chunk_contents, chunk_results))) {
          LOG_WARN("failed to call embedding", K(ret), K(chunk_contents.count()));
        }
        for (int64_t k = 0; OB_SUCC(ret) && k < chunk_pos.count(); k++) {
          const int64_t row = rows.at(chunk_pos.at(k));
          batch_info_guard.set_batch_idx(row);
          if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, res_datums[row], chunk_results.at(k)))) {
          } else {
            eval_flags.set(row);
          }
        }
        next = chunk_pos.at(chunk_pos.count() - 1) + 1;
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::cg_expr(ObExprCGCtx &expr_cg_ctx, 
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
  //   ObObj const_data;
  //   bool got_data = false;
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
  //   } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //     LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //   } else {
  //     rt_expr.extra_info_ = info;
  //   }
  // }

  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIEmbed::eval_ai_embed;
    rt_expr.eval_batch_func_ = ObExprAIEmbed::eval_ai_embed_batch;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
