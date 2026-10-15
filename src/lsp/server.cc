// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/lsp/server.h"

#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "wolang/base/diagnostic.h"
#include "wolang/compiler/database.h"
#include "wolang/format/formatter.h"
#include "wolang/lsp/json.h"
#include "wolang/sema/type_check.h"
#include "wolang/syntax/ast.h"

namespace wolang {
namespace lsp {
namespace {

// ------------------------------------------------------------- framing ---

// One message, read from stdin: a header with a length, a blank line, and
// that many bytes of JSON.
std::optional<std::string> ReadMessage() {
  size_t length = 0;
  bool have_length = false;
  std::string line;
  for (;;) {
    const int letter = std::getchar();
    if (letter == EOF) {
      return std::nullopt;
    }
    if (letter == '\n') {
      // A blank line ends the header.
      if (line.empty() || line == "\r") {
        break;
      }
      const std::string_view header(line);
      if (header.rfind("Content-Length:", 0) == 0) {
        length = static_cast<size_t>(
            std::strtoull(std::string(header.substr(15)).c_str(), nullptr,
                          10));
        have_length = true;
      }
      line.clear();
      continue;
    }
    line.push_back(static_cast<char>(letter));
  }
  if (!have_length) {
    return std::nullopt;
  }
  std::string body(length, '\0');
  for (size_t i = 0; i < length; ++i) {
    const int letter = std::getchar();
    if (letter == EOF) {
      return std::nullopt;
    }
    body[i] = static_cast<char>(letter);
  }
  return body;
}

void WriteMessage(const Json& message) {
  const std::string body = message.Write();
  std::printf("Content-Length: %zu\r\n\r\n%s", body.size(), body.c_str());
  std::fflush(stdout);
}

// ------------------------------------------------------------ the server ---

// What a URI means as a path: `file:///a/b.wo` is `/a/b.wo`, with the
// percent escapes undone.
std::string PathOfUri(const std::string& uri) {
  std::string path = uri;
  if (path.rfind("file://", 0) == 0) {
    path = path.substr(7);
  }
  std::string out;
  for (size_t i = 0; i < path.size(); ++i) {
    if (path[i] == '%' && i + 2 < path.size()) {
      const std::string hex = path.substr(i + 1, 2);
      out.push_back(static_cast<char>(
          std::strtoul(hex.c_str(), nullptr, 16)));
      i += 2;
      continue;
    }
    out.push_back(path[i]);
  }
  return out;
}

std::string UriOfPath(const std::string& path) {
  return "file://" + path;
}

class Server {
 public:
  int Run() {
    for (;;) {
      const std::optional<std::string> message = ReadMessage();
      if (!message.has_value()) {
        return 0;
      }
      const Json request = Json::Parse(*message);
      const std::string method = request["method"].AsString();
      if (method.empty()) {
        continue;
      }
      if (method == "exit") {
        return shutting_down_ ? 0 : 1;
      }
      Handle(method, request);
    }
  }

 private:
  void Handle(const std::string& method, const Json& request) {
    if (method == "initialize") {
      Reply(request, Capabilities());
      return;
    }
    if (method == "shutdown") {
      shutting_down_ = true;
      Reply(request, Json());
      return;
    }
    if (method == "textDocument/didOpen") {
      const Json& document = request["params"]["textDocument"];
      Open(document["uri"].AsString(), document["text"].AsString());
      return;
    }
    if (method == "textDocument/didChange") {
      const std::string uri =
          request["params"]["textDocument"]["uri"].AsString();
      const Json& changes = request["params"]["contentChanges"];
      // Whole-document sync, which is what this server asks for: an
      // incremental edit would be a second representation of the text to
      // keep in step with the one the compiler holds.
      if (changes.size() != 0) {
        Open(uri, changes.At(changes.size() - 1)["text"].AsString());
      }
      return;
    }
    if (method == "textDocument/didSave") {
      return;
    }
    if (method == "textDocument/didClose") {
      return;
    }
    if (method == "textDocument/hover") {
      Reply(request, Hover(request["params"]));
      return;
    }
    if (method == "textDocument/definition") {
      Reply(request, Definition(request["params"]));
      return;
    }
    if (method == "textDocument/documentSymbol") {
      Reply(request, Symbols(request["params"]));
      return;
    }
    if (method == "textDocument/formatting") {
      Reply(request, Formatting(request["params"]));
      return;
    }
    if (method == "textDocument/completion") {
      Reply(request, Completion(request["params"]));
      return;
    }
    // A request this server does not answer still needs an answer, or the
    // editor waits forever.
    if (request.Has("id")) {
      Reply(request, Json());
    }
  }

