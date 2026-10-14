// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/sema/initialization.h"

#include <string>
#include <vector>

namespace wolang {
namespace {

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

struct Local {
  Symbol name;
  bool written = false;
  // Reported once: a variable used three times before it is written is one
  // mistake, not three.
  bool reported = false;
};

class Checker {
 public:
  Checker(const Ast& ast,
          const StringInterner& interner,
          DiagnosticEngine* diagnostics)
      : ast_(ast), interner_(interner), diagnostics_(diagnostics) {}

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

  void Report(Symbol name, SourceSpan span) {
    Local* local = Find(name);
    if (local == nullptr || local->written || local->reported) {
      return;
    }
    local->reported = true;
    std::string message = "`";
    message.append(interner_.Text(name));
    message.append("` is used before anything is written to it");
    diagnostics_->Error(kUsedBeforeWritten, std::move(message))
        .Primary(span, "read here")
        .Note("give it a value where it is declared, or on every path that "
              "reaches this point");
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
        locals_.push_back(Local{pattern.name, stmt.expr.is_valid(), false});
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
          local->written = true;
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
        std::vector<bool> before = Written();
        const Flow then_flow = Expression(expr.rhs);
        std::vector<bool> after_then = Written();
        Restore(before);
        const Flow else_flow =
            expr.extra.is_valid() ? Expression(expr.extra) : Flow::kFalls;
        std::vector<bool> after_else = Written();

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
          // No `else`, so the then-branch may not have run.
          Restore(before);
          return Flow::kFalls;
        }
        Restore(Intersect(after_then, after_else));
        return Flow::kFalls;
      }

      case ExprKind::kMatch: {
        Expression(expr.lhs);
        const std::vector<bool> before = Written();
        std::vector<bool> common;
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
      case ExprKind::kForeach: {
        Expression(expr.lhs);
        const std::vector<bool> before = Written();
        const size_t depth = locals_.size();
        Expression(expr.rhs);
        locals_.resize(depth);
        // The body may not run at all, so nothing it wrote counts after.
        Restore(before);
        return Flow::kFalls;
      }

      case ExprKind::kLoop: {
        const size_t depth = locals_.size();
        const std::vector<bool> before = Written();
        Expression(expr.rhs);
        locals_.resize(depth);
        Restore(before);
        // A `loop` is left by `break`, and a `break` carries what the body
        // wrote with it -- which this does not track. Conservative: nothing
        // is known to be written after it.
        return Flow::kFalls;
      }

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

  // ---------------------------------------------------- written states ----

  std::vector<bool> Written() const {
    std::vector<bool> state;
    state.reserve(locals_.size());
    for (const Local& local : locals_) {
      state.push_back(local.written);
    }
    return state;
  }

  void Restore(const std::vector<bool>& state) {
    for (size_t i = 0; i < locals_.size() && i < state.size(); ++i) {
      locals_[i].written = state[i];
    }
  }

  static std::vector<bool> Intersect(const std::vector<bool>& lhs,
                                     const std::vector<bool>& rhs) {
    std::vector<bool> result;
    for (size_t i = 0; i < lhs.size(); ++i) {
      result.push_back(lhs[i] && i < rhs.size() && rhs[i]);
    }
    return result;
  }

  static std::vector<bool> Union(const std::vector<bool>& lhs,
                                 const std::vector<bool>& rhs) {
    std::vector<bool> result;
    for (size_t i = 0; i < lhs.size(); ++i) {
      result.push_back(lhs[i] || (i < rhs.size() && rhs[i]));
    }
    return result;
  }

  const Ast& ast_;
  const StringInterner& interner_;
  DiagnosticEngine* diagnostics_;
  std::vector<Local> locals_;
};

}  // namespace

void CheckInitialization(const Ast& ast,
                         const StringInterner& interner,
                         ItemId item,
                         DiagnosticEngine* diagnostics) {
  Checker checker(ast, interner, diagnostics);
  checker.Run(item);
}

}  // namespace wolang
