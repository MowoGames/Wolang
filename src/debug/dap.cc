// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/debug/dap.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>

namespace wolang {
namespace debug {

using lsp::Json;

static std::mutex& SendLock() {
  static std::mutex sm_Lock;
  return sm_Lock;
}

DapSession::DapSession(Send send, Launch launch) : m_Send(std::move(send)), m_Launch(std::move(launch)) {}

DapSession::~DapSession() {
  Resume(Mode::kStop);
  Join();
}

std::string DapSession::Normalized(const std::string& path) {
  std::error_code error;
  std::filesystem::path absolute = std::filesystem::weakly_canonical(std::filesystem::path(path), error);
  if (error) {
    absolute = std::filesystem::path(path);
  }
  std::string text = absolute.generic_string();
#ifdef _WIN32
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
  return text;
}

void DapSession::Respond(const Json& request, Json body, bool success, const std::string& message) {
  Json response = Json::Object();
  response.Set("type", Json::String("response"));
  response.Set("request_seq", Json::Number(static_cast<double>(request["seq"].AsInt())));
  response.Set("command", Json::String(request["command"].AsString()));
  response.Set("success", Json::Bool(success));
  if (!message.empty()) {
    response.Set("message", Json::String(message));
  }
  response.Set("body", std::move(body));

  const std::lock_guard<std::mutex> held(SendLock());
  response.Set("seq", Json::Number(static_cast<double>(m_Sequence++)));
  m_Send(response.Write());
}

void DapSession::Event(const std::string& event, Json body) {
  Json message = Json::Object();
  message.Set("type", Json::String("event"));
  message.Set("event", Json::String(event));
  message.Set("body", std::move(body));

  const std::lock_guard<std::mutex> held(SendLock());
  message.Set("seq", Json::Number(static_cast<double>(m_Sequence++)));
  m_Send(message.Write());
}

void DapSession::Output(const std::string& text, const char* category) {
  Json body = Json::Object();
  body.Set("category", Json::String(category));
  body.Set("output", Json::String(text));
  Event("output", std::move(body));
}

void DapSession::SetSources(const SourceManager* sources) {
  const std::lock_guard<std::mutex> held(m_Lock);
  m_Sources = sources;
}

void DapSession::Start() {
  if (m_Started) {
    return;
  }
  m_Started = true;
  m_Program = std::thread([this]() {
    std::string error;
    const int code = m_Launch(m_Arguments, this, &error);
    if (code < 0) {
      Output(error, "stderr");
    }

    Json exited = Json::Object();
    exited.Set("exitCode", Json::Number(code < 0 ? 1 : code));
    Event("exited", std::move(exited));
    Event("terminated", Json::Object());
  });
}

void DapSession::Resume(Mode mode) {
  const std::lock_guard<std::mutex> held(m_Lock);
  m_Mode = mode;
  m_StepDepth = m_Stack.size();
  m_Go = true;
  m_Changed.notify_all();
}

void DapSession::Join() {
  if (m_Program.joinable()) {
    m_Program.join();
  }
}

uint32_t DapSession::Register(const std::vector<interp::DebugVariable>* variables) {
  m_References.push_back(variables);
  return static_cast<uint32_t>(m_References.size());
}

bool DapSession::Wants(SourceSpan span, size_t depth) {
  const std::lock_guard<std::mutex> held(m_Lock);
  if (m_Sources == nullptr) {
    return false;
  }
  if (m_Mode == Mode::kStop) {
    return true;
  }
  const uint32_t line = m_Sources->Position(span).line;
  const std::pair<uint32_t, uint32_t> here(span.file.index, line);
  m_Lines.resize(std::max<size_t>(depth, 1));
  const bool same = m_Lines.back() == here;
  m_Lines.back() = here;
  // Out of the call it was asked to leave: stopped at once, in the middle
  // of the caller's line, which is where the answer is about to be used.
  if (m_Mode == Mode::kStepOut && depth < m_StepDepth) {
    return true;
  }
  if (same) {
    return false;
  }

  const std::string path = Normalized(std::string(m_Sources->Path(span.file)));
  const auto breakpoints = m_Breakpoints.find(path);
  if (breakpoints != m_Breakpoints.end() && breakpoints->second.count(line) != 0) {
    m_Reason = "breakpoint";
    return true;
  }
  switch (m_Mode) {
    case Mode::kStepIn:
      return true;
    case Mode::kNext:
      return depth <= m_StepDepth;
    case Mode::kStepOut:
      return depth < m_StepDepth;
    case Mode::kPause:
      m_Reason = "pause";
      return true;
    default:
      return false;
  }
}

bool DapSession::Paused(const std::vector<interp::DebugFrame>& stack) {
  std::unique_lock<std::mutex> held(m_Lock);
  if (m_Mode == Mode::kStop) {
    return false;
  }
  m_Stack = stack;
  m_References.clear();
  for (const interp::DebugFrame& frame : m_Stack) {
    Register(&frame.Variables);
  }
  m_Paused = true;
  m_Go = false;

  Json stopped = Json::Object();
  stopped.Set("reason", Json::String(m_Reason.empty() ? "step" : m_Reason));
  stopped.Set("threadId", Json::Number(1));
  stopped.Set("allThreadsStopped", Json::Bool(true));
  m_Reason.clear();
  held.unlock();
  Event("stopped", std::move(stopped));
  held.lock();

  m_Changed.wait(held, [this]() { return m_Go; });
  m_Paused = false;
  return m_Mode != Mode::kStop;
}

void DapSession::Handle(const std::string& body) {
  const Json request = Json::Parse(body);
  if (request["type"].AsString() != "request") {
    return;
  }
  const std::string& command = request["command"].AsString();
  const Json& arguments = request["arguments"];

  if (command == "initialize") {
    Json capabilities = Json::Object();
    capabilities.Set("supportsConfigurationDoneRequest", Json::Bool(true));
    capabilities.Set("supportsTerminateRequest", Json::Bool(true));
    Respond(request, std::move(capabilities));
    Event("initialized", Json::Object());
    return;
  }
  if (command == "launch") {
    m_Arguments = arguments;
    m_Launched = true;
    if (arguments["stopOnEntry"].AsBool()) {
      const std::lock_guard<std::mutex> held(m_Lock);
      m_Mode = Mode::kStepIn;
      m_Reason = "entry";
    }
    Respond(request, Json::Object());
    if (m_Configured) {
      Start();
    }
    return;
  }
  if (command == "attach") {
    m_Attached = true;
    Respond(request, Json::Object());
    return;
  }
  if (command == "setBreakpoints") {
    const std::string path = Normalized(arguments["source"]["path"].AsString());
    Json verified = Json::Array();
    std::set<uint32_t> lines;
    const Json& wanted = arguments["breakpoints"];
    for (size_t i = 0; i < wanted.size(); ++i) {
      const auto line = static_cast<uint32_t>(wanted.At(i)["line"].AsInt());
      lines.insert(line);
      Json breakpoint = Json::Object();
      breakpoint.Set("verified", Json::Bool(true));
      breakpoint.Set("line", Json::Number(line));
      verified.Push(std::move(breakpoint));
    }
    {
      const std::lock_guard<std::mutex> held(m_Lock);
      m_Breakpoints[path] = std::move(lines);
    }
    Json answer = Json::Object();
    answer.Set("breakpoints", std::move(verified));
    Respond(request, std::move(answer));
    return;
  }
  if (command == "configurationDone") {
    m_Configured = true;
    Respond(request, Json::Object());
    if (m_Launched) {
      Start();
    }
    return;
  }
  if (command == "threads") {
    Json thread = Json::Object();
    thread.Set("id", Json::Number(1));
    thread.Set("name", Json::String("main"));
    Json threads = Json::Array();
    threads.Push(std::move(thread));
    Json answer = Json::Object();
    answer.Set("threads", std::move(threads));
    Respond(request, std::move(answer));
    return;
  }
  if (command == "stackTrace") {
    Json frames = Json::Array();
    {
      const std::lock_guard<std::mutex> held(m_Lock);
      for (size_t i = 0; m_Paused && m_Sources != nullptr && i < m_Stack.size(); ++i) {
        const interp::DebugFrame& frame = m_Stack[i];
        const LineColumn where = m_Sources->Position(frame.Span);
        const std::string path(m_Sources->Path(frame.Span.file));
        Json source = Json::Object();
        source.Set("name", Json::String(std::filesystem::path(path).filename().string()));
        source.Set("path", Json::String(path));
        Json shown = Json::Object();
        shown.Set("id", Json::Number(static_cast<double>(i)));
        shown.Set("name", Json::String(frame.Function));
        shown.Set("source", std::move(source));
        shown.Set("line", Json::Number(where.line));
        shown.Set("column", Json::Number(where.column));
        frames.Push(std::move(shown));
      }
    }
    Json answer = Json::Object();
    answer.Set("totalFrames", Json::Number(static_cast<double>(frames.size())));
    answer.Set("stackFrames", std::move(frames));
    Respond(request, std::move(answer));
    return;
  }
  if (command == "scopes") {
    const int64_t frame = arguments["frameId"].AsInt();
    Json scope = Json::Object();
    scope.Set("name", Json::String("Locals"));
    scope.Set("variablesReference", Json::Number(static_cast<double>(frame + 1)));
    scope.Set("expensive", Json::Bool(false));
    Json scopes = Json::Array();
    scopes.Push(std::move(scope));
    Json answer = Json::Object();
    answer.Set("scopes", std::move(scopes));
    Respond(request, std::move(answer));
    return;
  }
  if (command == "variables") {
    const int64_t reference = arguments["variablesReference"].AsInt();
    Json variables = Json::Array();
    {
      const std::lock_guard<std::mutex> held(m_Lock);
      if (m_Paused && reference >= 1 && static_cast<size_t>(reference) <= m_References.size()) {
        const std::vector<interp::DebugVariable>* listed = m_References[static_cast<size_t>(reference - 1)];
        for (const interp::DebugVariable& variable : *listed) {
          Json shown = Json::Object();
          shown.Set("name", Json::String(variable.Name));
          shown.Set("value", Json::String(variable.Value));
          shown.Set("type", Json::String(variable.Type));
          shown.Set("variablesReference", Json::Number(variable.Fields.empty() ? 0 : Register(&variable.Fields)));
          variables.Push(std::move(shown));
        }
      }
    }
    Json answer = Json::Object();
    answer.Set("variables", std::move(variables));
    Respond(request, std::move(answer));
    return;
  }
  if (command == "continue" || command == "next" || command == "stepIn" || command == "stepOut") {
    Json answer = Json::Object();
    if (command == "continue") {
      answer.Set("allThreadsContinued", Json::Bool(true));
    }
    Respond(request, std::move(answer));
    Resume(command == "continue" ? Mode::kRun : command == "next" ? Mode::kNext : command == "stepIn" ? Mode::kStepIn : Mode::kStepOut);
    return;
  }
  if (command == "pause") {
    {
      const std::lock_guard<std::mutex> held(m_Lock);
      m_Mode = Mode::kPause;
    }
    Respond(request, Json::Object());
    return;
  }
  // A host's program goes on without the editor: nothing to stop at, and
  // nothing paused.
  if (m_Attached && command == "disconnect") {
    {
      const std::lock_guard<std::mutex> held(m_Lock);
      m_Breakpoints.clear();
    }
    Resume(Mode::kRun);
    Respond(request, Json::Object());
    m_Finished = true;
    return;
  }
  if (command == "disconnect" || command == "terminate") {
    Resume(Mode::kStop);
    Join();
    Respond(request, Json::Object());
    m_Finished = command == "disconnect";
    return;
  }
  Respond(request, Json::Object(), false, "`" + command + "` is not something this debugger does");
}

std::optional<std::string> ReadFramed(std::FILE* in) {
  size_t length = 0;
  bool have_length = false;
  std::string line;
  for (;;) {
    const int letter = std::fgetc(in);
    if (letter == EOF) {
      return std::nullopt;
    }
    if (letter != '\n') {
      line.push_back(static_cast<char>(letter));
      continue;
    }
    if (line.empty() || line == "\r") {
      break;
    }
    if (line.rfind("Content-Length:", 0) == 0) {
      length = static_cast<size_t>(std::strtoull(line.c_str() + 15, nullptr, 10));
      have_length = true;
    }
    line.clear();
  }
  if (!have_length) {
    return std::nullopt;
  }

  std::string body(length, '\0');
  if (length != 0 && std::fread(body.data(), 1, length, in) != length) {
    return std::nullopt;
  }
  return body;
}

void WriteFramed(std::FILE* out, const std::string& body) {
  std::fprintf(out, "Content-Length: %zu\r\n\r\n", body.size());
  std::fwrite(body.data(), 1, body.size(), out);
  std::fflush(out);
}

}  // namespace debug
}  // namespace wolang
