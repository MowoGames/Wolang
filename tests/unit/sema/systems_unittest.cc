// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/sema/systems.h"

#include <string>

#include "harness/test.h"
#include "wolang/compiler/database.h"

namespace wolang {
namespace {

// The access graph is read off the declarations, so a fixture needs no more
// than a file: no world, no entities, nothing running.
class Fixture {
 public:
  explicit Fixture(std::string_view code) {
    file_ = db_.AddFile("<test>", std::string(code));
  }

  std::string Codes() {
    std::string result;
    const std::vector<Diagnostic>* diagnostics = db_.Diagnostics(file_);
    if (diagnostics == nullptr) {
      return result;
    }
    for (const Diagnostic& diagnostic : *diagnostics) {
      if (diagnostic.severity != Severity::kError) {
        continue;
      }
      if (!result.empty()) {
        result.push_back(',');
      }
      result.append(diagnostic.code);
    }
    return result;
  }

  // The schedule as "A, B | C": stages separated by a bar, in order.
  std::string Stages() {
    AccessGraph graph;
    const ParsedFile* parsed = db_.Parse(file_);
    if (parsed == nullptr) {
      return "<not parsed>";
    }
    CollectSystems(db_.interner(), parsed->ast, file_.index, &graph);
    DiagnosticEngine ignored;
    ScheduleSystems(db_.interner(), &graph, &ignored);
    std::string result;
    for (const std::vector<Symbol>& stage : graph.stages) {
      if (!result.empty()) {
        result.append(" | ");
      }
      bool first = true;
      for (Symbol name : stage) {
        if (!first) {
          result.append(", ");
        }
        first = false;
        result.append(db_.interner().Text(name));
      }
    }
    return result;
  }

  // What one system accesses, as "mut A, B".
  std::string Access(std::string_view system) {
    AccessGraph graph;
    const ParsedFile* parsed = db_.Parse(file_);
    if (parsed == nullptr) {
      return "<not parsed>";
    }
    CollectSystems(db_.interner(), parsed->ast, file_.index, &graph);
    const SystemAccess* found =
        graph.Find(db_.interner().Intern(system));
    if (found == nullptr) {
      return "<no such system>";
    }
    std::string result;
    for (const ComponentAccess& access : found->components) {
      if (!result.empty()) {
        result.append(", ");
      }
      if (access.access == Access::kWrite) {
        result.append("mut ");
      }
      result.append(db_.interner().Text(access.component));
    }
    return result;
  }

 private:
  Database db_;
  FileId file_;
};

WOLANG_TEST(SystemTest, ReadsAccessFromTheQuery) {
  // A filter is a read: asking whether an entity has a tag is reading the
  // fact that it does, and two systems that both only ask can run at the
  // same time.
  Fixture f(R"(
component Velocity {
  var x: float = 0.0
}

component Position {
  var x: float = 0.0
}

tag Falling

system Gravity(dt: float, q: Query<mut Velocity, with Falling>) {
  foreach v in q {
    v.x = v.x - dt
  }
}

system Report(q: Query<Position, without Falling>) {
  foreach p in q {
    const _ = p.x
  }
}
)");
  WOLANG_EXPECT_EQ(f.Codes(), "");
  WOLANG_EXPECT_EQ(f.Access("Gravity"), "mut Velocity, Falling");
  WOLANG_EXPECT_EQ(f.Access("Report"), "Position, Falling");
  // Nothing conflicts: one writes Velocity, the other reads Position, and
  // the tag is only read by both.
  WOLANG_EXPECT_EQ(f.Stages(), "Gravity, Report");
}

WOLANG_TEST(SystemTest, RejectsTwoWritersWithNoOrdering) {
  Fixture f(R"(
component Velocity {
  var x: float = 0.0
}

system Gravity(q: Query<mut Velocity>) {
  foreach v in q {
    v.x = v.x - 1.0
  }
}

system Damp(q: Query<mut Velocity>) {
  foreach v in q {
    v.x = v.x * 0.5
  }
}
)");
  WOLANG_EXPECT_EQ(f.Codes(), "E0127");
}

WOLANG_TEST(SystemTest, AWriterAndAReaderAlsoConflict) {
  // Reading what another system is writing is a race, not a shortcut.
  Fixture f(R"(
component Velocity {
  var x: float = 0.0
}

system Gravity(q: Query<mut Velocity>) {
  foreach v in q {
    v.x = v.x - 1.0
  }
}

system Draw(q: Query<Velocity>) {
  foreach v in q {
    const _ = v.x
  }
}
)");
  WOLANG_EXPECT_EQ(f.Codes(), "E0127");
}

WOLANG_TEST(SystemTest, AnOrderingPutsThemInSeparateStages) {
  Fixture f(R"(
component Velocity {
  var x: float = 0.0
}

system Gravity(q: Query<mut Velocity>) {
  foreach v in q {
    v.x = v.x - 1.0
  }
}

@After(Gravity)
system Damp(q: Query<mut Velocity>) {
  foreach v in q {
    v.x = v.x * 0.5
  }
}
)");
  WOLANG_EXPECT_EQ(f.Codes(), "");
  WOLANG_EXPECT_EQ(f.Stages(), "Gravity | Damp");
}

WOLANG_TEST(SystemTest, BeforeIsTheSameEdgeFromTheOtherEnd) {
  Fixture f(R"(
component Velocity {
  var x: float = 0.0
}

@Before(Damp)
system Gravity(q: Query<mut Velocity>) {
  foreach v in q {
    v.x = v.x - 1.0
  }
}

system Damp(q: Query<mut Velocity>) {
  foreach v in q {
    v.x = v.x * 0.5
  }
}
)");
  WOLANG_EXPECT_EQ(f.Codes(), "");
  WOLANG_EXPECT_EQ(f.Stages(), "Gravity | Damp");
}

WOLANG_TEST(SystemTest, ReportsAnOrderingThatNamesNothing) {
  Fixture f(R"(
component Velocity {
  var x: float = 0.0
}

@After(Grabity)
system Damp(q: Query<mut Velocity>) {
  foreach v in q {
    v.x = v.x * 0.5
  }
}
)");
  WOLANG_EXPECT_EQ(f.Codes(), "E0128");
}

WOLANG_TEST(SystemTest, ReportsACycleOfOrderings) {
  Fixture f(R"(
component Velocity {
  var x: float = 0.0
}

@After(Damp)
system Gravity(q: Query<mut Velocity>) {
  foreach v in q {
    v.x = v.x - 1.0
  }
}

@After(Gravity)
system Damp(q: Query<mut Velocity>) {
  foreach v in q {
    v.x = v.x * 0.5
  }
}
)");
  WOLANG_EXPECT_EQ(f.Codes(), "E0129");
}

}  // namespace
}  // namespace wolang
