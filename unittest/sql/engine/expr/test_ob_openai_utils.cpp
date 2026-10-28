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
// Provider protocol tests: OpenAI completion / embedding (validation, out of order
// recovery), Ollama native request / response, endpoint provider checks.

#include <gtest/gtest.h>
#include "ob_ai_func_test_utils.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
#include "share/ai_service/ob_ai_service_struct.h"

using namespace oceanbase::common;
using namespace oceanbase::share;
using namespace oceanbase::unittest;

class ObOpenAIUtilsTest : public ::testing::Test
{
protected:
  ObJsonNode *json(const char *text)
  {
    ObJsonNode *node = parse_json(allocator_, text);
    EXPECT_NE(nullptr, node) << text;
    return node;
  }
  ObJsonObject *object(const char *text)
  {
    ObJsonObject *obj = parse_json_object(allocator_, text);
    EXPECT_NE(nullptr, obj) << text;
    return obj;
  }
  std::string str(ObIJsonBase *node) { return json_to_string(allocator_, node); }
  int endpoint_check(const char *text)
  {
    ObAiModelEndpointInfo info;
    return info.parse_from_json_base(allocator_, ObString("ep"), *json(text));
  }
  ObArenaAllocator allocator_;
};

// ---------------------------------------------------------------- OpenAI completion

TEST_F(ObOpenAIUtilsTest, test_complete_get_header)
{
  ObArray<ObString> headers;
  ObString api_key("sk-1234567890");
  ObOpenAIUtils::ObOpenAIComplete completion;
  ASSERT_EQ(OB_SUCCESS, completion.get_header(allocator_, api_key, headers));
  ASSERT_EQ(2, headers.count());
  EXPECT_EQ(ObString("Authorization: Bearer sk-1234567890"), headers[0]);
  EXPECT_EQ(ObString("Content-Type: application/json"), headers[1]);
}

TEST_F(ObOpenAIUtilsTest, test_complete_get_body)
{
  ObString model("gpt-4o-mini");
  ObString prompt("You are a helpful assistant.");
  ObString content("oceanbase is a distributed database");
  ObOpenAIUtils::ObOpenAIComplete completion;
  ObJsonObject *body = nullptr;
  ObJsonObject *config = object(R"J({"temperature":0.2,"max_tokens":16})J");
  ASSERT_EQ(OB_SUCCESS, completion.get_body(allocator_, model, prompt, content, config, body));
  EXPECT_EQ("gpt-4o-mini", str(body->get_value("model")));
  ObJsonArray *messages = static_cast<ObJsonArray *>(body->get_value("messages"));
  ASSERT_EQ(2, messages->element_count());
  EXPECT_EQ(to_std_string(prompt), str(static_cast<ObJsonObject *>(messages->get_value(0))->get_value("content")));
  EXPECT_EQ(to_std_string(content), str(static_cast<ObJsonObject *>(messages->get_value(1))->get_value("content")));
  // generation parameters are passed through
  EXPECT_EQ("0.2", str(body->get_value("temperature")));
  EXPECT_EQ("16", str(body->get_value("max_tokens")));
}

TEST_F(ObOpenAIUtilsTest, test_complete_parse_output)
{
  ObOpenAIUtils::ObOpenAIComplete completion;
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, completion.parse_output(allocator_, object(
      R"J({"choices":[{"message":{"role":"assistant","content":"天气真好。"},"finish_reason":"stop","index":0}],
          "object":"chat.completion","model":"qwen-plus"})J"), result));
  EXPECT_EQ("天气真好。", str(result));
}

TEST_F(ObOpenAIUtilsTest, test_complete_parse_output_empty)
{
  ObOpenAIUtils::ObOpenAIComplete completion;
  ObIJsonBase *result = nullptr;
  EXPECT_EQ(OB_INVALID_DATA, completion.parse_output(allocator_, object(
      R"J({"choices":[{"message":{"role":"assistant"},"index":0}]})J"), result));
  EXPECT_EQ(OB_INVALID_DATA, completion.parse_output(allocator_, object(R"J({"choices":[]})J"), result));
  EXPECT_EQ(OB_INVALID_DATA, completion.parse_output(allocator_, object(R"J({})J"), result));
}

