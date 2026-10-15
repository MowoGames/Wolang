// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/sema/flow.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wolang {
namespace {

constexpr std::string_view kEscapesRegion = "E0121";
constexpr std::string_view kUsedAfterMove = "E0122";
constexpr std::string_view kUsedBeforeWritten = "E0123";
// A function that declares a result and has a path that reaches its end
// without returning one.
constexpr std::string_view kMissingReturn = "E0132";

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
enum class Liveness : uint8_t {
  // Declared with no value, and no path here has written one.
  kUnwritten,
  kLive,
  // Its value was handed to something else. A type that owns a resource
  // cannot be in two places, so the name it was in no longer holds one.
  kMoved,
};

struct Local {
  Symbol name;
  // The region nesting this local was declared at. Zero is the function
  // body, which is a region in the only sense that matters here: a
  // reference to a local cannot outlive the frame the local is in.
  uint32_t depth = 0;
  Liveness state = Liveness::kUnwritten;
  // Reported once: a variable used three times after being moved is one
  // mistake, not three.
  bool reported = false;
};

class FlowChecker {
 public:
  FlowChecker(const Ast& ast,
          const StringInterner& interner,
          const TypeStore& types,
          const FileResolution& resolution,
          const TypedBody& typed,
          DiagnosticEngine* diagnostics)
      : ast_(ast),
        interner_(interner),
        types_(types),
        resolution_(resolution),
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
    const Flow flow = Expression(declaration.body);
    locals_.resize(depth);

    // A function that says what it gives back has to give one back on
    // every path. Without this the end of such a body returned whatever
    // the register happened to hold -- compiled code read the zero the
    // prologue left and the reference tier read nothing at all, and the
    // program was wrong in both without being told (D99).
    //
    // A declared result is the written `-> T`: a function with nothing
    // after its parameters returns nothing, and falling off the end of it
    // is the whole point.
    // An expression body gives its value back: `fun Add(..) -> int => a + b`
    // is a return, and only a block can fall off its end.
    const bool is_block =
        ast_.Contains(declaration.body) &&
        ast_.Get(declaration.body).kind == ExprKind::kBlock;
    if (declaration.kind == ItemKind::kFunction && is_block &&
        flow == Flow::kFalls && declaration.type.is_valid()) {
      std::string message = "`";
      message.append(interner_.Text(declaration.name));
      message.append("` can reach its end without returning a value");
      diagnostics_->Error(kMissingReturn, std::move(message))
          .Primary(declaration.name_span, "declared with a result")
          .Note("return on every path, or end the function with one "
                "`return`");
    }
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
    if (types_.KindOf(type) == TypeKind::kString) {
      return true;
    }
    // A type that says what to do when a value of it goes away owns
    // something, and two names cannot hold it: that is the whole reason
    // `deinit` and moves are one feature (D87).
    for (TypeId current = types_.StripWrappers(type); current.is_valid();
         current = resolution_.BaseOf(current)) {
      for (const Member& member : resolution_.MembersOf(current)) {
        if (member.kind == MemberKind::kDeinit) {
          return true;
        }
      }
    }
    return false;
  }

  // Whether anything in |body| could leave the loop it belongs to.
  //
  // A `break` inside a nested loop belongs to that one, so the walk stops
  // there -- except for a labelled `break`, which may name an outer loop
  // and is counted wherever it is. Wrong in the safe direction: a loop
  // read as one that ends is the answer this had before.
  bool HasBreak(ExprId body) const {
    if (!ast_.Contains(body)) {
      return false;
    }
    const Expr& expr = ast_.Get(body);
    switch (expr.kind) {
      case ExprKind::kWhile:
      case ExprKind::kFor:
      case ExprKind::kForeach:
      case ExprKind::kLoop:
        // A nested loop: only a `break` that names something can be for
        // this one.
        return HasLabelledBreak(expr.rhs);
      case ExprKind::kBlock: {
        for (StmtId id : ast_.Get(expr.stmts)) {
          if (!ast_.Contains(id)) {
            continue;
          }
          const Stmt& stmt = ast_.Get(id);
          if (stmt.kind == StmtKind::kBreak) {
            return true;
          }
          if (HasBreak(stmt.expr)) {
            return true;
          }
        }
        return false;
      }
      default:
        return HasBreak(expr.lhs) || HasBreak(expr.rhs) ||
               HasBreak(expr.extra);
    }
  }

