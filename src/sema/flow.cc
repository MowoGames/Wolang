// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/sema/flow.h"

#include <string>
#include <vector>

namespace wolang {
namespace {

constexpr std::string_view kUsedAfterMove = "E0122";
constexpr std::string_view kUsedBeforeWritten = "E0123";

// What a statement does to the flow after it.
enum class Flow : uint8_t {
  // Control carries on to whatever follows.
  kFalls,
  // It does not: a `return`, a `break`, a `continue`, or a block that ends
  // in one. A branch that diverges says nothing about what is initialized
  // afterwards, because there is no afterwards on that path.
  kDiverges,
};

// What is known about a local at a point in the walk.
enum class State : uint8_t {
  // Declared with no value, and no path here has written one.
  kUnwritten,
  kLive,
  // Its value was handed to something else. A type that owns a resource
  // cannot be in two places, so the name it was in no longer holds one.
  kMoved,
};

struct Local {
  Symbol name;
  State state = State::kUnwritten;
  // Reported once: a variable used three times after being moved is one
  // mistake, not three.
  bool reported = false;
};

class Checker {
 public:
  Checker(const Ast& ast,
          const StringInterner& interner,
          const TypeStore& types,
          const TypedBody& typed,
          DiagnosticEngine* diagnostics)
      : ast_(ast),
        interner_(interner),
        types_(types),
        typed_(typed),
        diagnostics_(diagnostics) {}

  void Run(ItemId item) {
    if (!ast_.Contains(item)) {
      return;
    }
    const Item& declaration = ast_.Get(item);
    for (ItemId member : ast_.Get(declaration.members)) {
      Run(member);
    }
    if (!declaration.body.is_valid()) {
      return;
    }
    const size_t depth = locals_.size();
    Expression(declaration.body);
    locals_.resize(depth);
  }

 private:
  Local* Find(Symbol name) {
    for (auto it = locals_.rbegin(); it != locals_.rend(); ++it) {
      if (it->name == name) {
        return &*it;
      }
    }
    return nullptr;
  }

  // A type that owns a resource is moved rather than copied: it cannot be
  // in two places at once. Which use is a move and which is a borrow falls
  // out of the type the context wanted -- handing a `String` where a
  // `String` is expected moves it; where a `str` is expected it is the
  // implicit borrow of D15b.
  bool IsOwning(TypeId type) const {
    switch (types_.KindOf(type)) {
      case TypeKind::kString:
        return true;
      default:
        return false;
    }
  }

  void Report(Symbol name, SourceSpan span) {
    Local* local = Find(name);
    if (local == nullptr || local->reported) {
      return;
    }
    if (local->state == State::kUnwritten) {
      local->reported = true;
      std::string message = "`";
      message.append(interner_.Text(name));
      message.append("` is used before anything is written to it");
      diagnostics_->Error(kUsedBeforeWritten, std::move(message))
          .Primary(span, "read here")
          .Note("give it a value where it is declared, or on every path "
                "that reaches this point");
      return;
    }
    if (local->state == State::kMoved) {
      local->reported = true;
      std::string message = "`";
      message.append(interner_.Text(name));
      message.append("` was moved out of, so it no longer holds a value");
      diagnostics_->Error(kUsedAfterMove, std::move(message))
          .Primary(span, "used here")
          .Note("a type that owns a resource cannot be in two places; "
                "borrow it instead, or give this name a new value");
    }
  }

  // Whether the use at |id| was accepted as a borrow rather than taken by
  // value. Only the checker knows, because only it knows which conversion
  // rule it applied.
  bool IsBorrow(ExprId id) const {
    for (ExprId borrow : typed_.borrows) {
      if (borrow == id) {
        return true;
      }
    }
    return false;
  }

  // Marks |name| moved when the use at |id| hands the value away.
  void MaybeMove(Symbol name, ExprId id) {
    Local* local = Find(name);
    if (local == nullptr || local->state != State::kLive) {
      return;
    }
    if (!IsOwning(typed_.TypeOf(id)) || IsBorrow(id)) {
      return;
    }
    local->state = State::kMoved;
  }

  // ------------------------------------------------------- statements ----

  Flow Statements(AstRange<StmtId> range) {
    Flow flow = Flow::kFalls;
    for (StmtId id : ast_.Get(range)) {
      if (Statement(id) == Flow::kDiverges) {
        flow = Flow::kDiverges;
      }
    }
    return flow;
  }

