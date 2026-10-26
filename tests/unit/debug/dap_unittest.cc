// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.
//
// A debugging session, driven the way an editor drives one: requests in,
// and a program on the interpreter that stops where it was asked to.

#include "wolang/debug/dap.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "harness/test.h"
#include "wolang/abi/layout.h"
#include "wolang/codegen/foreign.h"
#include "wolang/compiler/database.h"
#include "wolang/interp/interpreter.h"

namespace wolang {
namespace debug {

static const char* const kProgram = R"(
class Player {
  var hp: int
}

fun Hurt(player: Player, amount: int) -> int {
  const left = player.hp - amount
  player.hp = left
  return left
}

fun main() -> int {
  var total = 0
  const hero = Player(10)
  for i in 0..3 {
    total = total + Hurt(hero, i)
  }
  return total
}
)";

// What the session sent, and a way to wait for the next thing it sends.
class Editor {
 public:
  void Take(const std::string& body) {
    const std::lock_guard<std::mutex> held(m_Lock);
    m_Received.push_back(lsp::Json::Parse(body));
    m_Arrived.notify_all();
  }

  lsp::Json WaitFor(const std::string& type, const std::string& name) {
    std::unique_lock<std::mutex> held(m_Lock);
    for (;;) {
      for (auto it = m_Received.begin(); it != m_Received.end(); ++it) {
        const std::string& kind = (*it)["type"].AsString();
        const std::string& what = kind == "event" ? (*it)["event"].AsString() : (*it)["command"].AsString();
        if (kind == type && what == name) {
          lsp::Json found = *it;
          m_Received.erase(it);
          return found;
        }
      }
      if (m_Arrived.wait_for(held, std::chrono::seconds(30)) == std::cv_status::timeout) {
        return lsp::Json();
      }
    }
  }

 private:
  std::mutex m_Lock;
  std::condition_variable m_Arrived;
  std::deque<lsp::Json> m_Received;
};

static int RunProgram(DapSession* session, std::string* error) {
  Database database;
  const FileId file = database.AddFile("debugged.wo", kProgram);
  const LoweredFile* lowered = database.Lowered(file);
  const CheckedFile* checked = database.Checked(file);
  if (lowered == nullptr || checked == nullptr) {
    *error = "no program";
    return -1;
  }

  const std::unique_ptr<interp::ForeignCaller> foreign = codegen::MakeForeignCaller();
  abi::LayoutStore layouts(database.types(), checked->resolution);
  session->SetSources(&database.sources());
  interp::Limits limits;
  limits.debugger = session;
  const interp::Outcome outcome = interp::RunNamed(database.interner(), database.types(), lowered->unoptimized, database.interner().Intern("main"), {}, limits, nullptr, foreign.get(), &layouts, {});
  session->SetSources(nullptr);
  return outcome.ok() ? static_cast<int>(outcome.value.signed_integer()) : 1;
}

static std::string Request(int64_t seq, const std::string& command, const std::string& arguments) {
  return "{\"seq\":" + std::to_string(seq) + ",\"type\":\"request\",\"command\":\"" + command + "\",\"arguments\":" + arguments + "}";
}

static int64_t LineOf(const lsp::Json& trace, size_t frame) {
  return trace["body"]["stackFrames"].At(frame)["line"].AsInt();
}

WOLANG_TEST(DapTest, StopsStepsAndShowsWhatTheProgramHolds) {
  Editor editor;
  DapSession session([&editor](const std::string& body) { editor.Take(body); }, [](const lsp::Json&, DapSession* debugged, std::string* error) { return RunProgram(debugged, error); });

  session.Handle(Request(1, "initialize", "{}"));
  const lsp::Json initialized = editor.WaitFor("event", "initialized");
  WOLANG_EXPECT_EQ(initialized["event"].AsString(), "initialized");
  session.Handle(Request(2, "launch", "{\"program\":\"debugged.wo\"}"));
  session.Handle(Request(3, "setBreakpoints", "{\"source\":{\"path\":\"debugged.wo\"},\"breakpoints\":[{\"line\":7}]}"));
  const lsp::Json set = editor.WaitFor("response", "setBreakpoints");
  WOLANG_EXPECT(set["body"]["breakpoints"].At(0)["verified"].AsBool());
  session.Handle(Request(4, "configurationDone", "{}"));

  const lsp::Json first = editor.WaitFor("event", "stopped");
  WOLANG_EXPECT_EQ(first["body"]["reason"].AsString(), "breakpoint");
  session.Handle(Request(5, "stackTrace", "{\"threadId\":1}"));
  const lsp::Json trace = editor.WaitFor("response", "stackTrace");
  WOLANG_EXPECT_EQ(trace["body"]["stackFrames"].size(), 2u);
  WOLANG_EXPECT_EQ(trace["body"]["stackFrames"].At(0)["name"].AsString(), "Hurt");
  WOLANG_EXPECT_EQ(LineOf(trace, 0), 7);
  WOLANG_EXPECT_EQ(LineOf(trace, 1), 16);

  session.Handle(Request(6, "variables", "{\"variablesReference\":1}"));
  const lsp::Json locals = editor.WaitFor("response", "variables")["body"]["variables"];
  WOLANG_EXPECT_EQ(locals.size(), 2u);
  WOLANG_EXPECT_EQ(locals.At(0)["name"].AsString(), "player");
  WOLANG_EXPECT_EQ(locals.At(1)["value"].AsString(), "0");
  const int64_t fields = locals.At(0)["variablesReference"].AsInt();
  WOLANG_EXPECT(fields != 0);
  session.Handle(Request(7, "variables", "{\"variablesReference\":" + std::to_string(fields) + "}"));
  const lsp::Json player = editor.WaitFor("response", "variables")["body"]["variables"];
  WOLANG_EXPECT_EQ(player.At(0)["name"].AsString(), "hp");
  WOLANG_EXPECT_EQ(player.At(0)["value"].AsString(), "10");

  session.Handle(Request(8, "next", "{\"threadId\":1}"));
  const lsp::Json stepped = editor.WaitFor("event", "stopped");
  WOLANG_EXPECT_EQ(stepped["body"]["reason"].AsString(), "step");
  session.Handle(Request(9, "stackTrace", "{\"threadId\":1}"));
  WOLANG_EXPECT_EQ(LineOf(editor.WaitFor("response", "stackTrace"), 0), 8);

  session.Handle(Request(10, "stepOut", "{\"threadId\":1}"));
  editor.WaitFor("event", "stopped");
  session.Handle(Request(11, "stackTrace", "{\"threadId\":1}"));
  const lsp::Json out = editor.WaitFor("response", "stackTrace");
  WOLANG_EXPECT_EQ(out["body"]["stackFrames"].size(), 1u);
  WOLANG_EXPECT_EQ(LineOf(out, 0), 16);

  session.Handle(Request(12, "setBreakpoints", "{\"source\":{\"path\":\"debugged.wo\"},\"breakpoints\":[]}"));
  session.Handle(Request(13, "continue", "{\"threadId\":1}"));
  const lsp::Json exited = editor.WaitFor("event", "exited");
  WOLANG_EXPECT_EQ(exited["body"]["exitCode"].AsInt(), 26);
  editor.WaitFor("event", "terminated");
  session.Handle(Request(14, "disconnect", "{}"));
  WOLANG_EXPECT(session.finished());
}

}  // namespace debug
}  // namespace wolang