  void Reply(const Json& request, Json result) {
    Json message = Json::Object();
    message.Set("jsonrpc", Json::String("2.0"));
    message.Set("id", request["id"]);
    message.Set("result", std::move(result));
    WriteMessage(message);
  }

  static Json Capabilities() {
    Json text_document = Json::Object();
    // 1: the whole document on every change.
    text_document.Set("textDocumentSync", Json::Number(1));
    text_document.Set("hoverProvider", Json::Bool(true));
    text_document.Set("definitionProvider", Json::Bool(true));
    text_document.Set("documentSymbolProvider", Json::Bool(true));
    text_document.Set("documentFormattingProvider", Json::Bool(true));
    Json completion = Json::Object();
    completion.Set("resolveProvider", Json::Bool(false));
    text_document.Set("completionProvider", std::move(completion));

    Json server = Json::Object();
    server.Set("name", Json::String("wolang"));
    server.Set("version", Json::String("0.1.0"));

    Json result = Json::Object();
    result.Set("capabilities", std::move(text_document));
    result.Set("serverInfo", std::move(server));
    return result;
  }

  // ------------------------------------------------------- documents ---

  // Reads a file into the database, replacing what was there, and
  // publishes what the compiler says about it.
  void Open(const std::string& uri, const std::string& text) {
    const std::string path = PathOfUri(uri);
    const auto found = files_.find(path);
    FileId file;
    if (found == files_.end()) {
      file = database_.AddFile(path, text);
      files_.emplace(path, file);
    } else {
      file = found->second;
      database_.SetFileText(file, text);
    }
    uris_[file.index] = uri;
    Publish(uri, file);
  }

  void Publish(const std::string& uri, FileId file) {
    Json list = Json::Array();
    if (const std::vector<Diagnostic>* diagnostics =
            database_.Diagnostics(file)) {
      for (const Diagnostic& diagnostic : *diagnostics) {
        list.Push(Describe(diagnostic));
      }
    }
    Json params = Json::Object();
    params.Set("uri", Json::String(uri));
    params.Set("diagnostics", std::move(list));

    Json message = Json::Object();
    message.Set("jsonrpc", Json::String("2.0"));
    message.Set("method",
                Json::String("textDocument/publishDiagnostics"));
    message.Set("params", std::move(params));
    WriteMessage(message);
  }

  Json Describe(const Diagnostic& diagnostic) {
    Json entry = Json::Object();
    entry.Set("range", Range(diagnostic.PrimarySpan()));
    // 1 error, 2 warning: the protocol's numbering.
    entry.Set("severity",
              Json::Number(diagnostic.severity == Severity::kError ? 1 : 2));
    entry.Set("code", Json::String(diagnostic.code));
    entry.Set("source", Json::String("wolang"));
    std::string message = diagnostic.message;
    for (const std::string& note : diagnostic.notes) {
      message.append("\n");
      message.append(note);
    }
    entry.Set("message", Json::String(message));
    return entry;
  }

  // A span as the protocol's zero-based line and UTF-16-ish column. The
  // columns are byte offsets inside the line, which is exact for ASCII
  // and what the rest of this compiler uses.
  Json Range(SourceSpan span) {
    Json range = Json::Object();
    if (!span.is_valid()) {
      range.Set("start", At(0, 0));
      range.Set("end", At(0, 0));
      return range;
    }
    const LineColumn begin = database_.sources().Position(span.file,
                                                          span.begin);
    const LineColumn end = database_.sources().Position(span.file, span.end);
    range.Set("start", At(begin.line - 1, begin.column - 1));
    range.Set("end", At(end.line - 1, end.column - 1));
    return range;
  }