TEST_F(ObOpenAIUtilsTest, test_completion_output_check)
{
  EXPECT_EQ(OB_SUCCESS, ObAIFuncUtils::check_completion_output(json("\"ok\"")));
  EXPECT_EQ(OB_INVALID_DATA, ObAIFuncUtils::check_completion_output(json("\"\"")));
  EXPECT_EQ(OB_INVALID_DATA, ObAIFuncUtils::check_completion_output(json("null")));
  // structured (json format) answers are kept as they are
  EXPECT_EQ(OB_SUCCESS, ObAIFuncUtils::check_completion_output(json("{\"k\":1}")));
  EXPECT_EQ(OB_INVALID_DATA, ObAIFuncUtils::check_completion_output(nullptr));
}

// ---------------------------------------------------------------- OpenAI embedding

TEST_F(ObOpenAIUtilsTest, test_embedding_get_header)
{
  ObArray<ObString> headers;
  ObString api_key("sk-1234567890");
  ObOpenAIUtils::ObOpenAIEmbed embedding;
  ASSERT_EQ(OB_SUCCESS, embedding.get_header(allocator_, api_key, headers));
  ASSERT_EQ(2, headers.count());
  EXPECT_EQ(ObString("Authorization: Bearer sk-1234567890"), headers[0]);
  EXPECT_EQ(ObString("Content-Type: application/json"), headers[1]);
}

TEST_F(ObOpenAIUtilsTest, test_embedding_get_body)
{
  ObString model("text-embedding-3-small");
  ObArray<ObString> inputs;
  ASSERT_EQ(OB_SUCCESS, inputs.push_back(ObString("oceanbase is a distributed database")));
  ASSERT_EQ(OB_SUCCESS, inputs.push_back(ObString("seekdb")));
  ObOpenAIUtils::ObOpenAIEmbed embedding;
  ObJsonObject *body = nullptr;
  ObJsonObject *config = object(R"J({"dimensions":8})J");
  ASSERT_EQ(OB_SUCCESS, embedding.get_body(allocator_, model, inputs, config, body));
  EXPECT_EQ(to_std_string(model), str(body->get_value("model")));
  ObJsonArray *input = static_cast<ObJsonArray *>(body->get_value("input"));
  ASSERT_EQ(2, input->element_count());
  EXPECT_EQ("seekdb", str(input->get_value(1)));
  EXPECT_EQ("8", str(body->get_value("dimensions")));
}

TEST_F(ObOpenAIUtilsTest, test_embedding_parse_output)
{
  ObOpenAIUtils::ObOpenAIEmbed embedding;
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, embedding.parse_output(allocator_, object(
      R"J({"data":[{"embedding":[0.0023064255,-0.009327292,-0.0028842222],"index":0,"object":"embedding"}],
          "model":"text-embedding-v4","object":"list"})J"), result));
  ObJsonArray *vectors = static_cast<ObJsonArray *>(result);
  ASSERT_EQ(1, vectors->element_count());
  EXPECT_EQ("[0.0023064255, -0.009327292, -0.0028842222]", str(vectors->get_value(0)));
}

TEST_F(ObOpenAIUtilsTest, test_embedding_parse_output_empty)
{
  ObOpenAIUtils::ObOpenAIEmbed embedding;
  ObIJsonBase *result = nullptr;
  EXPECT_EQ(OB_INVALID_DATA, embedding.parse_output(allocator_, object(
      R"J({"data":[{"index":0,"object":"embedding"}]})J"), result));
  EXPECT_EQ(OB_INVALID_DATA, embedding.parse_output(allocator_, object(R"J({"data":"x"})J"), result));
  EXPECT_EQ(OB_INVALID_DATA, embedding.parse_output(allocator_, object(R"J({})J"), result));
}

