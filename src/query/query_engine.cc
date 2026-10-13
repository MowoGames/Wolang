// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/query/query_engine.h"

#include <algorithm>

#include "wolang/base/check.h"

namespace wolang {
namespace {

constexpr size_t kQueryCount = static_cast<size_t>(QueryKind::kCount);

constexpr std::array<std::string_view, kQueryCount> kNames = {
#define WOLANG_QUERY_INPUT(Name, Argument, Description) #Name,
#define WOLANG_QUERY(Name, Argument, Description) #Name,
#include "wolang/query/query_kind.inc"
};

constexpr std::array<std::string_view, kQueryCount> kDescriptions = {
#define WOLANG_QUERY_INPUT(Name, Argument, Description) Description,
#define WOLANG_QUERY(Name, Argument, Description) Description,
#include "wolang/query/query_kind.inc"
};

constexpr std::array<bool, kQueryCount> kIsInput = {
#define WOLANG_QUERY_INPUT(Name, Argument, Description) true,
#define WOLANG_QUERY(Name, Argument, Description) false,
#include "wolang/query/query_kind.inc"
};

bool InRange(QueryKind kind) {
  return static_cast<size_t>(kind) < kQueryCount;
}

}  // namespace

std::string_view QueryKindName(QueryKind kind) {
  return InRange(kind) ? kNames[static_cast<size_t>(kind)] : "<invalid>";
}

std::string_view QueryKindDescription(QueryKind kind) {
  return InRange(kind) ? kDescriptions[static_cast<size_t>(kind)] : "";
}

bool IsInputQuery(QueryKind kind) {
  return InRange(kind) && kIsInput[static_cast<size_t>(kind)];
}

QueryEngine::QueryEngine() = default;
QueryEngine::~QueryEngine() = default;

void QueryEngine::Register(QueryKind kind, Computation computation) {
  WOLANG_CHECK(InRange(kind));
  WOLANG_CHECK(!IsInputQuery(kind));
  computations_[static_cast<size_t>(kind)] = std::move(computation);
}

QueryEngine::Node* QueryEngine::FindNode(QueryKey key) const {
  auto it = nodes_.find(key.Pack());
  return it == nodes_.end() ? nullptr : it->second.get();
}

QueryEngine::Node& QueryEngine::EnsureNode(QueryKey key) {
  auto it = nodes_.find(key.Pack());
  if (it != nodes_.end()) {
    return *it->second;
  }
  auto node = std::make_unique<Node>();
  Node& reference = *node;
  nodes_.emplace(key.Pack(), std::move(node));
  return reference;
}

void QueryEngine::RecordDependency(QueryKey key) {
  if (!frames_.empty()) {
    frames_.back().push_back(key);
  }
}

void QueryEngine::SetInput(QueryKey key, QueryResult result) {
  WOLANG_CHECK(IsInputQuery(key.kind));
  ++revision_;

  Node& node = EnsureNode(key);
  node.result = std::move(result);
  node.is_input = true;
  node.dependencies.clear();
  node.changed_at = revision_;
  node.verified_at = revision_;
}

void QueryEngine::Seed(QueryKey key,
                       QueryResult result,
                       std::vector<QueryKey> dependencies) {
  WOLANG_CHECK(!IsInputQuery(key.kind));
  Node* existing = FindNode(key);
  if (existing != nullptr && existing->result.has_value()) {
    return;
  }

  Node& node = EnsureNode(key);
  node.result = std::move(result);
  node.dependencies = std::move(dependencies);
  node.verified_at = revision_;
  // The value is as old as the newest thing it was computed from, so a later
  // edit to any dependency still invalidates it.
  node.changed_at = revision_;
}

Revision QueryEngine::ChangedAt(QueryKey key) const {
  const Node* node = FindNode(key);
  // A query with no node has no history, so treat it as having just changed.
  // That is the conservative answer: it forces a recomputation rather than
  // reusing a value that may be stale.
  return node == nullptr ? revision_ : node->changed_at;
}

bool QueryEngine::HasValue(QueryKey key) const {
  const Node* node = FindNode(key);
  return node != nullptr && node->result.has_value();
}

uint64_t QueryEngine::ComputationCount(QueryKind kind) const {
  return InRange(kind) ? counters_[static_cast<size_t>(kind)] : 0;
}

void QueryEngine::ResetCounters() {
  counters_.fill(0);
  total_computations_ = 0;
  total_verifications_ = 0;
  total_hits_ = 0;
}

const QueryResult* QueryEngine::Get(QueryKey key) {
  RecordDependency(key);

  Node* node = FindNode(key);
  if (node != nullptr) {
    if (node->running) {
      // A computation asked for itself, directly or through a chain. Return
      // what is there instead of recursing forever.
      cycle_detected_ = true;
      return node->result.has_value() ? &node->result : nullptr;
    }
    if (node->verified_at == revision_) {
      ++total_hits_;
      return node->result.has_value() ? &node->result : nullptr;
    }
    if (node->is_input) {
      // Inputs only change through SetInput, which already recorded the
      // revision, so a stale verification stamp just needs refreshing.
      node->verified_at = revision_;
      ++total_hits_;
      return node->result.has_value() ? &node->result : nullptr;
    }
  }

  const Computation* computation =
      InRange(key.kind) ? &computations_[static_cast<size_t>(key.kind)]
                        : nullptr;
  if (computation == nullptr || !*computation) {
    // No way to produce this value. An input that was never set lands here.
    return node != nullptr && node->result.has_value() ? &node->result
                                                       : nullptr;
  }

  if (node != nullptr && node->result.has_value()) {
    // The value exists but has not been verified in this revision. Verify the
    // dependencies first: if none of them actually changed, the cached value
    // is still correct and nothing has to run.
    const std::vector<QueryKey> dependencies = node->dependencies;
    const Revision verified_at = node->verified_at;
    // No dependencies means the value cannot have changed: everything a
    // computation reads must be asked for through the engine.
    bool dirty = false;
    for (QueryKey dependency : dependencies) {
      Get(dependency);
      if (ChangedAt(dependency) > verified_at) {
        dirty = true;
        break;
      }
    }

    node = FindNode(key);
    if (!dirty) {
      node->verified_at = revision_;
      ++total_verifications_;
      return node->result.has_value() ? &node->result : nullptr;
    }
  }

  Node& target = EnsureNode(key);
  target.running = true;
  frames_.emplace_back();

  QueryResult fresh = (*computation)(*this, key.arg);

  std::vector<QueryKey> dependencies = std::move(frames_.back());
  frames_.pop_back();
  // The same dependency is often asked for several times in one computation.
  std::sort(dependencies.begin(), dependencies.end(),
            [](QueryKey lhs, QueryKey rhs) { return lhs.Pack() < rhs.Pack(); });
  dependencies.erase(
      std::unique(dependencies.begin(), dependencies.end()),
      dependencies.end());

  Node& stored = EnsureNode(key);
  stored.running = false;
  ++counters_[static_cast<size_t>(key.kind)];
  ++total_computations_;

  // Back-dating: a recomputation that produced the same value leaves
  // `changed_at` alone, so everything downstream stays green.
  const bool unchanged = stored.result.has_value() && fresh.has_value() &&
                         fresh.equals != nullptr &&
                         stored.result.equals == fresh.equals &&
                         fresh.equals(stored.result.value.get(),
                                      fresh.value.get());

  stored.result = std::move(fresh);
  stored.dependencies = std::move(dependencies);
  stored.verified_at = revision_;
  if (!unchanged) {
    stored.changed_at = revision_;
  }

  return stored.result.has_value() ? &stored.result : nullptr;
}

}  // namespace wolang