  static Json At(uint32_t line, uint32_t column) {
    Json position = Json::Object();
    position.Set("line", Json::Number(line));
    position.Set("character", Json::Number(column));
    return position;
  }

  // Where a position in a document is, as an offset into its text.
  uint32_t OffsetOf(FileId file, const Json& position) {
    const std::string_view text = database_.sources().Text(file);
    const auto line = static_cast<uint32_t>(position["line"].AsInt());
    const auto column =
        static_cast<uint32_t>(position["character"].AsInt());
    uint32_t at = 0;
    uint32_t seen = 0;
    while (seen < line && at < text.size()) {
      if (text[at] == '\n') {
        ++seen;
      }
      ++at;
    }
    return at + column;
  }

  FileId FileOf(const Json& params) {
    const std::string path =
        PathOfUri(params["textDocument"]["uri"].AsString());
    const auto found = files_.find(path);
    return found == files_.end() ? FileId{} : found->second;
  }

  // --------------------------------------------------------- answers ---

  // The innermost expression whose span covers |offset|, with the
  // declaration it belongs to.
  struct Found {
    ItemId item;
    ExprId expr;
    uint32_t item_index = 0;
  };

  Found Innermost(const Ast& ast, uint32_t offset) {
    Found found;
    uint32_t index = 0;
    for (const ItemId id : ast.Get(ast.root())) {
      if (!ast.Contains(id)) {
        ++index;
        continue;
      }
      const Item& item = ast.Get(id);
      if (item.span.is_valid() && offset >= item.span.begin &&
          offset < item.span.end) {
        found.item = id;
        found.item_index = index;
      }
      ++index;
    }
    // Every expression of the file, narrowed to the smallest one that
    // covers the position: the tree is flat arrays, so this is a scan
    // rather than a walk -- and a file has thousands of expressions, not
    // millions.
    uint32_t best = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < ast.expr_count(); ++i) {
      const ExprId id{i};
      if (!ast.Contains(id)) {
        continue;
      }
      const SourceSpan span = ast.Get(id).span;
      if (!span.is_valid() || offset < span.begin || offset >= span.end) {
        continue;
      }
      const uint32_t width = span.end - span.begin;
      if (width < best) {
        best = width;
        found.expr = id;
      }
    }
    return found;
  }

  Json Hover(const Json& params) {
    const FileId file = FileOf(params);
    const ParsedFile* parsed = file.is_valid() ? database_.Parse(file)
                                               : nullptr;
    if (parsed == nullptr) {
      return Json();
    }
    const uint32_t offset = OffsetOf(file, params["position"]);
    const Found found = Innermost(parsed->ast, offset);
    std::string text;

    // What the checker worked out for the expression under the cursor.
    if (found.expr.is_valid() && found.item.is_valid()) {
      if (const CheckedFile* checked = database_.Checked(file)) {
        const TypeId type = TypeAt(*checked, found.item_index, found.expr);
        if (type.is_valid()) {
          text.append("```wolang\n");
          text.append(database_.types().Describe(database_.interner(),
                                                  type));
          text.append("\n```");
        }
      }
    }

    // And what the declaration it is in says about itself.
    if (found.item.is_valid()) {
      const Item& item = parsed->ast.Get(found.item);
      if (item.doc.is_valid()) {
        if (!text.empty()) {
          text.append("\n\n");
        }
        text.append(DocOf(item.doc));
      }
    }
    if (text.empty()) {
      return Json();
    }
    Json contents = Json::Object();
    contents.Set("kind", Json::String("markdown"));
    contents.Set("value", Json::String(text));
    Json result = Json::Object();
    result.Set("contents", std::move(contents));
    return result;
  }