TEST_F(ObOpenAIUtilsTest, test_embedding_restores_index_order)
{
  ObOpenAIUtils::ObOpenAIEmbed embedding;
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, embedding.parse_output(allocator_, object(
      R"J({"data":[{"index":2,"embedding":[3.0]},{"index":0,"embedding":[1.0]},{"index":1,"embedding":[2.0]}]})J"),
      result));
  EXPECT_EQ("[[1.0], [2.0], [3.0]]", str(result));
  // without any index the response order is kept
  ASSERT_EQ(OB_SUCCESS, embedding.parse_output(allocator_, object(
      R"J({"data":[{"embedding":[5.0]},{"embedding":[6.0]}]})J"), result));
  EXPECT_EQ("[[5.0], [6.0]]", str(result));
}

TEST_F(ObOpenAIUtilsTest, test_embedding_rejects_bad_index)
{
  ObOpenAIUtils::ObOpenAIEmbed embedding;
  ObIJsonBase *result = nullptr;
  // duplicated
  EXPECT_EQ(OB_INVALID_DATA, embedding.parse_output(allocator_, object(
      R"J({"data":[{"index":0,"embedding":[1]},{"index":0,"embedding":[2]}]})J"), result));
  // out of range
  EXPECT_EQ(OB_INVALID_DATA, embedding.parse_output(allocator_, object(
      R"J({"data":[{"index":2,"embedding":[1]},{"index":0,"embedding":[2]}]})J"), result));
  EXPECT_EQ(OB_INVALID_DATA, embedding.parse_output(allocator_, object(
      R"J({"data":[{"index":-1,"embedding":[1]}]})J"), result));
  // mixed presence
  EXPECT_EQ(OB_INVALID_DATA, embedding.parse_output(allocator_, object(
      R"J({"data":[{"embedding":[1]},{"index":0,"embedding":[2]}]})J"), result));
  // not an integer
  EXPECT_EQ(OB_INVALID_DATA, embedding.parse_output(allocator_, object(
      R"J({"data":[{"index":0.5,"embedding":[1]}]})J"), result));
  EXPECT_EQ(OB_INVALID_DATA, embedding.parse_output(allocator_, object(
      R"J({"data":[{"index":"0","embedding":[1]}]})J"), result));
}

TEST_F(ObOpenAIUtilsTest, test_embedding_vector_check)
{
  int64_t dim = 0;
  EXPECT_EQ(OB_SUCCESS, ObAIFuncUtils::check_embedding_vector(json("[0.1, 2, -3]"), 0, dim));
  EXPECT_EQ(3, dim);
  EXPECT_EQ(OB_SUCCESS, ObAIFuncUtils::check_embedding_vector(json("[0.1, 2, -3]"), 3, dim));
  // dimension mismatch
  EXPECT_EQ(OB_INVALID_ARGUMENT, ObAIFuncUtils::check_embedding_vector(json("[0.1, 2]"), 3, dim));
  // illegal vectors and elements
  EXPECT_EQ(OB_INVALID_DATA, ObAIFuncUtils::check_embedding_vector(json("[]"), 0, dim));
  EXPECT_EQ(OB_INVALID_DATA, ObAIFuncUtils::check_embedding_vector(json("[\"a\"]"), 0, dim));
  EXPECT_EQ(OB_INVALID_DATA, ObAIFuncUtils::check_embedding_vector(json("[null, 1]"), 0, dim));
  EXPECT_EQ(OB_INVALID_DATA, ObAIFuncUtils::check_embedding_vector(json("[[1], 1]"), 0, dim));
  EXPECT_EQ(OB_INVALID_DATA, ObAIFuncUtils::check_embedding_vector(json("{\"a\":1}"), 0, dim));
  EXPECT_EQ(OB_INVALID_DATA, ObAIFuncUtils::check_embedding_vector(nullptr, 0, dim));
}

