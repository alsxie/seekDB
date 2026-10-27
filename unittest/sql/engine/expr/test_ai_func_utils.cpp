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
// Unit tests for the provider protocol and validation helpers used by AI functions.

#include <gtest/gtest.h>
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
#include "share/ai_service/ob_ai_service_struct.h"
#include "common/json_type/ob_json_parse.h"

using namespace oceanbase::common;
using namespace oceanbase::share;

namespace
{

class AiFuncUtilsTest : public ::testing::Test
{
protected:
  ObJsonNode *parse(const char *text)
  {
    ObJsonNode *node = nullptr;
    EXPECT_EQ(OB_SUCCESS, ObJsonParser::get_tree(&allocator_, ObString(text), node));
    return node;
  }
  ObJsonObject *parse_object(const char *text)
  {
    ObJsonNode *node = parse(text);
    EXPECT_NE(nullptr, node);
    EXPECT_EQ(ObJsonNodeType::J_OBJECT, node->json_type());
    return static_cast<ObJsonObject *>(node);
  }
  std::string to_str(ObIJsonBase *node)
  {
    ObString str;
    EXPECT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::print_json_to_str(allocator_, node, str));
    return std::string(str.ptr(), str.length());
  }
  int endpoint_check(const char *json)
  {
    ObAiModelEndpointInfo info;
    ObJsonNode *node = parse(json);
    return info.parse_from_json_base(allocator_, ObString("ep"), *node);
  }
  ObArenaAllocator allocator_;
};

TEST_F(AiFuncUtilsTest, prompt_repeated_and_json_args)
{
  ObString rendered;
  ObJsonObject *prompt = parse_object(
      R"({"template":"{0}{0}{0}-{1}-{2}","args":["abcdefghij","x",{"k":2}]})");
  ASSERT_TRUE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(prompt));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator_, prompt, rendered));
  EXPECT_EQ(std::string("abcdefghijabcdefghijabcdefghij-x-{\"k\": 2}"),
            std::string(rendered.ptr(), rendered.length()));
}

TEST_F(AiFuncUtilsTest, prompt_out_of_range_placeholder)
{
  ObString rendered;
  ObJsonObject *prompt = parse_object(R"({"template":"{0}-{3}","args":["a"]})");
  EXPECT_NE(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator_, prompt, rendered));
}

TEST_F(AiFuncUtilsTest, ollama_header_without_key)
{
  ObArray<ObString> headers;
  ObString empty_key;
  ASSERT_EQ(OB_SUCCESS, ObOllamaUtils::get_header(allocator_, empty_key, headers));
  EXPECT_EQ(1, headers.count());
  headers.reuse();
  ASSERT_EQ(OB_SUCCESS, ObOllamaUtils::get_header(allocator_, ObString("secret"), headers));
  ASSERT_EQ(2, headers.count());
  EXPECT_TRUE(headers.at(1).prefix_match("Authorization: Bearer "));
}

TEST_F(AiFuncUtilsTest, ollama_generate_body)
{
  ObOllamaUtils::ObOllamaComplete complete;
  ObString model("qwen2.5");
  ObString system("be brief");
  ObString content("hello");
  ObJsonObject *config = parse_object(
      R"({"temperature":0.2,"max_tokens":16,"keep_alive":"1m","stream":true,"options":{"seed":7}})");
  ObJsonObject *body = nullptr;
  ASSERT_EQ(OB_SUCCESS, complete.get_body(allocator_, model, system, content, config, body));
  ASSERT_NE(nullptr, body);
  ObJsonNode *stream = body->get_value("stream");
  ASSERT_NE(nullptr, stream);
  EXPECT_EQ(ObJsonNodeType::J_BOOLEAN, stream->json_type());
  EXPECT_FALSE(stream->get_boolean());
  EXPECT_NE(nullptr, body->get_value("system"));
  EXPECT_NE(nullptr, body->get_value("keep_alive"));
  EXPECT_EQ(nullptr, body->get_value("max_tokens"));
  ObJsonNode *options = body->get_value("options");
  ASSERT_NE(nullptr, options);
  ASSERT_EQ(ObJsonNodeType::J_OBJECT, options->json_type());
  ObJsonObject *opts = static_cast<ObJsonObject *>(options);
  EXPECT_NE(nullptr, opts->get_value("num_predict"));
  EXPECT_NE(nullptr, opts->get_value("temperature"));
  EXPECT_NE(nullptr, opts->get_value("seed"));
}

TEST_F(AiFuncUtilsTest, ollama_parse_generate_and_chat)
{
  ObOllamaUtils::ObOllamaComplete complete;
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, complete.parse_output(allocator_, parse_object(R"({"response":"r1","done":true})"), result));
  EXPECT_EQ(std::string("r1"), to_str(result));
  ASSERT_EQ(OB_SUCCESS, complete.parse_output(allocator_,
      parse_object(R"({"message":{"role":"assistant","content":"r2"}})"), result));
  EXPECT_EQ(std::string("r2"), to_str(result));
  EXPECT_EQ(OB_INVALID_DATA, complete.parse_output(allocator_, parse_object(R"({"error":"x"})"), result));
}