  // The type the checker recorded for an expression.
  //
  // Per declaration, because that is how the checker runs: one typed body
  // per top-level item, in item tree order, and an expression of another
  // declaration has no entry in this one (D117).
  TypeId TypeAt(const CheckedFile& checked, uint32_t item, ExprId expr) {
    if (item < checked.items.size()) {
      const TypeId type = checked.items[item].TypeOf(expr);
      if (type.is_valid()) {
        return type;
      }
    }
    for (const TypedBody& body : checked.items) {
      const TypeId type = body.TypeOf(expr);
      if (type.is_valid()) {
        return type;
      }
    }
    return checked.body.TypeOf(expr);
  }

  std::string DocOf(SourceSpan span) {
    std::string text;
    const std::string_view whole = database_.sources().SpanText(span);
    size_t at = 0;
    while (at <= whole.size()) {
      const size_t stop = whole.find('\n', at);
      std::string_view line = whole.substr(
          at, stop == std::string_view::npos ? stop : stop - at);
      at = stop == std::string_view::npos ? whole.size() + 1 : stop + 1;
      while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
        line.remove_prefix(1);
      }
      if (line.rfind("///", 0) == 0) {
        line.remove_prefix(3);
      }
      if (!line.empty() && line.front() == ' ') {
        line.remove_prefix(1);
      }
      text.append(line);
      text.push_back('\n');
    }
    return text;
  }

  Json Definition(const Json& params) {
    const FileId file = FileOf(params);
    const ParsedFile* parsed = file.is_valid() ? database_.Parse(file)
                                               : nullptr;
    if (parsed == nullptr) {
      return Json();
    }
    const uint32_t offset = OffsetOf(file, params["position"]);
    const Found found = Innermost(parsed->ast, offset);
    if (!found.expr.is_valid()) {
      return Json();
    }
    const Expr& expr = parsed->ast.Get(found.expr);
    Symbol name = expr.name;
    if (!name.is_valid()) {
      return Json();
    }
    // Where that name was declared: the file resolution answers it, and
    // the declaration's own name span is where to go.
    const FileResolution* resolution = database_.Resolution(file);
    if (resolution == nullptr) {
      return Json();
    }
    const Binding* binding = resolution->Find(name);
    if (binding == nullptr || binding->item == Binding::kNoItem) {
      return Json();
    }
    const FileId home{binding->file};
    const ParsedFile* other = database_.Parse(home);
    if (other == nullptr) {
      return Json();
    }
    const ItemId item = ResolveItem(other->ast, binding->item);
    if (!other->ast.Contains(item)) {
      return Json();
    }
    const SourceSpan span = other->ast.Get(item).name_span;
    Json location = Json::Object();
    location.Set("uri", Json::String(UriOf(home)));
    location.Set("range", Range(span));
    return location;
  }

  std::string UriOf(FileId file) {
    const auto found = uris_.find(file.index);
    if (found != uris_.end()) {
      return found->second;
    }
    return UriOfPath(std::string(database_.sources().Path(file)));
  }

  Json Symbols(const Json& params) {
    const FileId file = FileOf(params);
    const ParsedFile* parsed = file.is_valid() ? database_.Parse(file)
                                               : nullptr;
    if (parsed == nullptr) {
      return Json::Array();
    }
    Json list = Json::Array();
    const Ast& ast = parsed->ast;
    for (const ItemId id : ast.Get(ast.root())) {
      if (!ast.Contains(id)) {
        continue;
      }
      const Item& item = ast.Get(id);
      if (!item.name.is_valid() || !item.span.is_valid()) {
        continue;
      }
      Json symbol = Json::Object();
      symbol.Set("name",
                 Json::String(std::string(
                     database_.interner().Text(item.name))));
      symbol.Set("kind", Json::Number(SymbolKind(item.kind)));
      symbol.Set("range", Range(item.span));
      symbol.Set("selectionRange", Range(item.name_span));
      Json children = Json::Array();
      for (const ItemId member_id : ast.Get(item.members)) {
        if (!ast.Contains(member_id)) {
          continue;
        }
        const Item& member = ast.Get(member_id);
        if (!member.name.is_valid() || !member.span.is_valid()) {
          continue;
        }
        Json child = Json::Object();
        child.Set("name",
                  Json::String(std::string(
                      database_.interner().Text(member.name))));
        child.Set("kind", Json::Number(SymbolKind(member.kind)));
        child.Set("range", Range(member.span));
        child.Set("selectionRange", Range(member.name_span));
        children.Push(std::move(child));
      }
      if (children.size() != 0) {
        symbol.Set("children", std::move(children));
      }
      list.Push(std::move(symbol));
    }
    return list;
  }