  Flow Statement(StmtId id) {
    if (!ast_.Contains(id)) {
      return Flow::kFalls;
    }
    const Stmt& stmt = ast_.Get(id);
    switch (stmt.kind) {
      case StmtKind::kLet:
      case StmtKind::kVar: {
        if (stmt.expr.is_valid()) {
          Expression(stmt.expr);
        }
        if (!ast_.Contains(stmt.pattern)) {
          return Flow::kFalls;
        }
        const Pattern& pattern = ast_.Get(stmt.pattern);
        if (pattern.kind != PatternKind::kBinding) {
          return Flow::kFalls;
        }
        locals_.push_back(
            Local{pattern.name,
                  stmt.expr.is_valid() ? State::kLive : State::kUnwritten,
                  false});
        return Flow::kFalls;
      }

      case StmtKind::kReturn:
        if (stmt.expr.is_valid()) {
          Expression(stmt.expr);
        }
        return Flow::kDiverges;

      case StmtKind::kBreak:
      case StmtKind::kContinue:
        if (stmt.expr.is_valid()) {
          Expression(stmt.expr);
        }
        return Flow::kDiverges;

      case StmtKind::kExpr:
      case StmtKind::kDefer:
        return stmt.expr.is_valid() ? Expression(stmt.expr) : Flow::kFalls;

      default:
        return Flow::kFalls;
    }
  }

  // ------------------------------------------------------ expressions ----

  // The name an assignment writes, or an invalid symbol when the target is
  // anything else. Only a whole variable counts as written: a field of an
  // uninitialized struct is not a place to write to.
  Symbol AssignedName(ExprId id) const {
    if (!ast_.Contains(id)) {
      return Symbol();
    }
    const Expr& target = ast_.Get(id);
    return target.kind == ExprKind::kPath ? target.name : Symbol();
  }

  Flow Expression(ExprId id) {
    if (!ast_.Contains(id)) {
      return Flow::kFalls;
    }
    const Expr& expr = ast_.Get(id);
    switch (expr.kind) {
      case ExprKind::kPath:
        Report(expr.name, expr.span);
        MaybeMove(expr.name, id);
        return Flow::kFalls;

      case ExprKind::kAssign: {
        // The value first: `x = x + 1` reads before it writes.
        Expression(expr.rhs);
        const Symbol name = AssignedName(expr.lhs);
        if (!name.is_valid()) {
          Expression(expr.lhs);
          return Flow::kFalls;
        }
        if (expr.op != TokenKind::kEq) {
          // A compound assignment reads the target as well.
          Report(name, expr.span);
        }
        if (Local* local = Find(name)) {
          // Assigning to a name that was moved out of gives it a value
          // again, which is the whole of what makes `var` and moves get
          // along.
          local->state = State::kLive;
        }
        return Flow::kFalls;
      }

      case ExprKind::kBlock: {
        const size_t depth = locals_.size();
        const Flow flow = Statements(expr.stmts);
        locals_.resize(depth);
        return flow;
      }

      case ExprKind::kIf: {
        Expression(expr.lhs);
        const size_t depth = locals_.size();
        // What each branch wrote, so that "written on every path" is an
        // intersection rather than whatever the last branch did.
        std::vector<State> before = Written();
        const Flow then_flow = Expression(expr.rhs);
        std::vector<State> after_then = Written();
        Restore(before);
        const Flow else_flow =
            expr.extra.is_valid() ? Expression(expr.extra) : Flow::kFalls;
        std::vector<State> after_else = Written();

        locals_.resize(depth);
        if (then_flow == Flow::kDiverges && else_flow == Flow::kDiverges) {
          Restore(Union(after_then, after_else));
          return Flow::kDiverges;
        }
        // A branch that diverges constrains nothing: there is no path from
        // it to here.
        if (then_flow == Flow::kDiverges) {
          Restore(after_else);
          return Flow::kFalls;
        }
        if (else_flow == Flow::kDiverges) {
          Restore(after_then);
          return Flow::kFalls;
        }
        if (!expr.extra.is_valid()) {
          // No `else`, so nothing the then-branch wrote is certain -- but
          // a move it may have made is: the value is gone either way.
          // Writes intersect and moves union, which is what `Intersect`
          // does.
          Restore(Intersect(after_then, before));
          return Flow::kFalls;
        }
        Restore(Intersect(after_then, after_else));
        return Flow::kFalls;
      }

      case ExprKind::kMatch: {
        Expression(expr.lhs);
        const std::vector<State> before = Written();
        std::vector<State> common;
        bool any_falls = false;
        for (MatchArmId arm_id : ast_.Get(expr.arms)) {
          const MatchArm& arm = ast_.Get(arm_id);
          Restore(before);
          if (arm.guard.is_valid()) {
            Expression(arm.guard);
          }
          const size_t depth = locals_.size();
          const Flow flow = Expression(arm.body);
          locals_.resize(depth);
          if (flow == Flow::kDiverges) {
            continue;
          }
          common = any_falls ? Intersect(common, Written()) : Written();
          any_falls = true;
        }
        Restore(any_falls ? common : before);
        return any_falls ? Flow::kFalls : Flow::kDiverges;
      }

      case ExprKind::kWhile:
      case ExprKind::kFor:
      case ExprKind::kForeach:
        Expression(expr.lhs);
        Body(expr.rhs);
        return Flow::kFalls;

      case ExprKind::kLoop:
        // A `loop` is left by `break`, and a `break` carries what the body
        // wrote with it -- which this does not track. Conservative either
        // way: nothing the body wrote is known afterwards.
        Body(expr.rhs);
        return Flow::kFalls;

      case ExprKind::kUnsafe:
      case ExprKind::kRegion:
      case ExprKind::kAwait:
      case ExprKind::kSpawn:
      case ExprKind::kTry:
      case ExprKind::kCast:
      case ExprKind::kUnary:
        return Expression(expr.lhs);

      case ExprKind::kField:
      case ExprKind::kOptionalField:
        return Expression(expr.lhs);

      case ExprKind::kBinary:
      case ExprKind::kRange:
      case ExprKind::kIndex:
        Expression(expr.lhs);
        Expression(expr.rhs);
        return Flow::kFalls;

      case ExprKind::kCall:
      case ExprKind::kStructLiteral:
        Expression(expr.lhs);
        for (ExprId argument : ast_.Get(expr.args)) {
          Expression(argument);
        }
        return Flow::kFalls;

      case ExprKind::kArrayLiteral:
      case ExprKind::kTupleLiteral:
      case ExprKind::kStringLiteral:
        for (ExprId element : ast_.Get(expr.args)) {
          Expression(element);
        }
        return Flow::kFalls;

      case ExprKind::kClosure:
        // A closure body runs later, and whether it runs at all is not a
        // question this walk can answer.
        return Flow::kFalls;

      default:
        return Flow::kFalls;
    }
  }

