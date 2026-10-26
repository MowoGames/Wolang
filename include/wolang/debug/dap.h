// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.
//
// The Debug Adapter Protocol, spoken by the interpreter (D216).

#ifndef WOLANG_DEBUG_DAP_H_
#define WOLANG_DEBUG_DAP_H_

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "wolang/base/source_manager.h"
#include "wolang/interp/debug.h"
#include "wolang/lsp/json.h"

namespace wolang {
namespace debug {

// One editor's debugging session: requests in, responses and events out,
// and a program run on the interpreter that stops where the editor says.
//
// It does not know how messages travel or how a program is built. The
// transport hands it each message body and takes what it sends back; the
// launcher builds the program, says which sources it came from, and runs
// it with the session as its debugger -- on a thread of the session's,
// so that requests keep being answered while the program runs.
class DapSession : public interp::Debugger {
 public:
  using Send = std::function<void(const std::string& body)>;
  // Builds what |arguments| of a `launch` name and runs it to the end:
  // calls |sources| once the program is built, and answers the exit code,
  // or -1 with |error| filled when it could not be built.
  using Launch = std::function<int(const lsp::Json& arguments, DapSession* session, std::string* error)>;

  DapSession(Send send, Launch launch);
  ~DapSession() override;

  // One message from the editor.
  void Handle(const std::string& body);

  // Whether the editor said it is done.
  bool finished() const { return m_Finished; }

  // Whether the editor attached to a program that was already running,
  // rather than asking for one to be started: an embedding host's vm.
  bool attached() const { return m_Attached; }

  // Called by the launcher before the program starts.
  void SetSources(const SourceManager* sources);

  // What the program wrote, as the editor's console shows it.
  void Output(const std::string& text, const char* category = "stdout");

  bool Wants(SourceSpan span, size_t depth) override;
  bool Paused(const std::vector<interp::DebugFrame>& stack) override;

 private:
  enum class Mode { kRun, kStepIn, kNext, kStepOut, kPause, kStop };

  void Respond(const lsp::Json& request, lsp::Json body, bool success = true, const std::string& message = std::string());
  void Event(const std::string& event, lsp::Json body);
  void Start();
  void Resume(Mode mode);
  void Join();
  uint32_t Register(const std::vector<interp::DebugVariable>* variables);
  static std::string Normalized(const std::string& path);

  Send m_Send;
  Launch m_Launch;
  lsp::Json m_Arguments;
  int64_t m_Sequence = 1;
  bool m_Launched = false;
  bool m_Configured = false;
  bool m_Started = false;
  bool m_Finished = false;
  bool m_Attached = false;
  std::thread m_Program;

  // What the program thread and the message thread share.
  std::mutex m_Lock;
  std::condition_variable m_Changed;
  const SourceManager* m_Sources = nullptr;
  std::map<std::string, std::set<uint32_t>> m_Breakpoints;
  Mode m_Mode = Mode::kRun;
  size_t m_StepDepth = 0;
  // The line each call on the stack was last on, outermost first: a call
  // that returns lands back in the middle of its caller's line, which is
  // not a new one.
  std::vector<std::pair<uint32_t, uint32_t>> m_Lines;
  std::string m_Reason;
  bool m_Paused = false;
  bool m_Go = false;
  std::vector<interp::DebugFrame> m_Stack;
  std::vector<const std::vector<interp::DebugVariable>*> m_References;
};

// The framing both protocols an editor speaks use: a `Content-Length`
// header, a blank line, and that many bytes.
std::optional<std::string> ReadFramed(std::FILE* in);
void WriteFramed(std::FILE* out, const std::string& body);

}  // namespace debug
}  // namespace wolang

#endif  // WOLANG_DEBUG_DAP_H_