  bool HasLabelledBreak(ExprId body) const {
    if (!ast_.Contains(body)) {
      return false;
    }
    const Expr& expr = ast_.Get(body);
    if (expr.kind == ExprKind::kBlock) {
      for (StmtId id : ast_.Get(expr.stmts)) {
        if (!ast_.Contains(id)) {
          continue;
        }
        const Stmt& stmt = ast_.Get(id);
        if (stmt.kind == StmtKind::kBreak && stmt.label.is_valid()) {
          return true;
        }
        if (HasLabelledBreak(stmt.expr)) {
          return true;
        }
      }
      return false;
    }
    return HasLabelledBreak(expr.lhs) || HasLabelledBreak(expr.rhs) ||
           HasLabelledBreak(expr.extra);
  }

  void Report(Symbol name, SourceSpan span) {
    Local* local = Find(name);
    if (local == nullptr || local->reported) {
      return;
    }
    if (local->state == Liveness::kUnwritten) {
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
    if (local->state == Liveness::kMoved) {
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

  // The local a `&x` refers to, or an invalid symbol when the expression is
  // not a reference to a name.
  Symbol Referent(ExprId id) const {
    if (!ast_.Contains(id)) {
      return Symbol();
    }
    const Expr& expr = ast_.Get(id);
    if (expr.kind != ExprKind::kUnary || expr.op != TokenKind::kAmp) {
      return Symbol();
    }
    if (!ast_.Contains(expr.lhs) ||
        ast_.Get(expr.lhs).kind != ExprKind::kPath) {
      return Symbol();
    }
    return ast_.Get(expr.lhs).name;
  }

  // Reports a reference that would outlive what it points into. |depth| is
  // where the reference is going: a shallower depth outlives a deeper one,
  // and -1 is outside the call altogether.
  void CheckEscape(ExprId value, int depth, SourceSpan span) {
    const Symbol referent = Referent(value);
    if (!referent.is_valid()) {
      return;
    }
    const Local* local = Find(referent);
    if (local == nullptr || static_cast<int>(local->depth) <= depth) {
      return;
    }
    std::string message = "a reference to `";
    message.append(interner_.Text(referent));
    message.append("` would outlive what it points into");
    diagnostics_->Error(kEscapesRegion, std::move(message))
        .Primary(span, "escapes here")
        .Note("a reference is second class: it may be passed inwards, "
              "never kept somewhere that outlives it");
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
  // Walks |id| as something read rather than taken: the name is used, so
  // `E0123` still applies, but nothing moves out of it.
  Flow Borrowed(ExprId id) {
    if (!ast_.Contains(id)) {
      return Flow::kFalls;
    }
    const Expr& expr = ast_.Get(id);
    if (expr.kind == ExprKind::kPath) {
      Report(expr.name, expr.span);
      return Flow::kFalls;
    }
    if (expr.kind == ExprKind::kField ||
        expr.kind == ExprKind::kOptionalField) {
      return Borrowed(expr.lhs);
    }
    if (expr.kind == ExprKind::kIndex) {
      Expression(expr.rhs);
      return Borrowed(expr.lhs);
    }
    return Expression(id);
  }

  void MaybeMove(Symbol name, ExprId id) {
    Local* local = Find(name);
    if (local == nullptr || local->state != Liveness::kLive) {
      return;
    }
    if (!IsOwning(typed_.TypeOf(id)) || IsBorrow(id)) {
      return;
    }
    local->state = Liveness::kMoved;
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
        // An array with no initializer starts as zeroes, in every tier,
        // and is not reported as unwritten. Element-level tracking is not
        // something this pass can do -- a write inside a loop may not
        // happen -- so the choice is between a buffer nobody can fill one
        // element at a time and an array that starts at zero. Every other
        // type still has to be given a value.
        const bool zero_filled =
            !stmt.expr.is_valid() && ast_.Contains(stmt.type) &&
            ast_.Get(stmt.type).kind == TypeRefKind::kArray;
        locals_.push_back(
            Local{pattern.name, depth_,
                  stmt.expr.is_valid() || zero_filled ? Liveness::kLive
                                                      : Liveness::kUnwritten,
                  false});
        return Flow::kFalls;
      }

      case StmtKind::kReturn:
        if (stmt.expr.is_valid()) {
          Expression(stmt.expr);
          // Nothing inside this function outlives the call, so a reference
          // to a local cannot be returned at all.
          CheckEscape(stmt.expr, -1, ast_.Get(stmt.expr).span);
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
  // The variable an assignment writes to, walking through the fields and
  // the elements on the way: `buf[i] = v` and `o.inner.n = v` write to
  // `buf` and to `o`. Reading the target instead -- which is what this
  // did when it only understood a bare name -- reports a variable as used
  // before it was written on the very line that writes it.
  Symbol AssignedName(ExprId id) const {
    ExprId current = id;
    while (ast_.Contains(current)) {
      const Expr& target = ast_.Get(current);
      if (target.kind == ExprKind::kField ||
          target.kind == ExprKind::kOptionalField ||
          target.kind == ExprKind::kIndex) {
        current = target.lhs;
        continue;
      }
      return target.kind == ExprKind::kPath ? target.name : Symbol();
    }
    return Symbol();
  }

  // The index expressions on the way to an assignment's target, which are
  // read even though the target is written.
  void IndicesOf(ExprId id) {
    ExprId current = id;
    while (ast_.Contains(current)) {
      const Expr& target = ast_.Get(current);
      if (target.kind == ExprKind::kIndex) {
        Expression(target.rhs);
        current = target.lhs;
        continue;
      }
      if (target.kind == ExprKind::kField ||
          target.kind == ExprKind::kOptionalField) {
        current = target.lhs;
        continue;
      }
      return;
    }
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
        // The index expressions are read even though the target is
        // written: `buf[i] = v` reads `i`.
        IndicesOf(expr.lhs);
        if (expr.op != TokenKind::kEq) {
          // A compound assignment reads the target as well.
          Report(name, expr.span);
        }
        if (Local* local = Find(name)) {
          CheckEscape(expr.rhs, static_cast<int>(local->depth),
                      expr.span);
          // Assigning to a name that was moved out of gives it a value
          // again, which is the whole of what makes `var` and moves get
          // along.
          local->state = Liveness::kLive;
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
        std::vector<Liveness> before = Written();
        const Flow then_flow = Expression(expr.rhs);
        std::vector<Liveness> after_then = Written();
        Restore(before);
        const Flow else_flow =
            expr.extra.is_valid() ? Expression(expr.extra) : Flow::kFalls;
        std::vector<Liveness> after_else = Written();

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
        const std::vector<Liveness> before = Written();
        std::vector<Liveness> common;
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
        Expression(expr.lhs);
        Body(expr.rhs);
        return Flow::kFalls;

      case ExprKind::kFor:
      case ExprKind::kForeach:
        // Walking something borrows it: the loop asks it how to be
        // walked and reads what comes back, which is reading *through*
        // the name rather than taking what it holds (D96).
        Borrowed(expr.lhs);
        Body(expr.rhs);
        return Flow::kFalls;

      case ExprKind::kLoop:
        // A `loop` is left by `break`, and a `break` carries what the body
        // wrote with it -- which this does not track. Conservative either
        // way: nothing the body wrote is known afterwards.
        Body(expr.rhs);
        // One without a `break` in it never ends, so nothing after it
        // runs: what makes `loop { }` a legitimate body for a function
        // that declares a result (D99).
        return HasBreak(expr.rhs) ? Flow::kFalls : Flow::kDiverges;

      case ExprKind::kRegion: {
        // The block's value leaves the region, so a reference to something
        // declared inside it would outlive the arena it points into. The
        // body's statements are walked here rather than through the block
        // case, because the check needs the inner names still in scope.
        ++depth_;
        const size_t base = locals_.size();
        const bool is_block = ast_.Contains(expr.lhs) &&
                              ast_.Get(expr.lhs).kind == ExprKind::kBlock;
        Flow flow = Flow::kFalls;
        if (is_block) {
          flow = Statements(ast_.Get(expr.lhs).stmts);
          CheckEscape(ValueOf(expr.lhs), static_cast<int>(depth_) - 1,
                      expr.span);
        } else {
          flow = Expression(expr.lhs);
          CheckEscape(expr.lhs, static_cast<int>(depth_) - 1, expr.span);
        }
        locals_.resize(base);
        --depth_;
        return flow;
      }

      case ExprKind::kUnsafe:
      case ExprKind::kAwait:
      case ExprKind::kSpawn:
      case ExprKind::kTry:
      case ExprKind::kCast:
      case ExprKind::kUnary:
        // `&x` and `&mut x` borrow rather than take: a reference is how an
        // owning value is lent to something, and reading it as a move made
        // the next use of the name a use after move (D109).
        if (expr.op == TokenKind::kAmp) {
          return Borrowed(expr.lhs);
        }
        return Expression(expr.lhs);

      case ExprKind::kField:
      case ExprKind::kOptionalField:
        // Reading a field borrows what it is in: `message.len` does not
        // consume the string, and treating it as a move reported the next
        // use of the name as a use after move.
        return Borrowed(expr.lhs);

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

  // The expression a block evaluates to: its last statement, when that
  // statement is an expression. What a `region { ... }` hands outwards.
  ExprId ValueOf(ExprId block) const {
    if (!ast_.Contains(block)) {
      return ExprId();
    }
    const Expr& expr = ast_.Get(block);
    if (expr.kind != ExprKind::kBlock) {
      return block;
    }
    const std::span<const StmtId> stmts = ast_.Get(expr.stmts);
    if (stmts.empty() || !ast_.Contains(stmts.back())) {
      return ExprId();
    }
    const Stmt& last = ast_.Get(stmts.back());
    return last.kind == StmtKind::kExpr ? last.expr : ExprId();
  }

  // A loop body, walked twice.
  //
  // Once is not enough for a move: the second iteration would move a value
  // the first one already moved, and that is the mistake worth reporting.
  // The body may also not run at all, so nothing it wrote is known
  // afterwards -- while anything it moved is gone.
  void Body(ExprId body) {
    const std::vector<Liveness> before = Written();
    const size_t depth = locals_.size();
    Expression(body);
    const std::vector<Liveness> after = Written();
    locals_.resize(depth);

    bool moved_something = false;
    for (size_t i = 0; i < after.size() && i < before.size(); ++i) {
      moved_something = moved_something ||
                        (after[i] == Liveness::kMoved &&
                         before[i] != Liveness::kMoved);
    }
    if (moved_something) {
      Restore(Intersect(after, before));
      Expression(body);
      locals_.resize(depth);
    }
    Restore(Intersect(after, before));
  }

  // ---------------------------------------------------- written states ----

  std::vector<Liveness> Written() const {
    std::vector<Liveness> state;
    state.reserve(locals_.size());
    for (const Local& local : locals_) {
      state.push_back(local.state);
    }
    return state;
  }

  void Restore(const std::vector<Liveness>& state) {
    for (size_t i = 0; i < locals_.size() && i < state.size(); ++i) {
      locals_[i].state = state[i];
    }
  }

  // What is true after a join: written only where both paths wrote, and
  // moved where either did -- a value moved on one path is gone.
  static std::vector<Liveness> Intersect(const std::vector<Liveness>& lhs,
                                      const std::vector<Liveness>& rhs) {
    std::vector<Liveness> result;
    for (size_t i = 0; i < lhs.size(); ++i) {
      const Liveness other = i < rhs.size() ? rhs[i] : Liveness::kUnwritten;
      if (lhs[i] == Liveness::kMoved || other == Liveness::kMoved) {
        result.push_back(Liveness::kMoved);
      } else if (lhs[i] == Liveness::kLive && other == Liveness::kLive) {
        result.push_back(Liveness::kLive);
      } else {
        result.push_back(Liveness::kUnwritten);
      }
    }
    return result;
  }

  // What is true after two paths that both diverge: nothing joins, so the
  // most that is known is what either path established.
  static std::vector<Liveness> Union(const std::vector<Liveness>& lhs,
                                  const std::vector<Liveness>& rhs) {
    std::vector<Liveness> result;
    for (size_t i = 0; i < lhs.size(); ++i) {
      const Liveness other = i < rhs.size() ? rhs[i] : Liveness::kUnwritten;
      result.push_back(lhs[i] == Liveness::kLive || other == Liveness::kLive
                           ? Liveness::kLive
                           : lhs[i]);
    }
    return result;
  }

  const Ast& ast_;
  const StringInterner& interner_;
  const TypeStore& types_;
  const FileResolution& resolution_;
  const TypedBody& typed_;
  DiagnosticEngine* diagnostics_;
  std::vector<Local> locals_;
  uint32_t depth_ = 0;
};

}  // namespace

void CheckFlow(const Ast& ast,
               const StringInterner& interner,
               const TypeStore& types,
               const FileResolution& resolution,
               const TypedBody& typed,
               ItemId item,
               DiagnosticEngine* diagnostics) {
  FlowChecker checker(ast, interner, types, resolution, typed, diagnostics);
  checker.Run(item);
}

}  // namespace wolang