TEST_F(ObOpenAIUtilsTest, test_json_index_and_number)
{
  int64_t index = 0;
  double score = 0;
  EXPECT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_index_value(json("3"), index));
  EXPECT_EQ(3, index);
  EXPECT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_index_value(json("2.0"), index));
  EXPECT_EQ(2, index);
  EXPECT_NE(OB_SUCCESS, ObAIFuncJsonUtils::get_index_value(json("-1"), index));
  EXPECT_NE(OB_SUCCESS, ObAIFuncJsonUtils::get_index_value(json("1.5"), index));
  EXPECT_NE(OB_SUCCESS, ObAIFuncJsonUtils::get_index_value(json("\"1\""), index));
  EXPECT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_number_value(json("0.75"), score));
  EXPECT_DOUBLE_EQ(0.75, score);
  EXPECT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_number_value(json("1"), score));
  EXPECT_DOUBLE_EQ(1.0, score);
  EXPECT_NE(OB_SUCCESS, ObAIFuncJsonUtils::get_number_value(json("\"0.75\""), score));
  EXPECT_NE(OB_SUCCESS, ObAIFuncJsonUtils::get_number_value(json("null"), score));
}

// ---------------------------------------------------------------- Ollama

TEST_F(ObOpenAIUtilsTest, test_ollama_get_header)
{
  ObArray<ObString> headers;
  ObString empty_key;
  ObOllamaUtils::ObOllamaComplete completion;
  // no api key: no Authorization header
  ASSERT_EQ(OB_SUCCESS, completion.get_header(allocator_, empty_key, headers));
  ASSERT_EQ(1, headers.count());
  EXPECT_EQ(ObString("Content-Type: application/json"), headers[0]);
  // a configured key is still forwarded
  headers.reuse();
  ObString api_key("secret");
  ObOllamaUtils::ObOllamaEmbed embedding;
  ASSERT_EQ(OB_SUCCESS, embedding.get_header(allocator_, api_key, headers));
  ASSERT_EQ(2, headers.count());
  EXPECT_EQ(ObString("Authorization: Bearer secret"), headers[0]);
  EXPECT_EQ(ObString("Content-Type: application/json"), headers[1]);
}

TEST_F(ObOpenAIUtilsTest, test_ollama_generate_body)
{
  ObOllamaUtils::ObOllamaComplete completion;
  completion.set_request_url(ObString("http://127.0.0.1:11434/api/generate"));
  ObString model("qwen2.5");
  ObString system("be brief");
  ObString content("hello");
  ObJsonObject *config = object(
      R"J({"temperature":0.2,"max_tokens":16,"keep_alive":"1m","stream":true,"options":{"seed":7}})J");
  ObJsonObject *body = nullptr;
  ASSERT_EQ(OB_SUCCESS, completion.get_body(allocator_, model, system, content, config, body));
  EXPECT_EQ("qwen2.5", str(body->get_value("model")));
  EXPECT_EQ("hello", str(body->get_value("prompt")));
  EXPECT_EQ("be brief", str(body->get_value("system")));
  EXPECT_EQ(nullptr, body->get_value("messages"));
  // never streaming, even if the config asks for it
  EXPECT_EQ("false", str(body->get_value("stream")));
  EXPECT_EQ("1m", str(body->get_value("keep_alive")));
  EXPECT_EQ(nullptr, body->get_value("max_tokens"));
  EXPECT_EQ(nullptr, body->get_value("temperature"));
  ObJsonObject *options = static_cast<ObJsonObject *>(body->get_value("options"));
  ASSERT_NE(nullptr, options);
  EXPECT_EQ("16", str(options->get_value("num_predict")));
  EXPECT_EQ("0.2", str(options->get_value("temperature")));
  EXPECT_EQ("7", str(options->get_value("seed")));
}

TEST_F(ObOpenAIUtilsTest, test_ollama_generate_body_without_url_and_config)
{
  // generate is the default protocol
  ObOllamaUtils::ObOllamaComplete completion;
  ObString model("qwen2.5");
  ObString system;
  ObString content("hello");
  ObJsonObject *body = nullptr;
  ASSERT_EQ(OB_SUCCESS, completion.get_body(allocator_, model, system, content, nullptr, body));
  EXPECT_EQ("hello", str(body->get_value("prompt")));
  EXPECT_EQ("false", str(body->get_value("stream")));
  EXPECT_EQ(nullptr, body->get_value("system"));
  EXPECT_EQ(nullptr, body->get_value("options"));
  ObString empty_content;
  EXPECT_EQ(OB_INVALID_ARGUMENT, completion.get_body(allocator_, model, system, empty_content, nullptr, body));
}

