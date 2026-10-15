// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/lsp/json.h"

#include "harness/test.h"

namespace wolang {
namespace lsp {
namespace {

WOLANG_TEST(LspJsonTest, ReadsWhatAnEditorSends) {
  const Json message = Json::Parse(
      "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"textDocument/hover\","
      "\"params\":{\"position\":{\"line\":3,\"character\":11},"
      "\"ok\":true,\"nothing\":null,\"list\":[1,\"two\",false]}}");
  WOLANG_EXPECT_EQ(message["jsonrpc"].AsString(), "2.0");
  WOLANG_EXPECT_EQ(message["id"].AsInt(), int64_t{7});
  WOLANG_EXPECT_EQ(message["method"].AsString(), "textDocument/hover");
  const Json& params = message["params"];
  WOLANG_EXPECT_EQ(params["position"]["line"].AsInt(), int64_t{3});
  WOLANG_EXPECT_EQ(params["position"]["character"].AsInt(), int64_t{11});
  WOLANG_EXPECT(params["ok"].AsBool());
  WOLANG_EXPECT(params["nothing"].is_null());
  WOLANG_EXPECT_EQ(params["list"].size(), size_t{3});
  WOLANG_EXPECT_EQ(params["list"].At(1).AsString(), "two");
  // A name nothing set is null rather than a crash: a server must not die
  // on a message it did not expect.
  WOLANG_EXPECT(message["missing"].is_null());
  WOLANG_EXPECT(message["params"]["missing"]["deeper"].is_null());
}

WOLANG_TEST(LspJsonTest, WritesWhatAnEditorReads) {
  Json position = Json::Object();
  position.Set("line", Json::Number(2));
  position.Set("character", Json::Number(0));
  Json list = Json::Array();
  list.Push(Json::String("a\"b\nc"));
  list.Push(Json::Bool(false));
  Json message = Json::Object();
  message.Set("position", std::move(position));
  message.Set("list", std::move(list));
  // Whole numbers without a point, and the escapes an editor expects.
  WOLANG_EXPECT_EQ(message.Write(),
                   "{\"position\":{\"line\":2,\"character\":0},"
                   "\"list\":[\"a\\\"b\\nc\",false]}");
}

WOLANG_TEST(LspJsonTest, SurvivesSomethingThatIsNotJson) {
  WOLANG_EXPECT(Json::Parse("").is_null());
  WOLANG_EXPECT(Json::Parse("{").kind() == Json::Kind::kObject);
  WOLANG_EXPECT(Json::Parse("[1,").kind() == Json::Kind::kArray);
  WOLANG_EXPECT(Json::Parse("not json at all").is_null());
}

WOLANG_TEST(LspJsonTest, ReadsTheEscapesInAPath) {
  const Json message = Json::Parse(
      "{\"uri\":\"file:///tmp/a%20b.wo\",\"text\":\"\\u0041\\tb\"}");
  WOLANG_EXPECT_EQ(message["uri"].AsString(), "file:///tmp/a%20b.wo");
  WOLANG_EXPECT_EQ(message["text"].AsString(), "A\tb");
}

}  // namespace
}  // namespace lsp
}  // namespace wolang