  // The protocol's symbol numbering, for the kinds this language has.
  static int SymbolKind(ItemKind kind) {
    switch (kind) {
      case ItemKind::kFunction:
        return 12;
      case ItemKind::kClass:
        return 5;
      case ItemKind::kStruct:
      case ItemKind::kComponent:
        return 23;
      case ItemKind::kEnum:
        return 10;
      case ItemKind::kVariant:
        return 22;
      case ItemKind::kAbility:
        return 11;
      case ItemKind::kBinding:
        return 14;
      case ItemKind::kTypeAlias:
        return 5;
      default:
        return 12;
    }
  }

  Json Formatting(const Json& params) {
    const FileId file = FileOf(params);
    const ParsedFile* parsed = file.is_valid() ? database_.Parse(file)
                                               : nullptr;
    if (parsed == nullptr) {
      return Json::Array();
    }
    const TokenizedFile* tokens = database_.Tokens(file);
    if (tokens == nullptr) {
      return Json::Array();
    }
    const std::string formatted =
        Format(database_.sources(), database_.interner(), tokens->tokens,
               parsed->ast);
    if (formatted.empty()) {
      return Json::Array();
    }
    // One edit for the whole file: the formatter answers with the whole
    // text, and a diff would be this server guessing at what the editor
    // does with it.
    const std::string_view text = database_.sources().Text(file);
    const LineColumn last =
        database_.sources().Position(file,
                                     static_cast<uint32_t>(text.size()));
    Json range = Json::Object();
    range.Set("start", At(0, 0));
    range.Set("end", At(last.line - 1, last.column - 1));
    Json edit = Json::Object();
    edit.Set("range", std::move(range));
    edit.Set("newText", Json::String(formatted));
    Json list = Json::Array();
    list.Push(std::move(edit));
    return list;
  }

  Json Completion(const Json& params) {
    const FileId file = FileOf(params);
    Json list = Json::Array();
    if (!file.is_valid()) {
      return list;
    }
    // What is in scope at file level, which is what a completion in a
    // body mostly wants: the module's own names and its imports. A
    // completion that knew about locals needs the checker to record a
    // scope per position, which it does not do yet -- said here rather
    // than guessed at.
    if (const FileResolution* resolution = database_.Resolution(file)) {
      for (const Binding& binding : resolution->scope) {
        if (!binding.name.is_valid()) {
          continue;
        }
        Json entry = Json::Object();
        entry.Set("label",
                  Json::String(std::string(
                      database_.interner().Text(binding.name))));
        entry.Set("kind", Json::Number(CompletionKind(binding.kind)));
        if (binding.type.is_valid()) {
          entry.Set("detail",
                    Json::String(database_.types().Describe(
                        database_.interner(), binding.type)));
        }
        list.Push(std::move(entry));
      }
    }
    for (const std::string_view keyword : Keywords()) {
      Json entry = Json::Object();
      entry.Set("label", Json::String(std::string(keyword)));
      entry.Set("kind", Json::Number(14));
      list.Push(std::move(entry));
    }
    return list;
  }

  static int CompletionKind(BindingKind kind) {
    switch (kind) {
      case BindingKind::kFunction:
        return 3;
      case BindingKind::kType:
        return 7;
      case BindingKind::kLocal:
      case BindingKind::kValue:
        return 6;
      default:
        return 12;
    }
  }

  static std::vector<std::string_view> Keywords() {
    return {
#define WOLANG_KEYWORD(Name, Spelling) Spelling,
#include "wolang/syntax/token_kind.inc"
    };
  }

  Database database_;
  std::unordered_map<std::string, FileId> files_;
  std::unordered_map<uint32_t, std::string> uris_;
  bool shutting_down_ = false;
};

}  // namespace

int Serve() {
  Server server;
  return server.Run();
}

}  // namespace lsp
}  // namespace wolang