TEST_F(AiFuncUtilsTest, openai_embedding_restores_index_order)
{
  ObOpenAIUtils::ObOpenAIEmbed embed;
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, embed.parse_output(allocator_, parse_object(
      R"({"data":[{"index":1,"embedding":[2.0]},{"index":0,"embedding":[1.0]}]})"), result));
  EXPECT_EQ(std::string("[[1.0], [2.0]]"), to_str(result));
}

TEST_F(AiFuncUtilsTest, openai_embedding_rejects_bad_index)
{
  ObOpenAIUtils::ObOpenAIEmbed embed;
  ObIJsonBase *result = nullptr;
  // duplicated
  EXPECT_EQ(OB_INVALID_DATA, embed.parse_output(allocator_, parse_object(
      R"({"data":[{"index":0,"embedding":[1]},{"index":0,"embedding":[2]}]})"), result));
  // out of range
  EXPECT_EQ(OB_INVALID_DATA, embed.parse_output(allocator_, parse_object(
      R"({"data":[{"index":2,"embedding":[1]},{"index":0,"embedding":[2]}]})"), result));
  // mixed presence
  EXPECT_EQ(OB_INVALID_DATA, embed.parse_output(allocator_, parse_object(
      R"({"data":[{"embedding":[1]},{"index":0,"embedding":[2]}]})"), result));
  // not an integer
  EXPECT_EQ(OB_INVALID_DATA, embed.parse_output(allocator_, parse_object(
      R"({"data":[{"index":0.5,"embedding":[1]}]})"), result));
}

TEST_F(AiFuncUtilsTest, embedding_vector_check)
{
  int64_t dim = 0;
  EXPECT_EQ(OB_SUCCESS, ObAIFuncUtils::check_embedding_vector(parse("[0.1, 2, -3]"), 0, dim));
  EXPECT_EQ(3, dim);
  EXPECT_EQ(OB_INVALID_ARGUMENT, ObAIFuncUtils::check_embedding_vector(parse("[0.1, 2]"), 3, dim));
  EXPECT_EQ(OB_INVALID_DATA, ObAIFuncUtils::check_embedding_vector(parse("[]"), 0, dim));
  EXPECT_EQ(OB_INVALID_DATA, ObAIFuncUtils::check_embedding_vector(parse("[\"a\"]"), 0, dim));
  EXPECT_EQ(OB_INVALID_DATA, ObAIFuncUtils::check_embedding_vector(parse("{\"a\":1}"), 0, dim));
}

TEST_F(AiFuncUtilsTest, completion_output_check)
{
  EXPECT_EQ(OB_SUCCESS, ObAIFuncUtils::check_completion_output(parse("\"ok\"")));
  EXPECT_EQ(OB_INVALID_DATA, ObAIFuncUtils::check_completion_output(parse("\"\"")));
  EXPECT_EQ(OB_INVALID_DATA, ObAIFuncUtils::check_completion_output(parse("null")));
  EXPECT_EQ(OB_INVALID_DATA, ObAIFuncUtils::check_completion_output(nullptr));
}

TEST_F(AiFuncUtilsTest, json_index_and_number)
{
  int64_t index = 0;
  double score = 0;
  EXPECT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_index_value(parse("3"), index));
  EXPECT_EQ(3, index);
  EXPECT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_index_value(parse("2.0"), index));
  EXPECT_EQ(2, index);
  EXPECT_NE(OB_SUCCESS, ObAIFuncJsonUtils::get_index_value(parse("-1"), index));
  EXPECT_NE(OB_SUCCESS, ObAIFuncJsonUtils::get_index_value(parse("1.5"), index));
  EXPECT_NE(OB_SUCCESS, ObAIFuncJsonUtils::get_index_value(parse("\"1\""), index));
  EXPECT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_number_value(parse("0.75"), score));
  EXPECT_DOUBLE_EQ(0.75, score);
  EXPECT_NE(OB_SUCCESS, ObAIFuncJsonUtils::get_number_value(parse("\"0.75\""), score));
}

TEST_F(AiFuncUtilsTest, endpoint_provider_and_concurrency)
{
  EXPECT_EQ(OB_SUCCESS, endpoint_check(
      R"({"ai_model_name":"m","url":"http://h/api/generate","access_key":"","provider":"Ollama"})"));
  EXPECT_NE(OB_SUCCESS, endpoint_check(
      R"({"ai_model_name":"m","url":"http://h/v1","access_key":"","provider":"openai"})"));
  EXPECT_EQ(OB_SUCCESS, endpoint_check(
      R"({"ai_model_name":"m","url":"http://h/v1","access_key":"k","provider":"openai","max_concurrency":1024})"));
  EXPECT_NE(OB_SUCCESS, endpoint_check(
      R"({"ai_model_name":"m","url":"http://h/v1","access_key":"k","provider":"openai","max_concurrency":0})"));
  EXPECT_NE(OB_SUCCESS, endpoint_check(
      R"({"ai_model_name":"m","url":"http://h/v1","access_key":"k","provider":"openai","max_concurrency":1025})"));
  EXPECT_NE(OB_SUCCESS, endpoint_check(
      R"({"ai_model_name":"m","url":"http://h/v1","access_key":"k","provider":"openai","max_concurrency":"8"})"));
}

} // namespace

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