TEST_F(ObOpenAIUtilsTest, test_ollama_chat_body)
{
  ObOllamaUtils::ObOllamaComplete completion;
  completion.set_request_url(ObString("http://127.0.0.1:11434/API/Chat/?x=1"));
  ObString model("qwen2.5");
  ObString system("be brief");
  ObString content("hello");
  ObJsonObject *body = nullptr;
  ASSERT_EQ(OB_SUCCESS, completion.get_body(allocator_, model, system, content,
                                            object(R"J({"max_tokens":4})J"), body));
  EXPECT_EQ(nullptr, body->get_value("prompt"));
  ObJsonArray *messages = static_cast<ObJsonArray *>(body->get_value("messages"));
  ASSERT_NE(nullptr, messages);
  ASSERT_EQ(2, messages->element_count());
  ObJsonObject *sys = static_cast<ObJsonObject *>(messages->get_value(0));
  ObJsonObject *user = static_cast<ObJsonObject *>(messages->get_value(1));
  EXPECT_EQ("system", str(sys->get_value("role")));
  EXPECT_EQ("be brief", str(sys->get_value("content")));
  EXPECT_EQ("user", str(user->get_value("role")));
  EXPECT_EQ("hello", str(user->get_value("content")));
  EXPECT_EQ("false", str(body->get_value("stream")));
  EXPECT_EQ("4", str(static_cast<ObJsonObject *>(body->get_value("options"))->get_value("num_predict")));
}

TEST_F(ObOpenAIUtilsTest, test_ollama_api_path)
{
  EXPECT_TRUE(ObOllamaUtils::is_api_path(ObString("http://h:1/api/chat"), ObOllamaUtils::CHAT_API_PATH));
  EXPECT_TRUE(ObOllamaUtils::is_api_path(ObString("http://h:1/api/chat/"), ObOllamaUtils::CHAT_API_PATH));
  EXPECT_TRUE(ObOllamaUtils::is_api_path(ObString("http://h:1/api/chat?a=b#c"), ObOllamaUtils::CHAT_API_PATH));
  EXPECT_FALSE(ObOllamaUtils::is_api_path(ObString("http://h:1/api/generate"), ObOllamaUtils::CHAT_API_PATH));
  EXPECT_FALSE(ObOllamaUtils::is_api_path(ObString("http://h:1/api/chatx"), ObOllamaUtils::CHAT_API_PATH));
  EXPECT_FALSE(ObOllamaUtils::is_api_path(ObString(), ObOllamaUtils::CHAT_API_PATH));
}

TEST_F(ObOpenAIUtilsTest, test_ollama_complete_parse_output)
{
  ObOllamaUtils::ObOllamaComplete completion;
  ObIJsonBase *result = nullptr;
  // /api/generate
  ASSERT_EQ(OB_SUCCESS, completion.parse_output(allocator_, object(
      R"J({"model":"qwen2.5","response":"r1","done":true})J"), result));
  EXPECT_EQ("r1", str(result));
  // /api/chat
  ASSERT_EQ(OB_SUCCESS, completion.parse_output(allocator_, object(
      R"J({"model":"qwen2.5","message":{"role":"assistant","content":"r2"},"done":true})J"), result));
  EXPECT_EQ("r2", str(result));
  EXPECT_EQ(OB_INVALID_DATA, completion.parse_output(allocator_, object(R"J({"error":"x"})J"), result));
  EXPECT_EQ(OB_INVALID_DATA, completion.parse_output(allocator_, object(R"J({"response":1})J"), result));
  EXPECT_EQ(OB_INVALID_DATA, completion.parse_output(allocator_, object(R"J({"message":"x"})J"), result));
}

