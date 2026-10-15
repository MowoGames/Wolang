// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#ifndef WOLANG_SEMA_SYSTEMS_H_
#define WOLANG_SEMA_SYSTEMS_H_

#include <cstdint>
#include <vector>

#include "wolang/base/diagnostic.h"
#include "wolang/base/string_interner.h"
#include "wolang/syntax/ast.h"

namespace wolang {

// What the systems of a module do to the components they name, and in what
// order they may run.
//
// A system declares its access in its signature -- `Query<mut Velocity,
// with Falling>` writes Velocity and reads Falling -- so the compiler can
// work out which systems touch the same data before any of them runs. Two
// that do, where one writes, cannot run at the same time; everything else
// can. That is the whole argument for an ECS being in the language rather
// than in a library: the schedule is a property of the declarations, and
// nothing in a library can see them all.

enum class Access : uint8_t { kRead, kWrite };

struct ComponentAccess {
  Symbol component;
  Access access = Access::kRead;

  friend bool operator==(const ComponentAccess& lhs,
                         const ComponentAccess& rhs) {
    return lhs.component == rhs.component && lhs.access == rhs.access;
  }
};

struct SystemAccess {
  Symbol name;
  // Where the declaration is, the way everything else keyed by declaration
  // is: the file it came from and its index in that file's item tree.
  uint32_t file = 0;
  uint32_t item = 0;
  SourceSpan span;
  std::vector<ComponentAccess> components;

  bool Writes(Symbol component) const;
  bool Touches(Symbol component) const;

  friend bool operator==(const SystemAccess& lhs, const SystemAccess& rhs) {
    return lhs.name == rhs.name && lhs.file == rhs.file &&
           lhs.item == rhs.item && lhs.components == rhs.components;
  }
};

// One stated ordering: |before| runs before |after|. `@After(X)` and
// `@Before(X)` are the same edge written from the two ends, so both become
// this and the scheduler has one direction to walk.
struct OrderEdge {
  Symbol before;
  Symbol after;
  // Where it was written, for the diagnostic when it names nothing.
  SourceSpan span;

  friend bool operator==(const OrderEdge& lhs, const OrderEdge& rhs) {
    return lhs.before == rhs.before && lhs.after == rhs.after;
  }
};

struct AccessGraph {
  std::vector<SystemAccess> systems;
  std::vector<OrderEdge> edges;

  friend bool operator==(const AccessGraph& lhs, const AccessGraph& rhs) {
    return lhs.systems == rhs.systems && lhs.edges == rhs.edges &&
           lhs.stages == rhs.stages;
  }
  // One per stage, in order. Everything in a stage may run at the same
  // time as everything else in it, because nothing in it writes what
  // another reads or writes.
  std::vector<std::vector<Symbol>> stages;

  const SystemAccess* Find(Symbol name) const;
};

// Reads every `system` of one file into |graph|.
void CollectSystems(const StringInterner& interner,
                    const Ast& ast,
                    uint32_t file,
                    AccessGraph* graph);

// Works out the stages, and reports what cannot be scheduled: two systems
// that touch the same component where one writes and neither is ordered
// against the other (`E0127`), an ordering that names something that is
// not a system (`E0128`), and a cycle of orderings (`E0129`).
void ScheduleSystems(const StringInterner& interner,
                     AccessGraph* graph,
                     DiagnosticEngine* diagnostics);

}  // namespace wolang

#endif  // WOLANG_SEMA_SYSTEMS_H_
