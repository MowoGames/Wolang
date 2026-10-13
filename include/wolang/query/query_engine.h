// Copyright 2026 The Wolang Authors. All rights reserved.

#ifndef WOLANG_QUERY_QUERY_ENGINE_H_
#define WOLANG_QUERY_QUERY_ENGINE_H_

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wolang {

enum class QueryKind : uint16_t {
#define WOLANG_QUERY_INPUT(Name, Argument, Description) k##Name,
#define WOLANG_QUERY(Name, Argument, Description) k##Name,
#include "wolang/query/query_kind.inc"
  kCount,
};

std::string_view QueryKindName(QueryKind kind);
std::string_view QueryKindDescription(QueryKind kind);
// True for facts that are set from outside rather than computed.
bool IsInputQuery(QueryKind kind);

// Identifies one cached fact. Every query takes a single 32-bit argument -- a
// file, a module, or later a declaration -- which keeps keys to eight bytes
// and makes the dependency graph cheap to store.
struct QueryKey {
  QueryKind kind = QueryKind::kCount;
  uint32_t arg = 0;

  uint64_t Pack() const {
    return (static_cast<uint64_t>(kind) << 32) | arg;
  }

  friend bool operator==(QueryKey lhs, QueryKey rhs) {
    return lhs.kind == rhs.kind && lhs.arg == rhs.arg;
  }
  friend bool operator!=(QueryKey lhs, QueryKey rhs) { return !(lhs == rhs); }
};

// A global counter bumped whenever an input changes. Nothing is recomputed
// because the revision moved; it only tells the engine that its cached
// verification is older than the current state of the world.
using Revision = uint64_t;

// A cached value plus, optionally, a way to compare two of them.
//
// The comparison is what makes incremental compilation pay off: when a query
// is recomputed and produces a value equal to the previous one, its dependents
// stay valid. That is how editing a function body avoids retypechecking every
// caller -- the signature came out the same. Queries whose values are
// expensive or pointless to compare leave `equals` null, which makes every
// recomputation count as a change.
struct QueryResult {
  std::shared_ptr<const void> value;
  bool (*equals)(const void* lhs, const void* rhs) = nullptr;

  bool has_value() const { return value != nullptr; }
};

template <typename T>
QueryResult MakeResult(T value) {
  return QueryResult{std::make_shared<const T>(std::move(value)), nullptr};
}

// Builds a result whose recomputation can be compared with its predecessor.
// `T` must have `operator==`.
template <typename T>
QueryResult MakeComparableResult(T value) {
  return QueryResult{std::make_shared<const T>(std::move(value)),
                     +[](const void* lhs, const void* rhs) {
                       return *static_cast<const T*>(lhs) ==
                              *static_cast<const T*>(rhs);
                     }};
}

// A demand-driven cache with red-green invalidation.
//
// Computations declare their dependencies by asking for them: every Get()
// inside a running computation is recorded. When an input changes, nothing is
// eagerly invalidated. The next Get() walks the dependency graph, verifies
// each dependency, and recomputes only the queries whose inputs really moved.
class QueryEngine {
 public:
  // Computes the value for one key. The engine passes itself so the
  // computation can ask for its dependencies.
  using Computation = std::function<QueryResult(QueryEngine&, uint32_t arg)>;

  QueryEngine();
  ~QueryEngine();

  QueryEngine(const QueryEngine&) = delete;
  QueryEngine& operator=(const QueryEngine&) = delete;

  // Registers how a derived query is computed. Input queries need no
  // computation; they are supplied through SetInput.
  void Register(QueryKind kind, Computation computation);

  // Stores an input and bumps the revision. Setting an input to an equal value
  // still counts as a change: inputs have no comparison, because the point of
  // an input is that the outside world decides when it moved.
  void SetInput(QueryKey key, QueryResult result);

  // Installs a value that was computed earlier -- by a previous process, read
  // back from a cache -- together with the dependencies it was computed from.
  //
  // The caller is asserting that |result| is what the computation would
  // produce right now, and |dependencies| is what it would read. The engine
  // then treats it exactly like a value it computed itself: it stays valid
  // until one of those dependencies changes. Seeding an already-present
  // query does nothing, so a cache can never overwrite fresher knowledge.
  void Seed(QueryKey key,
            QueryResult result,
            std::vector<QueryKey> dependencies);

  // Returns the cached value, computing or verifying it as needed. Returns
  // null when the query has no registered computation and no stored input.
  const QueryResult* Get(QueryKey key);

  template <typename T>
  const T* GetAs(QueryKey key) {
    const QueryResult* result = Get(key);
    if (result == nullptr || !result->has_value()) {
      return nullptr;
    }
    return static_cast<const T*>(result->value.get());
  }

  Revision revision() const { return revision_; }

  // The revision in which this query's value last actually changed.
  Revision ChangedAt(QueryKey key) const;
  bool HasValue(QueryKey key) const;

  // Instrumentation. The tests assert on these counters, because "recomputed
  // only what was needed" is a property of the engine, not of its output.
  uint64_t ComputationCount(QueryKind kind) const;
  uint64_t total_computations() const { return total_computations_; }
  uint64_t total_verifications() const { return total_verifications_; }
  uint64_t total_hits() const { return total_hits_; }
  void ResetCounters();

  // True when a computation asked for a query that was already running. The
  // engine returns an empty result in that case rather than recursing, so a
  // cyclic definition produces a diagnostic instead of a crash.
  bool cycle_detected() const { return cycle_detected_; }
  void clear_cycle_detected() { cycle_detected_ = false; }

  size_t node_count() const { return nodes_.size(); }

 private:
  struct Node {
    QueryResult result;
    std::vector<QueryKey> dependencies;
    Revision verified_at = 0;
    Revision changed_at = 0;
    bool is_input = false;
    bool running = false;
  };

  Node* FindNode(QueryKey key) const;
  Node& EnsureNode(QueryKey key);
  void RecordDependency(QueryKey key);

  Revision revision_ = 1;
  // unique_ptr keeps node addresses stable across the recursive Get() calls
  // that insert new nodes while a computation is running.
  std::unordered_map<uint64_t, std::unique_ptr<Node>> nodes_;
  std::array<Computation, static_cast<size_t>(QueryKind::kCount)>
      computations_;
  // One frame per running computation, collecting the keys it asked for.
  std::vector<std::vector<QueryKey>> frames_;

  std::array<uint64_t, static_cast<size_t>(QueryKind::kCount)> counters_{};
  uint64_t total_computations_ = 0;
  uint64_t total_verifications_ = 0;
  uint64_t total_hits_ = 0;
  bool cycle_detected_ = false;
};

}  // namespace wolang

template <>
struct std::hash<wolang::QueryKey> {
  size_t operator()(wolang::QueryKey key) const noexcept {
    return std::hash<uint64_t>()(key.Pack());
  }
};

#endif  // WOLANG_QUERY_QUERY_ENGINE_H_