TEST_F(ObOpenAIUtilsTest, test_ollama_embed_body_and_output)
{
  ObOllamaUtils::ObOllamaEmbed embedding;
  ObString model("nomic-embed-text");
  ObArray<ObString> inputs;
  ASSERT_EQ(OB_SUCCESS, inputs.push_back(ObString("a")));
  ASSERT_EQ(OB_SUCCESS, inputs.push_back(ObString("b")));
  ObJsonObject *body = nullptr;
  ASSERT_EQ(OB_SUCCESS, embedding.get_body(allocator_, model, inputs, object(R"J({"dimensions":4})J"), body));
  EXPECT_EQ("nomic-embed-text", str(body->get_value("model")));
  EXPECT_EQ("[\"a\", \"b\"]", str(body->get_value("input")));
  EXPECT_EQ("4", str(body->get_value("dimensions")));

  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, embedding.parse_output(allocator_, object(
      R"J({"model":"nomic-embed-text","embeddings":[[0.1,0.2],[0.3,0.4]]})J"), result));
  EXPECT_EQ("[[0.1, 0.2], [0.3, 0.4]]", str(result));
  EXPECT_EQ(OB_INVALID_DATA, embedding.parse_output(allocator_, object(R"J({"embeddings":"x"})J"), result));
  EXPECT_EQ(OB_INVALID_DATA, embedding.parse_output(allocator_, object(R"J({"error":"x"})J"), result));
}

TEST_F(ObOpenAIUtilsTest, test_provider_factory)
{
  ObAIFuncIComplete *complete = nullptr;
  ObAIFuncIEmbed *embed = nullptr;
  // provider names are case insensitive
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::get_complete_provider(allocator_, ObString("oLLaMa"), complete));
  EXPECT_NE(nullptr, dynamic_cast<ObOllamaUtils::ObOllamaComplete *>(complete));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::get_embed_provider(allocator_, ObString("OLLAMA"), embed));
  EXPECT_NE(nullptr, dynamic_cast<ObOllamaUtils::ObOllamaEmbed *>(embed));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::get_complete_provider(allocator_, ObString("OpenAI"), complete));
  EXPECT_NE(nullptr, dynamic_cast<ObOpenAIUtils::ObOpenAIComplete *>(complete));
  EXPECT_EQ(OB_NOT_SUPPORTED, ObAIFuncUtils::get_complete_provider(allocator_, ObString("unknown"), complete));
}

TEST_F(ObOpenAIUtilsTest, test_endpoint_provider_and_concurrency)
{
  // ollama accepts an empty or missing access key, any case
  EXPECT_EQ(OB_SUCCESS, endpoint_check(
      R"J({"ai_model_name":"m","url":"http://h/api/generate","access_key":"","provider":"Ollama"})J"));
  EXPECT_EQ(OB_SUCCESS, endpoint_check(
      R"J({"ai_model_name":"m","url":"http://h/api/embed","provider":"OLLAMA"})J"));
  // other providers still require a key
  EXPECT_NE(OB_SUCCESS, endpoint_check(
      R"J({"ai_model_name":"m","url":"http://h/v1","access_key":"","provider":"openai"})J"));
  EXPECT_NE(OB_SUCCESS, endpoint_check(
      R"J({"ai_model_name":"m","url":"http://h/v1","access_key":"k","provider":"no-such-provider"})J"));
  EXPECT_EQ(OB_SUCCESS, endpoint_check(
      R"J({"ai_model_name":"m","url":"http://h/v1","access_key":"k","provider":"openai","max_concurrency":1024})J"));
  EXPECT_EQ(OB_SUCCESS, endpoint_check(
      R"J({"ai_model_name":"m","url":"http://h/v1","access_key":"k","provider":"openai","max_concurrency":1})J"));
  EXPECT_NE(OB_SUCCESS, endpoint_check(
      R"J({"ai_model_name":"m","url":"http://h/v1","access_key":"k","provider":"openai","max_concurrency":0})J"));
  EXPECT_NE(OB_SUCCESS, endpoint_check(
      R"J({"ai_model_name":"m","url":"http://h/v1","access_key":"k","provider":"openai","max_concurrency":1025})J"));
  EXPECT_NE(OB_SUCCESS, endpoint_check(
      R"J({"ai_model_name":"m","url":"http://h/v1","access_key":"k","provider":"openai","max_concurrency":"8"})J"));
}