  // A loop body, walked twice.
  //
  // Once is not enough for a move: the second iteration would move a value
  // the first one already moved, and that is the mistake worth reporting.
  // The body may also not run at all, so nothing it wrote is known
  // afterwards -- while anything it moved is gone.
  void Body(ExprId body) {
    const std::vector<State> before = Written();
    const size_t depth = locals_.size();
    Expression(body);
    const std::vector<State> after = Written();
    locals_.resize(depth);

    bool moved_something = false;
    for (size_t i = 0; i < after.size() && i < before.size(); ++i) {
      moved_something = moved_something ||
                        (after[i] == State::kMoved &&
                         before[i] != State::kMoved);
    }
    if (moved_something) {
      Restore(Intersect(after, before));
      Expression(body);
      locals_.resize(depth);
    }
    Restore(Intersect(after, before));
  }

  // ---------------------------------------------------- written states ----

  std::vector<State> Written() const {
    std::vector<State> state;
    state.reserve(locals_.size());
    for (const Local& local : locals_) {
      state.push_back(local.state);
    }
    return state;
  }

  void Restore(const std::vector<State>& state) {
    for (size_t i = 0; i < locals_.size() && i < state.size(); ++i) {
      locals_[i].state = state[i];
    }
  }

  // What is true after a join: written only where both paths wrote, and
  // moved where either did -- a value moved on one path is gone.
  static std::vector<State> Intersect(const std::vector<State>& lhs,
                                      const std::vector<State>& rhs) {
    std::vector<State> result;
    for (size_t i = 0; i < lhs.size(); ++i) {
      const State other = i < rhs.size() ? rhs[i] : State::kUnwritten;
      if (lhs[i] == State::kMoved || other == State::kMoved) {
        result.push_back(State::kMoved);
      } else if (lhs[i] == State::kLive && other == State::kLive) {
        result.push_back(State::kLive);
      } else {
        result.push_back(State::kUnwritten);
      }
    }
    return result;
  }

  // What is true after two paths that both diverge: nothing joins, so the
  // most that is known is what either path established.
  static std::vector<State> Union(const std::vector<State>& lhs,
                                  const std::vector<State>& rhs) {
    std::vector<State> result;
    for (size_t i = 0; i < lhs.size(); ++i) {
      const State other = i < rhs.size() ? rhs[i] : State::kUnwritten;
      result.push_back(lhs[i] == State::kLive || other == State::kLive
                           ? State::kLive
                           : lhs[i]);
    }
    return result;
  }

  const Ast& ast_;
  const StringInterner& interner_;
  const TypeStore& types_;
  const TypedBody& typed_;
  DiagnosticEngine* diagnostics_;
  std::vector<Local> locals_;
};

}  // namespace

void CheckFlow(const Ast& ast,
               const StringInterner& interner,
               const TypeStore& types,
               const TypedBody& typed,
               ItemId item,
               DiagnosticEngine* diagnostics) {
  Checker checker(ast, interner, types, typed, diagnostics);
  checker.Run(item);
}

}  // namespace wolang
