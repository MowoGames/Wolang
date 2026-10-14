// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/sema/systems.h"

#include <algorithm>
#include <string>

#include "wolang/compiler/item_tree.h"

namespace wolang {
namespace {

constexpr std::string_view kUnorderedSystems = "E0127";
constexpr std::string_view kUnknownSystem = "E0128";
constexpr std::string_view kCyclicOrder = "E0129";

// The component a query argument names, and whether it is written.
//
// `mut T` is a write. `with T` and `without T` are reads: a filter asks
// whether an entity has the component, which is reading the fact that it
// does. Anything else is a read of the component itself.
void CollectArgument(const Ast& ast,
                     TypeRefId id,
                     std::vector<ComponentAccess>* into) {
  if (!ast.Contains(id)) {
    return;
  }
  const TypeRef& argument = ast.Get(id);
  const bool writes = argument.kind == TypeRefKind::kMut;
  TypeRefId named = id;
  if (argument.kind == TypeRefKind::kMut ||
      argument.kind == TypeRefKind::kWith ||
      argument.kind == TypeRefKind::kWithout) {
    named = argument.element;
  }
  if (!ast.Contains(named)) {
    return;
  }
  const std::span<const Symbol> path = ast.Get(ast.Get(named).path);
  if (path.empty()) {
    return;
  }
  const Symbol component = path.back();
  for (ComponentAccess& existing : *into) {
    if (existing.component == component) {
      // The stronger access wins: naming a component twice, once as a
      // filter and once as `mut`, is a write.
      if (writes) {
        existing.access = Access::kWrite;
      }
      return;
    }
  }
  into->push_back(
      ComponentAccess{component, writes ? Access::kWrite : Access::kRead});
}

}  // namespace

bool SystemAccess::Writes(Symbol component) const {
  for (const ComponentAccess& access : components) {
    if (access.component == component) {
      return access.access == Access::kWrite;
    }
  }
  return false;
}

bool SystemAccess::Touches(Symbol component) const {
  for (const ComponentAccess& access : components) {
    if (access.component == component) {
      return true;
    }
  }
  return false;
}

const SystemAccess* AccessGraph::Find(Symbol name) const {
  for (const SystemAccess& system : systems) {
    if (system.name == name) {
      return &system;
    }
  }
  return nullptr;
}

void CollectSystems(const StringInterner& interner,
                    const Ast& ast,
                    uint32_t file,
                    AccessGraph* graph) {
  const ItemTree tree = BuildItemTree(interner, ast);
  for (uint32_t index = 0; index < tree.entries.size(); ++index) {
    if (tree.entries[index].kind != ItemKind::kSystem) {
      continue;
    }
    const ItemId id = ResolveItem(ast, index);
    if (!ast.Contains(id)) {
      continue;
    }
    const Item& item = ast.Get(id);

    SystemAccess system;
    system.name = item.name;
    system.file = file;
    system.item = index;
    system.span = item.name_span;

    for (ParamId param : ast.Get(item.params)) {
      if (!ast.Contains(param)) {
        continue;
      }
      const TypeRefId annotation = ast.Get(param).type;
      if (!ast.Contains(annotation)) {
        continue;
      }
      const TypeRef& declared = ast.Get(annotation);
      // Only a query says anything about components; `dt: float` is a
      // value the caller passes and no access at all.
      if (declared.kind != TypeRefKind::kPath || declared.args.empty()) {
        continue;
      }
      const std::span<const Symbol> path = ast.Get(declared.path);
      if (path.empty() || interner.Text(path.back()) != "Query") {
        continue;
      }
      for (TypeRefId argument : ast.Get(declared.args)) {
        CollectArgument(ast, argument, &system.components);
      }
    }

    // `@After(X)` and `@Before(X)`: the one ordering a system states
    // about itself, in the two directions it reads naturally. Both become
    // an edge from the one that runs first.
    for (AttributeId attribute : ast.Get(item.attributes)) {
      if (!ast.Contains(attribute)) {
        continue;
      }
      const Attribute& written = ast.Get(attribute);
      const std::string_view name = interner.Text(written.name);
      if (name != "After" && name != "Before") {
        continue;
      }
      for (ExprId argument : ast.Get(written.args)) {
        if (!ast.Contains(argument) ||
            ast.Get(argument).kind != ExprKind::kPath) {
          continue;
        }
        const Symbol other = ast.Get(argument).name;
        const SourceSpan where = ast.Get(argument).span;
        if (name == "After") {
          graph->edges.push_back(OrderEdge{other, system.name, where});
        } else {
          graph->edges.push_back(OrderEdge{system.name, other, where});
        }
      }
    }

    graph->systems.push_back(std::move(system));
  }
}

void ScheduleSystems(const StringInterner& interner,
                     AccessGraph* graph,
                     DiagnosticEngine* diagnostics) {
  const size_t count = graph->systems.size();
  if (count == 0) {
    return;
  }

  // An ordering that names nothing is worth saying so about: a renamed
  // system otherwise silently stops being ordered against anything.
  std::vector<std::vector<size_t>> after(count);
  for (const OrderEdge& edge : graph->edges) {
    const SystemAccess* before = graph->Find(edge.before);
    const SystemAccess* later = graph->Find(edge.after);
    if (before == nullptr || later == nullptr) {
      if (diagnostics != nullptr) {
        const Symbol missing =
            before == nullptr ? edge.before : edge.after;
        std::string message = "no system named `";
        message.append(interner.Text(missing));
        message.append("` to order against");
        diagnostics->Error(kUnknownSystem, std::move(message))
            .Primary(edge.span);
      }
      continue;
    }
    const size_t from =
        static_cast<size_t>(before - graph->systems.data());
    const size_t to = static_cast<size_t>(later - graph->systems.data());
    if (from != to) {
      after[to].push_back(from);
    }
  }

  // Each system lands in the first stage after everything it waits for,
  // which is the longest path to it. Found by iteration rather than by a
  // topological walk, because the cycle has to be reported rather than
  // walked into.
  std::vector<uint32_t> stage(count, 0);
  for (size_t round = 0; round <= count; ++round) {
    bool moved = false;
    for (size_t i = 0; i < count; ++i) {
      for (size_t dependency : after[i]) {
        if (stage[i] <= stage[dependency]) {
          stage[i] = stage[dependency] + 1;
          moved = true;
        }
      }
    }
    if (!moved) {
      break;
    }
    if (round == count && diagnostics != nullptr) {
      // Still moving after one round per system: the orderings come back
      // to where they started.
      std::string message = "the orderings between systems form a cycle";
      diagnostics->Error(kCyclicOrder, std::move(message))
          .Primary(graph->systems.front().span);
      return;
    }
  }

  // Two systems in one stage that touch the same component, where one
  // writes it, cannot run together -- and nothing said which goes first.
  for (size_t i = 0; i < count; ++i) {
    for (size_t j = i + 1; j < count; ++j) {
      if (stage[i] != stage[j]) {
        continue;
      }
      for (const ComponentAccess& access : graph->systems[i].components) {
        if (!graph->systems[j].Touches(access.component)) {
          continue;
        }
        const bool writes = access.access == Access::kWrite ||
                            graph->systems[j].Writes(access.component);
        if (!writes || diagnostics == nullptr) {
          continue;
        }
        std::string message = "`";
        message.append(interner.Text(graph->systems[i].name));
        message.append("` and `");
        message.append(interner.Text(graph->systems[j].name));
        message.append("` both use `");
        message.append(interner.Text(access.component));
        message.append("` and one of them writes it, with nothing saying "
                       "which runs first");
        diagnostics->Error(kUnorderedSystems, std::move(message))
            .Primary(graph->systems[j].span)
            .Secondary(graph->systems[i].span, "the other one is here");
        break;
      }
    }
  }

  uint32_t stages = 0;
  for (size_t i = 0; i < count; ++i) {
    stages = std::max(stages, stage[i] + 1);
  }
  graph->stages.assign(stages, {});
  for (size_t i = 0; i < count; ++i) {
    graph->stages[stage[i]].push_back(graph->systems[i].name);
  }
}

}  // namespace wolang
