// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/sema/type_check.h"

#include "wolang/sema/flow.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace wolang {
namespace {

constexpr std::string_view kMismatchedTypes = "E0101";
constexpr std::string_view kWrongArguments = "E0102";
constexpr std::string_view kUnknownName = "E0103";
constexpr std::string_view kMaybeNull = "E0104";
constexpr std::string_view kAssignToLet = "E0105";
constexpr std::string_view kNotCallable = "E0106";
constexpr std::string_view kNotIndexable = "E0107";
constexpr std::string_view kUnknownMember = "E0112";
constexpr std::string_view kInfiniteType = "E0117";
constexpr std::string_view kCyclicDeclaration = "E0118";
constexpr std::string_view kDuplicateParameter = "E0119";
constexpr std::string_view kNonExhaustive = "E0201";
constexpr std::string_view kBadOverride = "E0401";
constexpr std::string_view kMissingAbilityMethod = "E0402";
constexpr std::string_view kBoundNotSatisfied = "E0403";
constexpr std::string_view kReferenceInField = "E0120";
constexpr std::string_view kBadWeak = "E0137";
constexpr std::string_view kTypeWithoutTarget = "E0138";
constexpr std::string_view kSelfEscapesDeinit = "E0145";
// A closure that captures, started on another thread (D232). E0147, the
// module-level `var` it may not reach, is reported by the module walk.
constexpr std::string_view kThreadCaptures = "E0148";
// `extern class` is made, kept and freed by the host: no constructor and
// no fields, only methods the host binds (D213).
constexpr std::string_view kForeignClass = "E0139";
constexpr std::string_view kBadConversion = "E0124";
// Writing to a field of the receiver, which needs a receiver that is a
// reference. A method is passed its object by value today, so the write
// would land on a copy -- and losing it silently is worse than saying so.
constexpr std::string_view kSelfNotMutable = "E0125";
// A generic type built with nothing to bind its parameter: `Box(...)`
// where no field takes a `T`, which would otherwise carry a type
// parameter into a layout.
constexpr std::string_view kCannotInfer = "E0126";
// A module-level `const` or `var` with no type written.
constexpr std::string_view kModuleBindingNeedsType = "E0130";
// A `for` over something that is not a sequence and says no other way to
// walk it.
constexpr std::string_view kNotIterable = "E0131";
// A function's type parameter that its signature never mentions, so no
// call can say what it is.
constexpr std::string_view kUnboundParameter = "E0133";
// A closure whose parameter type nothing says.
constexpr std::string_view kClosureNeedsTypes = "E0134";
// A closure body naming a local of the function around it.
constexpr std::string_view kClosureCaptures = "E0135";
// A closure that captures, handed somewhere it would outlive its
// environment (D138).
constexpr std::string_view kClosureEscapes = "E0136";
constexpr std::string_view kWaitOutsideTask = "E0301";
constexpr std::string_view kTaskNotWaited = "E0302";
constexpr std::string_view kWaitNotWholeExpression = "E0303";
constexpr std::string_view kNotForeignType = "E0802";
constexpr std::string_view kForeignWithBody = "E0803";
constexpr std::string_view kBadAllow = "E0070";
constexpr std::string_view kUnusedName = "W0101";
constexpr std::string_view kUnreachableCode = "W0102";
// A state nothing leads to, and a transition to a state that is not
// there. The spec asks for both: a machine is a table, and a table with an
// unreachable row is a mistake somebody made while editing it.
constexpr std::string_view kUnreachableState = "W0109";
constexpr std::string_view kUnknownState = "E0206";
constexpr std::string_view kBadOperand = "E0108";
constexpr std::string_view kLiteralOutOfRange = "E0109";
constexpr std::string_view kDimensionMismatch = "E1001";
constexpr std::string_view kUnknownUnit = "E1002";

// One lexical scope: locals shadow whatever is outside.
// One local binding and whether anything ever read it. Tracked per scope so
// that an unused local is reported where it goes out of scope, which is also
// where the last chance to use it was.
struct LocalBinding {
  Binding binding;
  SourceSpan span;
  bool used = false;
  // `_` and names starting with it are deliberately unused.
  bool deliberate = false;
};

struct Scope {
  std::vector<LocalBinding> bindings;
};

// The range an integer type can hold, used to reject a literal that does not
// fit its destination rather than wrapping it silently.
struct IntRange {
  bool has_range = false;
  uint64_t max_magnitude = 0;
  bool is_signed = true;
};

IntRange RangeOf(const TypeStore& types, TypeId id) {
  if (types.KindOf(id) != TypeKind::kInt) {
    return IntRange{};
  }
  const uint32_t bits = types.BitWidth(id);
  const bool is_signed = types.Get(id).is_signed;
  IntRange range;
  range.has_range = true;
  range.is_signed = is_signed;
  // For a signed type the magnitude bound is 2^(n-1); the extra value on the
  // negative side is handled where the sign is known.
  range.max_magnitude = is_signed
                            ? (uint64_t{1} << (bits - 1))
                            : (bits == 64 ? UINT64_MAX
                                          : (uint64_t{1} << bits) - 1);
  return range;
}

class Checker {
 public:
  Checker(TypeStore* types,
          const StringInterner* interner,
          const FileResolution* resolution,
          const Ast& ast,
          const ItemIndexMap* item_indices)
      : types_(types),
        interner_(interner),
        resolution_(resolution),
        ast_(ast),
        resolver_(types, interner, resolution, &diagnostics_),
        item_indices_(item_indices) {
    result_.expr_types.assign(ast.expr_count(), TypeId{});
    // A name in type position counts as a use just as much as one in
    // expression position.
    resolver_.CollectUsedNames(&result_.used_names);
  }

  TypedBody Run(ItemId item, uint32_t item_index);

 private:
  // ----------------------------------------------------------- reporting --

  // The item tree index of |id|, or kNoItem when it is not known.
  uint32_t IndexOf(ItemId id) const {
    if (item_indices_ == nullptr || !id.is_valid() ||
        id.index >= item_indices_->size()) {
      return Binding::kNoItem;
    }
    return (*item_indices_)[id.index];
  }

  // True when an enclosing declaration carries `@allow` for |code|.
  bool IsAllowed(std::string_view code) const {
    for (const std::string& allowed : allowed_) {
      if (allowed == code) {
        return true;
      }
    }
    return false;
  }

  void Report(std::string_view code,
              std::string message,
              SourceSpan span,
              std::string label = {}) {
    if (IsAllowed(code)) {
      return;
    }
    diagnostics_.Error(code, std::move(message))
        .Primary(span, std::move(label));
  }

  void Warn(std::string_view code,
            std::string message,
            SourceSpan span,
            std::string label = {}) {
    if (IsAllowed(code)) {
      return;
    }
    diagnostics_.Warning(code, std::move(message))
        .Primary(span, std::move(label));
  }

  // The same, with a change the editor can apply.
  void WarnWith(std::string_view code,
                std::string message,
                SourceSpan span,
                std::string label,
                Fix fix) {
    if (IsAllowed(code)) {
      return;
    }
    diagnostics_.Warning(code, std::move(message))
        .Primary(span, std::move(label))
        .AddFix(std::move(fix));
  }

  // Reads `@allow(CODE, reason = "...")` off a declaration and returns how
  // many codes it pushed, so CheckItem can pop exactly those again.
  size_t PushAllowed(const Item& item);

  // Pops what PushAllowed pushed, on every path out of CheckItem.
  class ScopedAllow {
   public:
    ScopedAllow(Checker* checker, size_t count)
        : checker_(checker), count_(count) {}
    ~ScopedAllow() { checker_->PopAllowed(count_); }

   private:
    Checker* checker_;
    size_t count_;
  };

  void PopAllowed(size_t count) {
    allowed_.resize(allowed_.size() - count);
  }

  // Passed to Dimensions::Describe so a product prints with its declared
  // names: dimension ids are global, so the type store knows them all.
  std::function<Symbol(DimensionId)> DimensionName() const {
    return [this](DimensionId d) { return types_->DimensionName(d); };
  }

  std::string Describe(TypeId id) const {
    return types_->Describe(*interner_, id);
  }

  // Reports a mismatch unless either side is already an error type, which is
  // what stops one real problem from producing a cascade.
  void ReportMismatch(TypeId expected, TypeId found, SourceSpan span) {
    if (types_->IsError(expected) || types_->IsError(found)) {
      return;
    }
    std::string message = "expected `";
    message.append(Describe(expected));
    message.append("`, found `");
    message.append(Describe(found));
    message.append("`");
    Report(kMismatchedTypes, std::move(message), span);
  }

  // -------------------------------------------------------------- scopes --

  void PushScope() { scopes_.emplace_back(); }

  // Hides the locals of the function around a closure, and says how many
  // scopes were hidden.
  //
  // A closure takes nothing with it (D106): what it can see is its own
  // parameters, the file's names and `self` -- so a body that names a
  // local of the enclosing function gets "no value named x is in scope",
  // which is the truth. Hidden rather than removed, because the scopes
  // come back when the closure's body has been checked.
  size_t HideLocals() {
    const size_t hidden = hidden_scopes_;
    hidden_scopes_ = scopes_.size() - 1;
    return hidden;
  }
  void ShowLocals(size_t hidden) { hidden_scopes_ = hidden; }

  // Refuses a closure that took something with it where it would
  // outlive what it took.
  //
  // Its environment is a copy of the enclosing function's locals, made
  // where the closure was: handing it upwards -- returning it, storing
  // it in a field or a global -- leaves it pointing at a frame that is
  // gone. A closure that captures is for passing *down*, which covers a
  // comparator, a callback and a loop body; state that outlives a call
  // is what a class with a method is for (D138).
  void RefuseEscape(ExprId id, std::string_view what) {
    if (!ast_.Contains(id)) {
      return;
    }
    const Expr& expr = ast_.Get(id);
    if (expr.kind != ExprKind::kClosure ||
        result_.CapturesOf(id).empty()) {
      return;
    }
    std::string message = "a closure that captures cannot be ";
    message.append(what);
    Report(kClosureEscapes, std::move(message), expr.span,
           "what it captured lives in this call's frame; give a type the "
           "state and a method, or pass the closure down instead");
  }

  // The local a closure cannot see by name, or null. What a capture
  // copies -- and reading it counts as using it, which is why this is
  // not a const query (D138).
  const Binding* HiddenLocal(Symbol name) {
    for (size_t depth = hidden_scopes_; depth > 0; --depth) {
      for (LocalBinding& local : scopes_[depth - 1].bindings) {
        if (local.binding.name == name) {
          local.used = true;
          return &local.binding;
        }
      }
    }
    return nullptr;
  }

  bool IsHiddenLocal(Symbol name) {
    return HiddenLocal(name) != nullptr;
  }

  // Whether |name| is a local of this call rather than a module-level
  // one: a module-level `var` outlives the call, so a closure stored in
  // one escapes (D138).
  bool IsLocalName(Symbol name) const {
    for (const Scope& scope : scopes_) {
      for (const LocalBinding& local : scope.bindings) {
        if (local.binding.name == name) {
          return true;
        }
      }
    }
    return false;
  }

  // Says that |closure| reads |name| from the function around it. In
  // order and without duplicates: the order is the environment's layout.
  void Capture(ExprId closure, Symbol name) {
    for (auto& [id, names] : result_.captures) {
      if (id != closure) {
        continue;
      }
      for (Symbol held : names) {
        if (held == name) {
          return;
        }
      }
      names.push_back(name);
      return;
    }
    result_.captures.emplace_back(closure, std::vector<Symbol>{name});
  }
  void PopScope() {
    if (scopes_.empty()) {
      return;
    }
    // Reported here because this is also where the last chance to use the
    // binding was.
    for (const LocalBinding& local : scopes_.back().bindings) {
      if (local.used || local.deliberate || !local.span.is_valid()) {
        continue;
      }
      const std::string_view spelled =
          interner_->Text(local.binding.name);
      std::string message = "`";
      message.append(spelled);
      message.append("` is never used");
      // And the change that silences it, as an edit the editor can
      // apply: the first fix this compiler offers, and the one every
      // person meets first (D191).
      Fix fix;
      fix.message = "prefix with `_`";
      fix.edits.push_back(FixEdit{
          SourceSpan{local.span.file, local.span.begin, local.span.begin},
          "_"});
      WarnWith(kUnusedName, std::move(message), local.span,
               "prefix it with `_` if that is deliberate",
               std::move(fix));
    }
    scopes_.pop_back();
  }
  void Bind(Symbol name, TypeId type, bool is_mutable,
            SourceSpan span = SourceSpan());
  // Not const: finding a local marks it used, which is what W0101 reads.
  const Binding* Lookup(Symbol name);

  // --------------------------------------------------------- declarations --

  void CheckItem(ItemId id);
  void CheckFunction(const Item& item, uint32_t owner);

  // Interns a declaration's type parameters against |owner| -- the index the
  // signature pass used -- and puts them in scope. Returns how many were
  // pushed.
  size_t PushGenerics(const Item& item, uint32_t owner);
  void CheckBinding(const Item& item);
  // Whether the declaration being checked is one another file could name.
  bool at_module_level_ = false;
  // How many of the innermost scopes are hidden, which is how a closure
  // sees its own parameters and nothing of the function around it.
  size_t hidden_scopes_ = 0;
  // The closures being checked, innermost last: a name read inside one
  // is captured by it (D138).
  std::vector<ExprId> closures_;
  // Thread safety (D232). What each closure's own body touches, so that
  // handing one to a `@Thread` parameter can be judged without the
  // function around it; and the closures so handed, judged at the end of
  // the body when their captures are all known.
  struct ClosureFacts {
    ExprId closure;
    bool touches = false;
    std::vector<std::pair<uint32_t, uint32_t>> callees;
  };
  struct PendingThread {
    ExprId closure;
    uint8_t mode = 1;
    // False when the caller states `uses unsafe`: the globals are its
    // responsibility, but a missing environment is not a matter of trust
    // (D232).
    bool judge_globals = true;
  };
  std::vector<ClosureFacts> closure_facts_;
  std::vector<PendingThread> pending_threads_;
  // `uses unsafe` on the declaration being checked: the caller takes
  // responsibility for what the threads share.
  bool caller_unsafe_ = false;

  bool InsideClosure(ExprId closure) const {
    return std::find(closures_.begin(), closures_.end(), closure) !=
           closures_.end();
  }

  void NoteGlobalTouch() {
    result_.touches_global = true;
    for (ClosureFacts& facts : closure_facts_) {
      if (InsideClosure(facts.closure)) {
        facts.touches = true;
      }
    }
  }

  void NoteCall(uint32_t file, uint32_t item) {
    result_.all_callees.emplace_back(file, item);
    for (ClosureFacts& facts : closure_facts_) {
      if (InsideClosure(facts.closure)) {
        facts.callees.emplace_back(file, item);
      }
    }
  }

  // Looks at the arguments of a call to a `@Thread` function.
  void CollectThreadArguments(const Expr& call) {
    if (!ast_.Contains(call.lhs) ||
        ast_.Get(call.lhs).kind != ExprKind::kPath) {
      return;
    }
    const Binding* callee = Lookup(ast_.Get(call.lhs).name);
    if (callee == nullptr || callee->thread_args == 0) {
      return;
    }
    for (ExprId argument : ast_.Get(call.args)) {
      if (!ast_.Contains(argument)) {
        continue;
      }
      const Expr& value = ast_.Get(argument);
      if (value.kind == ExprKind::kClosure) {
        pending_threads_.push_back(
            {argument, callee->thread_args, !caller_unsafe_});
      } else if (value.kind == ExprKind::kPath && !caller_unsafe_) {
        const Binding* named = Lookup(value.name);
        if (named != nullptr && named->kind == BindingKind::kFunction &&
            named->item != Binding::kNoItem) {
          TypedBody::ThreadRoot root;
          root.span = value.span;
          root.callees.emplace_back(named->file, named->item);
          result_.thread_roots.push_back(std::move(root));
        }
      }
    }
  }

  void SettleThreadClosures() {
    for (const PendingThread& pending : pending_threads_) {
      const SourceSpan span = ast_.Get(pending.closure).span;
      if (pending.mode == 1 &&
          !result_.CapturesOf(pending.closure).empty()) {
        Report(kThreadCaptures,
               "a closure that captures cannot be started on another thread",
               span,
               "a thread gets no environment here; use a function and a "
               "module-level atomic or lock, or state `uses unsafe`");
      }
      if (!pending.judge_globals) {
        continue;
      }
      TypedBody::ThreadRoot root;
      root.span = span;
      for (const ClosureFacts& facts : closure_facts_) {
        if (facts.closure == pending.closure) {
          root.touches = facts.touches;
          root.callees = facts.callees;
          break;
        }
      }
      result_.thread_roots.push_back(std::move(root));
    }
    pending_threads_.clear();
  }

  // ---------------------------------------------------------- statements --

  void CheckBlock(ExprId id, TypeId expected);

  // True when control cannot continue past |id|: a `return`, a `break`, a
  // `continue`, or an expression statement whose type is `never`.
  bool Diverges(StmtId id) const;
  void CheckStmt(StmtId id);
  // |is_mutable| comes from the `var` keyword of the enclosing
  // statement; a pattern of its own only says `mut`.
  void BindPattern(PatternId id, TypeId type,
                   bool is_mutable = false);

  // --------------------------------------------------------- expressions --

  // |expected| is the type the context wants, or an invalid id when it has no
  // opinion. The returned type is what the expression actually has.
  //
  // |keep_open| leaves a literal un-defaulted. A binary operator uses it for
  // both operands, because in `1 + small` neither side should settle before
  // the other has been seen.
  TypeId Check(ExprId id, TypeId expected, bool keep_open = false);
  TypeId CheckLiteral(const Expr& expr, TypeId expected);
  TypeId CheckPath(const Expr& expr);
  TypeId CheckUnary(const Expr& expr, TypeId expected);
  TypeId CheckBinary(const Expr& expr, TypeId expected);
  bool IsLiteralTree(ExprId id) const;
  TypeId CheckAssign(const Expr& expr);
  void CheckMachine(const Item& item);
  ExprId RootOfPlace(ExprId id) const;
  const Member* SignalOperand(ExprId id) const;
  bool IsBytesAndText(TypeId from, TypeId to) const;
  bool IsPointerArithmetic(TokenKind op, TypeId left, TypeId right);
  TypeId CheckCall(ExprId id, const Expr& expr);
  TypeId CheckIndex(const Expr& expr);
  TypeId CheckField(const Expr& expr);
  // What a type parameter or a projection is known to be: its bounds, and
  // for `C.Walker` the bound `type Walker: Iterator` declared (D212).
  std::vector<TypeId> BoundsOf(TypeId type);
  const Member* MemberThroughBounds(TypeId receiver, Symbol name, TypeId* ability);
  // An ability's signature as it reads on |receiver|: its own `Item`
  // becomes `receiver.Item`.
  TypeId AsSelf(TypeId type, TypeId ability, TypeId receiver);
  // Every projection of a concrete type replaced by what its
  // implementation said: `ListWalk<int>.Item` is `int`.
  TypeId Normalize(TypeId type);
  TypeId Instantiate(ExprId id, const Expr& expr, TypeId declared);
  bool IsWideLiteral(ExprId id) const;
  void CheckConstructor(const Expr& expr, TypeId type);

  // Notes what calling |callee| does: its declared row if it has one, and
  // otherwise who to ask once the module-wide rows are known.
  void RecordCallEffects(ExprId callee, ExprId call);
  // The effects of reaching `binding`, whether by calling it or by naming it
  // as a value (D221, D222).
  void ChargeBinding(const Binding* binding, bool drop_async);

  // The member of the enclosing type named |name|, or null when there is no
  // enclosing type or it has no such member.
  const Member* SelfMember(Symbol name) const;
  TypeId IteratedElement(TypeId sequence);
  TypeId IteratedWalker(TypeId iterator);
  TypeId CheckIf(const Expr& expr, TypeId expected);
  void NarrowOnNullCheck(ExprId condition);
  TypeId CheckMatch(const Expr& expr, TypeId expected);
  void CheckExhaustive(const Expr& expr, TypeId subject);
  void CheckAbilityConformance(const Item& item, TypeId target);

  // Reports a value type that contains itself, which would have no size.
  void CheckSized(const Item& item, TypeId type);
  void CheckBaseChain(const Item& item, TypeId type);
  // True when |type| reaches |target| through fields stored inline.
  bool ReachesByValue(TypeId type, TypeId target, int depth) const;
  void CheckOverride(const Item& item);
  bool HasForeignRepresentation(TypeId type) const;
  void CheckForeignType(TypeId type, SourceSpan span, bool is_result);
  void RecordInstantiation(ExprId id,
                           const TypeStore::Substitution& substitution);
  void CheckBounds(const TypeStore::Substitution& substitution,
                   SourceSpan span);
  TypeId CheckLoop(const Expr& expr);
  TypeId CheckRange(const Expr& expr);

  // Adjusts a literal to the type its context wants, and reports when the
  // value does not fit.
  TypeId Adapt(TypeId found, TypeId expected, SourceSpan span);
  // True when a value of |found| may be used where |expected| is wanted.
  bool Accepts(TypeId expected, TypeId found) const;
  // Whether |type| is one of the two string types, which `+` joins.
  bool IsTextLike(TypeId type) const {
    return types_->KindOf(type) == TypeKind::kString ||
           types_->KindOf(type) == TypeKind::kStr;
  }
  // The type of a numeric literal with a unit suffix.
  TypeId QuantityOfUnit(const NumberLiteral& number,
                        TypeId numeric,
                        SourceSpan span);

  void Record(ExprId id, TypeId type) {
    if (id.is_valid() && id.index < result_.expr_types.size()) {
      result_.expr_types[id.index] = type;
    }
  }

  TypeStore* types_;
  const StringInterner* interner_;
  const FileResolution* resolution_;
  const Ast& ast_;
  DiagnosticEngine diagnostics_;
  TypeResolver resolver_;
  // ItemId to item tree index, so a type parameter is interned against the
  // same owner the signature pass used.
  const ItemIndexMap* item_indices_ = nullptr;
  TypedBody result_;
  // Whether the declaration being checked is a `task`, and which expression
  // is currently the operand of a `wait` or a `spawn`. Both are about where
  // suspension is allowed to happen.
  bool in_task_ = false;
  bool in_deinit_ = false;
  ExprId started_;
  // The path being called, which RecordCallEffects has already charged.
  const Expr* callee_path_ = nullptr;
  bool spawned_ = false;
  // Whether a `wait` may appear at the expression about to be checked, and
  // whether the block being entered stands somewhere one may.
  bool wait_allowed_ = false;
  bool block_allows_wait_ = false;
  std::vector<Scope> scopes_;
  // The declared return type of the function being checked.
  TypeId return_type_;
  // The type whose members are in scope, inside a method.
  TypeId self_type_;
  // Codes suppressed by an enclosing `@allow`, innermost last.
  std::vector<std::string> allowed_;
  // Nesting depth of loops, so `break` outside one can be reported.
  uint32_t loop_depth_ = 0;
};

void Checker::Bind(Symbol name, TypeId type, bool is_mutable,
                   SourceSpan span) {
  if (scopes_.empty() || !name.is_valid()) {
    return;
  }
  LocalBinding local;
  local.binding.name = name;
  local.binding.kind = BindingKind::kLocal;
  local.binding.type = type;
  local.binding.is_mutable = is_mutable;
  local.span = span;
  const std::string_view text = interner_->Text(name);
  local.deliberate = !text.empty() && text.front() == '_';
  scopes_.back().bindings.push_back(std::move(local));
}

const Binding* Checker::Lookup(Symbol name) {
  // Innermost scope first, so a local shadows a field and a field shadows a
  // module-level name. Inside a closure the scopes below it are hidden: it
  // takes nothing with it, so a local of the function around it is not
  // something it can see (D106).
  for (size_t depth = scopes_.size(); depth > hidden_scopes_; --depth) {
    Scope& scope = scopes_[depth - 1];
    for (auto it = scope.bindings.rbegin(); it != scope.bindings.rend();
         ++it) {
      if (it->binding.name == name) {
        it->used = true;
        return &it->binding;
      }
    }
  }
  if (resolution_ == nullptr) {
    return nullptr;
  }
  // Falling through to file scope means the name came from this file's own
  // declarations, its module siblings or its imports -- which is exactly
  // what an unused-import check needs to know.
  result_.used_names.push_back(name);
  return resolution_->Find(name);
}

// ------------------------------------------------------------- adaptation ---

bool Checker::Accepts(TypeId expected, TypeId found) const {
  if (!expected.is_valid() || !found.is_valid()) {
    return true;
  }
  // An expectation that is itself an open literal says nothing: it is what a
  // sibling operand happened to be before either side settled.
  if (types_->IsLiteral(expected)) {
    return types_->IsNumeric(found);
  }
  if (expected == found) {
    return true;
  }
  // An error anywhere is already reported; accepting it keeps the cascade
  // from starting.
  if (types_->IsError(expected) || types_->IsError(found)) {
    return true;
  }
  // `never` is the type of an expression that does not finish, so it fits
  // wherever a value is wanted.
  if (types_->KindOf(found) == TypeKind::kNever) {
    return true;
  }
  // Anything fits an optional of itself, which is what makes `let x: Pawn?
  // = pawn` work without a wrapper call. Asked before the literal rules
  // rather than after: `const n: int? = 5` is a literal against an
  // optional, and a literal rule that only looks at numbers answers no.
  if (types_->IsOptional(expected)) {
    return Accepts(types_->ElementOf(expected), found);
  }

  // A `T!` takes a `T` or one of the errors it may carry. The set is
  // inferred from the body (D90), so any error set fits here and the
  // inference is what works out which ones a signature really carries.
  if (types_->KindOf(expected) == TypeKind::kFallible) {
    return types_->KindOf(found) == TypeKind::kErrorSet ||
           Accepts(types_->ElementOf(expected), found);
  }
  // A literal adapts to any numeric type of the right family.
  if (types_->KindOf(found) == TypeKind::kIntLiteral) {
    return types_->IsNumeric(expected);
  }
  if (types_->KindOf(found) == TypeKind::kFloatLiteral) {
    return types_->IsFloatingPoint(expected) ||
           types_->KindOf(expected) == TypeKind::kQuantity;
  }
  // A pointer that may be written fits where one that may not is wanted.
  // The other direction does not: that would hand out permission nobody
  // gave.
  if (types_->KindOf(expected) == TypeKind::kPointer &&
      types_->KindOf(found) == TypeKind::kPointer &&
      types_->ElementOf(expected) == types_->ElementOf(found)) {
    return types_->Get(found).is_mutable &&
           !types_->Get(expected).is_mutable;
  }

  // A reference fits where a raw pointer is wanted. Both are an address;
  // the difference is what the language promises about it, and a raw
  // pointer promises nothing -- so `&buf[0]` is how a buffer is handed to
  // something outside the language, which is `unsafe` either way.
  if (types_->KindOf(expected) == TypeKind::kPointer &&
      types_->KindOf(found) == TypeKind::kReference) {
    const TypeId target = types_->ElementOf(expected);
    const TypeId source = types_->ElementOf(found);
    return target == source ||
           types_->KindOf(target) == TypeKind::kVoid;
  }

  // An owned `String` fits wherever a borrowed `str` is wanted. This is an
  // implicit borrow, and it is safe for the reason references are second
  // class (D8): a `str` cannot be stored in a field, so the view can never
  // outlive the string it points into.
  if (types_->KindOf(expected) == TypeKind::kStr &&
      types_->KindOf(found) == TypeKind::kString) {
    return true;
  }
  // A derived class fits where its base is wanted, and a type fits where a
  // ability it implements is wanted. Both are the same question -- "is this
  // one of those" -- and the resolution answers it from the `:` and the
  // `extend ... as ...` it saw.
  //
  // This is what makes dynamic dispatch reachable at all: without it a
  // `Barrel` could not be passed to something that takes an `Actor`, and
  // `virtual` had nothing to be virtual about.
  if (resolution_ != nullptr && found != expected &&
      resolution_->Satisfies(found, expected)) {
    return true;
  }

  // A slice is a view of an array, so an array fits wherever one is wanted.
  // Without this a slice has nothing to be a view of, and `fun f(xs: [int])`
  // could not be called with `[1, 2, 3]`.
  if (types_->KindOf(expected) == TypeKind::kSlice &&
      types_->KindOf(found) == TypeKind::kArray) {
    const Type& slice = types_->Get(expected);
    const Type& array = types_->Get(found);
    // The layouts have to agree: a struct-of-arrays view of an array of
    // structs would be a different memory shape, not a view.
    return slice.is_soa == array.is_soa &&
           types_->ElementOf(expected) == types_->ElementOf(found);
  }
  return false;
}

// An unsuffixed integer literal, perhaps negated, too big for the default
// `int`. It has no type of its own to be checked against, so it is looked
// for where the default would be taken, and where a cast names the type it
// is meant to have (D225).
bool Checker::IsWideLiteral(ExprId id) const {
  bool negated = false;
  while (ast_.Contains(id) && ast_.Get(id).kind == ExprKind::kUnary &&
         ast_.Get(id).op == TokenKind::kMinus) {
    negated = true;
    id = ast_.Get(id).lhs;
  }
  if (!ast_.Contains(id) || ast_.Get(id).kind != ExprKind::kIntLiteral) {
    return false;
  }
  const NumberLiteral& number = ast_.number(ast_.Get(id).aux);
  if (number.has_unit() || number.suffix != NumericSuffix::kNone) {
    return false;
  }
  const IntRange range = RangeOf(*types_, types_->default_int());
  return number.integer_value > range.max_magnitude - (negated ? 0 : 1);
}

TypeId Checker::Adapt(TypeId found, TypeId expected, SourceSpan span) {
  if (expected.is_valid() && types_->IsLiteral(expected)) {
    // The expectation was an open literal, so it imposes nothing; the found
    // type wins and stays open if it is a literal too.
    return found;
  }
  if (!expected.is_valid()) {
    // No expectation: a literal settles on its default type.
    if (types_->KindOf(found) == TypeKind::kIntLiteral) {
      return types_->default_int();
    }
    if (types_->KindOf(found) == TypeKind::kFloatLiteral) {
      return types_->default_float();
    }
    return found;
  }

  if (!Accepts(expected, found)) {
    ReportMismatch(expected, found, span);
    return expected;
  }

  if (types_->IsLiteral(found)) {
    // An integer literal in a float context is fine; the reverse is not,
    // which Accepts already decided.
    if (types_->KindOf(expected) == TypeKind::kQuantity) {
      return expected;
    }
    return expected;
  }
  if (types_->IsOptional(expected) && !types_->IsOptional(found)) {
    return expected;
  }
  return found;
}

TypeId Checker::QuantityOfUnit(const NumberLiteral& number,
                               TypeId numeric,
                               SourceSpan span) {
  const Binding* unit = Lookup(number.unit);
  if (unit == nullptr || unit->kind != BindingKind::kUnit) {
    std::string message = "no unit named `";
    message.append(interner_->Text(number.unit));
    message.append("` is in scope");
    Report(kUnknownUnit, std::move(message), span,
           "declare it with `unit s: Time`");
    return types_->error();
  }
  // A unit literal is always a float: `200ms` is 0.2 seconds, and keeping it
  // integral would make that conversion lossy.
  return types_->Quantity(numeric,
                          Dimensions::Base(unit->dimension));
}

// -------------------------------------------------------------- literals ----

TypeId Checker::CheckLiteral(const Expr& expr, TypeId expected) {
  switch (expr.kind) {
    case ExprKind::kIntLiteral:
    case ExprKind::kFloatLiteral: {
      const NumberLiteral& number = ast_.number(expr.aux);
      const bool is_float = expr.kind == ExprKind::kFloatLiteral;

      if (number.has_unit()) {
        const TypeId numeric = types_->default_float();
        return QuantityOfUnit(number, numeric, expr.span);
      }

      // An explicit suffix fixes the type; otherwise the literal stays open
      // and takes whatever the context wants.
      switch (number.suffix) {
        case NumericSuffix::kI8:
          return types_->Int(IntWidth::k8, true);
        case NumericSuffix::kI16:
          return types_->Int(IntWidth::k16, true);
        case NumericSuffix::kI32:
          return types_->Int(IntWidth::k32, true);
        case NumericSuffix::kI64:
          return types_->Int(IntWidth::k64, true);
        case NumericSuffix::kU8:
          return types_->Int(IntWidth::k8, false);
        case NumericSuffix::kU16:
          return types_->Int(IntWidth::k16, false);
        case NumericSuffix::kU32:
          return types_->Int(IntWidth::k32, false);
        case NumericSuffix::kU64:
          return types_->Int(IntWidth::k64, false);
        case NumericSuffix::kF32:
          return types_->Float(FloatWidth::k32);
        case NumericSuffix::kF64:
          return types_->Float(FloatWidth::k64);
        case NumericSuffix::kNone:
          break;
      }

      if (is_float) {
        return types_->float_literal();
      }

      // An integer literal has arbitrary precision until it is used, so the
      // only thing to check here is that it fits where it is going.
      const TypeId target =
          expected.is_valid() && types_->KindOf(expected) == TypeKind::kInt
              ? expected
              : TypeId{};
      if (target.is_valid()) {
        const IntRange range = RangeOf(*types_, target);
        const uint64_t value = number.integer_value;
        const bool fits = range.is_signed ? value <= range.max_magnitude
                                          : value <= range.max_magnitude;
        if (!fits) {
          std::string message = "the literal does not fit in `";
          message.append(Describe(target));
          message.append("`");
          Report(kLiteralOutOfRange, std::move(message), expr.span);
          return target;
        }
      }
      return types_->int_literal();
    }

    case ExprKind::kStringLiteral: {
      // A literal with no interpolation is static data, so it is a borrowed
      // `str`. One with interpolation builds a new string at runtime, which
      // is what makes it an owned `String`.
      bool interpolated = false;
      for (ExprId part : ast_.Get(expr.args)) {
        const Expr& piece = ast_.Get(part);
        if (piece.kind != ExprKind::kStringText) {
          // An interpolated expression can be anything that formats; its own
          // type is checked without an expectation.
          Check(part, TypeId{});
          interpolated = true;
        } else {
          Record(part, types_->str());
        }
      }
      return interpolated ? types_->string_type() : types_->str();
    }

    case ExprKind::kCharLiteral:
      return types_->char_type();
    case ExprKind::kBoolLiteral:
      return types_->bool_type();
    case ExprKind::kNullLiteral:
      // `null` alone has no type; it takes the optional the context wants.
      if (expected.is_valid() && types_->IsOptional(expected)) {
        return expected;
      }
      return types_->Optional(types_->never());

    case ExprKind::kPointerNull:
      // `ptrnull` is the address no object has: what a C function takes
      // to mean "nothing here" and gives back to mean "this failed". It
      // takes the pointer type the context wants, and `*mut u8` when
      // there is none -- the one every other pointer converts from
      // without a promise being broken (D156).
      if (expected.is_valid() &&
          types_->KindOf(expected) == TypeKind::kPointer) {
        return expected;
      }
      return types_->Pointer(types_->Int(IntWidth::k8, false), true);

    default:
      return types_->error();
  }
}

// ------------------------------------------------------------ expressions ---

// What `for x in value` binds when the value's own type says how to walk
// it: `Iterate` hands back something with `Next` and `Value`, and this is
// what `Value` gives.
TypeId Checker::IteratedElement(TypeId sequence) {
  const TypeId named = types_->StripWrappers(sequence);
  // `for x in items` where `items: C` and `C: Iterable`: the walker is
  // `C.Walker` and the element `C.Walker.Item` (D212).
  if (types_->KindOf(named) == TypeKind::kParameter || types_->KindOf(named) == TypeKind::kProjection) {
    TypeId ability;
    const Member* iterate = MemberThroughBounds(named, interner_->Lookup("Iterate"), &ability);
    if (iterate == nullptr || types_->KindOf(iterate->type) != TypeKind::kFunction) {
      return TypeId{};
    }
    const TypeId walker = Normalize(AsSelf(types_->ElementOf(iterate->type), ability, named));
    if (types_->KindOf(walker) != TypeKind::kParameter && types_->KindOf(walker) != TypeKind::kProjection) {
      return IteratedWalker(walker);
    }
    TypeId walks;
    const Member* value = MemberThroughBounds(walker, interner_->Lookup("Value"), &walks);
    if (value == nullptr || types_->KindOf(value->type) != TypeKind::kFunction) {
      return TypeId{};
    }
    return Normalize(AsSelf(types_->ElementOf(value->type), walks, walker));
  }
  if (types_->KindOf(named) != TypeKind::kNamed &&
      types_->KindOf(named) != TypeKind::kInstance) {
    return TypeId{};
  }
  const auto member_of = [&](TypeId owner, std::string_view name) {
    // Looked up rather than interned: a name nothing in the program ever
    // wrote cannot be a member of anything, and this is a const query.
    const Symbol symbol = interner_->Lookup(name);
    if (const Member* own = resolution_->FindMember(owner, symbol)) {
      return own;
    }
    return resolution_->FindInheritedMember(owner, symbol);
  };

  const Member* iterate = member_of(named, "Iterate");
  if (iterate == nullptr || iterate->kind != MemberKind::kMethod ||
      types_->KindOf(iterate->type) != TypeKind::kFunction) {
    return TypeId{};
  }
  return IteratedWalker(types_->ElementOf(iterate->type));
}

TypeId Checker::IteratedWalker(TypeId iterator) {
  const auto member_of = [&](TypeId owner, std::string_view name) {
    const Symbol symbol = interner_->Lookup(name);
    if (const Member* own = resolution_->FindMember(owner, symbol)) {
      return own;
    }
    return resolution_->FindInheritedMember(owner, symbol);
  };
  const Member* next = member_of(iterator, "Next");
  const Member* value = member_of(iterator, "Value");
  if (next == nullptr || value == nullptr ||
      types_->KindOf(value->type) != TypeKind::kFunction) {
    return TypeId{};
  }
  return Normalize(types_->ElementOf(value->type));
}

const Member* Checker::SelfMember(Symbol name) const {
  if (!self_type_.is_valid()) {
    return nullptr;
  }
  const TypeId self = types_->StripWrappers(self_type_);
  if (const Member* own = resolution_->FindMember(self, name)) {
    return own;
  }
  // A field of the class this one derives from is a field of this one: a
  // method of `Cube` reads `side` because `Square` declared it, which is
  // what inheritance means and what it is for.
  return resolution_->FindInheritedMember(self, name);
}

TypeId Checker::CheckPath(const Expr& expr) {
  const Binding* binding = Lookup(expr.name);
  if (binding == nullptr) {
    // Inside a method a bare name may be a member of the enclosing type:
    // `health` means `self.health`, which is how gameplay code reads.
    if (const Member* member = SelfMember(expr.name); member != nullptr) {
      return member->type;
    }
    // A local of the function around a closure: the closure takes a
    // *copy* of it, which is what capture means here. Recorded so that
    // lowering can build an environment, and recorded against the
    // innermost closure being checked -- a closure inside a closure
    // captures through both (D138).
    if (const Binding* outer = HiddenLocal(expr.name); outer != nullptr) {
      if (closures_.empty()) {
        std::string message = "`";
        message.append(interner_->Text(expr.name));
        message.append("` belongs to the function around this closure");
        Report(kClosureCaptures, std::move(message), expr.span,
               "a closure takes nothing with it: pass it as a parameter, or "
               "give a type the state and a method");
        return types_->error();
      }
      Capture(closures_.back(), expr.name);
      return outer->type;
    }
    std::string message = "no value named `";
    message.append(interner_->Text(expr.name));
    message.append("` is in scope");
    Report(kUnknownName, std::move(message), expr.span);
    return types_->error();
  }
  if (binding->kind == BindingKind::kValue && binding->is_mutable &&
      binding->item != Binding::kNoItem) {
    NoteGlobalTouch();
  }
  if (binding->kind == BindingKind::kErrorTag) {
    // An error tag is a value of the error set it belongs to.
    return types_->ErrorSet({binding->name});
  }
  // Naming a function as a value reaches it as surely as calling it does,
  // so it costs the same effects; the callee of a call is charged by
  // RecordCallEffects instead (D222).
  if (binding->kind == BindingKind::kFunction &&
      binding->item != Binding::kNoItem && &expr != callee_path_) {
    ChargeBinding(binding, false);
  }
  return binding->type.is_valid() ? binding->type : types_->error();
}

TypeId Checker::CheckUnary(const Expr& expr, TypeId expected) {
  // The expectation reaches the operand only where the operator preserves
  // the type. `*p` and `&x` change it, so passing the context's type down
  // would check the pointer against what the pointee has to be -- which
  // reports a mismatch on the operand and then a second one on the
  // dereference of the error type it became.
  const bool preserves_type = expr.op == TokenKind::kMinus ||
                              expr.op == TokenKind::kTilde ||
                              expr.op == TokenKind::kBang;
  // An optional expectation says nothing about the operand: `-total`
  // where an `int?` is wanted is still arithmetic on an `int`, and
  // passing the optional down made the operand an optional and then
  // reported `-` as needing a number (D114).
  TypeId wanted = preserves_type ? expected : TypeId{};
  if (wanted.is_valid() && types_->IsOptional(wanted)) {
    wanted = types_->ElementOf(wanted);
  }
  // `-9223372036854775807 - 1` is a literal expression until the context
  // says what it is; settling the operand on `int` first made it one (D225).
  const TypeId operand =
      Check(expr.lhs, wanted,
            /*keep_open=*/expr.op == TokenKind::kMinus ||
                expr.op == TokenKind::kTilde);
  switch (expr.op) {
    case TokenKind::kMinus:
      if (!types_->IsNumeric(operand) && !types_->IsError(operand)) {
        std::string message = "`-` needs a number, found `";
        message.append(Describe(operand));
        message.append("`");
        Report(kBadOperand, std::move(message), expr.span);
        return types_->error();
      }
      return operand;
    case TokenKind::kBang:
      if (!Accepts(types_->bool_type(), operand)) {
        ReportMismatch(types_->bool_type(), operand, expr.span);
      }
      return types_->bool_type();
    case TokenKind::kTilde:
      if (!types_->IsInteger(operand) && !types_->IsError(operand)) {
        std::string message = "`~` needs an integer, found `";
        message.append(Describe(operand));
        message.append("`");
        Report(kBadOperand, std::move(message), expr.span);
        return types_->error();
      }
      return operand;
    case TokenKind::kAmp:
      return types_->Reference(operand,
                               HasFlag(expr.flags, ExprFlags::kMutable));
    case TokenKind::kStar: {
      // Reading through a raw pointer is `unsafe`: nothing in the language
      // promises there is anything there. A reference carries that promise,
      // which is the whole difference between the two.
      if (types_->KindOf(operand) == TypeKind::kPointer) {
        result_.direct_effects.Add(
            types_->BuiltinEffectId(BuiltinEffect::kUnsafe));
      }
      const TypeId target = types_->ElementOf(operand);
      if (!target.is_valid()) {
        std::string message = "cannot dereference `";
        message.append(Describe(operand));
        message.append("`");
        Report(kBadOperand, std::move(message), expr.span);
        return types_->error();
      }
      return target;
    }
    default:
      return operand;
  }
}

// Whether |id| is arithmetic on integer literals alone, which has no type
// until the context gives it one.
bool Checker::IsLiteralTree(ExprId id) const {
  if (!ast_.Contains(id)) {
    return false;
  }
  const Expr& expr = ast_.Get(id);
  switch (expr.kind) {
    case ExprKind::kIntLiteral:
      return true;
    case ExprKind::kUnary:
      return (expr.op == TokenKind::kMinus || expr.op == TokenKind::kTilde) &&
             IsLiteralTree(expr.lhs);
    case ExprKind::kBinary:
      return IsLiteralTree(expr.lhs) && IsLiteralTree(expr.rhs);
    default:
      return false;
  }
}

TypeId Checker::CheckBinary(const Expr& expr, TypeId expected) {
  // `-9223372036854775807 - 1` where an `s64` is wanted: the literals are
  // read at that type from the start. Left open they were folded as `int`
  // and cut to 32 bits (D225).
  const bool takes_context =
      expected.is_valid() && types_->KindOf(expected) == TypeKind::kInt &&
      IsLiteralTree(expr.lhs) && IsLiteralTree(expr.rhs);
  const TypeId left = Check(expr.lhs, takes_context ? expected : TypeId{},
                            /*keep_open=*/true);

  // The left side's type is normally the expectation for the right, which is
  // what makes `health - 1` give the literal the field's type. Three
  // exceptions:
  //   * quantities, because `distance / duration` is supposed to mix
  //     dimensions and `10m + 2s` must reach the dimension check rather than
  //     being reported as a plain mismatch;
  //   * `??`, whose right side matches what is *inside* the optional;
  //   * the boolean operators, which want `bool` on both sides.
  TypeId right_expectation = left;
  if (types_->KindOf(left) == TypeKind::kQuantity ||
      types_->KindOf(left) == TypeKind::kPointer) {
    // A pointer says nothing about what may be added to it: `p + n` takes
    // a count, not another address.
    right_expectation = TypeId{};
  } else if (expr.op == TokenKind::kQuestionQuestion) {
    right_expectation = types_->IsOptional(left) ? types_->ElementOf(left)
                                                 : TypeId{};
  } else if (expr.op == TokenKind::kAmpAmp ||
             expr.op == TokenKind::kPipePipe) {
    right_expectation = types_->bool_type();
  } else if (types_->KindOf(left) == TypeKind::kOptional &&
             types_->KindOf(types_->ElementOf(left)) == TypeKind::kNever) {
    // The left side is the bare `null` literal, whose type says nothing
    // about what it is being compared with.
    right_expectation = TypeId{};
  } else if (types_->KindOf(left) == TypeKind::kString) {
    // An owned string on the left says nothing about what the right side
    // has to be: comparing what was built with a literal is the most
    // common thing done with a string, and demanding an owned one on both
    // sides would make it an error.
    right_expectation = TypeId{};
  } else if (expr.op == TokenKind::kPlus && IsTextLike(left)) {
    // `s + x`: what the right side has to be is said by the operator
    // rather than by the left, so that `s + 1` reads as "`+` joins two
    // strings or adds two numbers" instead of as a mismatch against
    // `text` (D110).
    right_expectation = TypeId{};
  }
  const TypeId right =
      Check(expr.rhs, right_expectation, /*keep_open=*/true);

  switch (expr.op) {
    case TokenKind::kAmpAmp:
    case TokenKind::kPipePipe:
      if (!Accepts(types_->bool_type(), left)) {
        ReportMismatch(types_->bool_type(), left, expr.span);
      }
      if (!Accepts(types_->bool_type(), right)) {
        ReportMismatch(types_->bool_type(), right, expr.span);
      }
      return types_->bool_type();

    case TokenKind::kEqEq:
    case TokenKind::kBangEq:
    case TokenKind::kLess:
    case TokenKind::kLessEq:
    case TokenKind::kGreater:
    case TokenKind::kGreaterEq: {
      // `null` compares with anything that may be absent, in either order,
      // and that is how an optional is tested without `if let`.
      const auto is_null = [this](ExprId id) {
        return ast_.Contains(id) &&
               ast_.Get(id).kind == ExprKind::kNullLiteral;
      };
      if ((is_null(expr.rhs) && types_->IsOptional(left)) ||
          (is_null(expr.lhs) && types_->IsOptional(right))) {
        return types_->bool_type();
      }

      const Dimensions left_dimensions = types_->DimensionsOf(left);
      const Dimensions right_dimensions = types_->DimensionsOf(right);
      if (left_dimensions != right_dimensions) {
        // Comparing metres with seconds is the same bug as adding them.
        std::string message = "cannot compare `";
        message.append(
            left_dimensions.Describe(*interner_, DimensionName()));
        message.append("` with `");
        message.append(
            right_dimensions.Describe(*interner_, DimensionName()));
        message.append("`");
        Report(kDimensionMismatch, std::move(message), expr.span);
      } else if (!Accepts(left, right) && !Accepts(right, left)) {
        ReportMismatch(left, right, expr.span);
      }
      return types_->bool_type();
    }

    case TokenKind::kQuestionQuestion: {
      // `a ?? b` removes one layer of optional, and `b` has to fit what is
      // inside.
      if (!types_->IsOptional(left)) {
        if (!types_->IsError(left)) {
          std::string message = "`??` needs an optional on its left, found `";
          message.append(Describe(left));
          message.append("`");
          Report(kBadOperand, std::move(message), expr.span);
        }
        return left;
      }
      const TypeId inner = types_->ElementOf(left);
      if (!Accepts(inner, right)) {
        ReportMismatch(inner, right, expr.span);
      }
      return inner;
    }

    case TokenKind::kStar:
    case TokenKind::kSlash: {
      // Multiplying and dividing quantities combines their dimensions, which
      // is what gives `10m / 2s` a type nobody declared.
      const bool left_quantity =
          types_->KindOf(left) == TypeKind::kQuantity;
      const bool right_quantity =
          types_->KindOf(right) == TypeKind::kQuantity;
      if (left_quantity || right_quantity) {
        const Dimensions left_dimensions = types_->DimensionsOf(left);
        const Dimensions right_dimensions = types_->DimensionsOf(right);
        const Dimensions combined =
            expr.op == TokenKind::kStar
                ? left_dimensions.Multiply(right_dimensions)
                : left_dimensions.Divide(right_dimensions);
        const TypeId carrier = left_quantity ? types_->ElementOf(left)
                                             : types_->ElementOf(right);
        return types_->Quantity(carrier, combined);
      }
      [[fallthrough]];
    }

    case TokenKind::kPlus:
    case TokenKind::kMinus:
    case TokenKind::kPercent: {
      // `p + n` on a raw pointer is the address of the nth element from
      // there, which is how bytes are walked where there is no slice to
      // walk instead.
      if (IsPointerArithmetic(expr.op, left, right)) {
        result_.direct_effects.Add(
            types_->BuiltinEffectId(BuiltinEffect::kUnsafe));
        return left;
      }
      // `a + b` on strings is the two of them joined.
      //
      // Interpolation covers most of it -- `"{a}{b}"` is the same thing --
      // but a program that builds a string from pieces it has in hand
      // reads better with `+`, and every user tries it first (D110).
      if (expr.op == TokenKind::kPlus &&
          (IsTextLike(left) || IsTextLike(right))) {
        if (IsTextLike(left) && IsTextLike(right)) {
          return types_->KindOf(left) == TypeKind::kString ||
                         types_->KindOf(right) == TypeKind::kString
                     ? types_->string_type()
                     : types_->str();
        }
        if (!types_->IsError(left) && !types_->IsError(right)) {
          std::string message =
              "`+` joins two strings or adds two numbers, found `";
          message.append(Describe(left));
          message.append("` and `");
          message.append(Describe(right));
          message.append("`");
          Report(kBadOperand, std::move(message), expr.span);
        }
        return types_->error();
      }
      if (!types_->IsNumeric(left) || !types_->IsNumeric(right)) {
        if (!types_->IsError(left) && !types_->IsError(right)) {
          std::string message = "`";
          message.append(TokenKindSpelling(expr.op));
          message.append("` needs numbers, found `");
          message.append(Describe(left));
          message.append("` and `");
          message.append(Describe(right));
          message.append("`");
          Report(kBadOperand, std::move(message), expr.span);
        }
        return types_->error();
      }
      // Adding quantities requires the same dimension: metres plus seconds
      // is the bug the whole feature exists to catch.
      const Dimensions left_dimensions = types_->DimensionsOf(left);
      const Dimensions right_dimensions = types_->DimensionsOf(right);
      if (left_dimensions != right_dimensions) {
        std::string message = "cannot combine `";
        message.append(
            left_dimensions.Describe(*interner_, DimensionName()));
        message.append("` with `");
        message.append(
            right_dimensions.Describe(*interner_, DimensionName()));
        message.append("`");
        Report(kDimensionMismatch, std::move(message), expr.span,
               "both sides of `+` and `-` need the same dimension");
        return left;
      }
      // A literal on either side takes the other side's type; when both are
      // literals the result stays open for the enclosing context to decide.
      return types_->IsLiteral(left) ? right : left;
    }

    default:
      return types_->IsLiteral(left) ? right : left;
  }
}

// `p + n` and `p - n` on a raw pointer: the address of the nth element
// from there. Scaled by the element, the way C scales it, and `unsafe`
// because nothing says how many elements are actually there.
bool Checker::IsPointerArithmetic(TokenKind op, TypeId left, TypeId right) {
  if (op != TokenKind::kPlus && op != TokenKind::kMinus) {
    return false;
  }
  return types_->KindOf(left) == TypeKind::kPointer &&
         (types_->IsNumeric(right) ||
          types_->KindOf(right) == TypeKind::kIntLiteral);
}

// The expression an assignment's target is rooted in: the name, the `self`
// or the dereference at the bottom of `a.b.c`. What may be written to is a
// property of the root, not of the field at the end.
// Whether this conversion is between a byte view and a string: `[]u8 as
// text`, or the other way.
bool Checker::IsBytesAndText(TypeId from, TypeId to) const {
  const auto is_bytes = [this](TypeId type) {
    if (types_->KindOf(type) != TypeKind::kSlice) {
      return false;
    }
    const TypeId element = types_->ElementOf(type);
    return types_->KindOf(element) == TypeKind::kInt &&
           types_->Get(element).int_width == IntWidth::k8 &&
           !types_->Get(element).is_signed;
  };
  const auto is_text = [this](TypeId type) {
    return types_->KindOf(type) == TypeKind::kStr ||
           types_->KindOf(type) == TypeKind::kString;
  };
  return (is_bytes(from) && is_text(to)) || (is_text(from) && is_bytes(to));
}

// The signal a field access names, when it names one: `p.Died` in
// `p.Died.Emit(...)`.
const Member* Checker::SignalOperand(ExprId id) const {
  if (!ast_.Contains(id)) {
    return nullptr;
  }
  const Expr& expr = ast_.Get(id);
  if (expr.kind == ExprKind::kPath) {
    // A bare name inside a method: `Died.Emit(...)` is `self.Died`, the
    // same way `health` is `self.health`.
    const Member* member = SelfMember(expr.name);
    return member != nullptr && member->kind == MemberKind::kSignal
               ? member
               : nullptr;
  }
  if (expr.kind != ExprKind::kField) {
    return nullptr;
  }
  const TypeId owner = types_->StripWrappers(result_.TypeOf(expr.lhs));
  const Member* member = resolution_->FindMember(owner, expr.name);
  return member != nullptr && member->kind == MemberKind::kSignal
             ? member
             : nullptr;
}

ExprId Checker::RootOfPlace(ExprId id) const {
  ExprId current = id;
  while (ast_.Contains(current)) {
    const Expr& expr = ast_.Get(current);
    if (expr.kind == ExprKind::kField ||
        expr.kind == ExprKind::kIndex ||
        expr.kind == ExprKind::kOptionalField) {
      current = expr.lhs;
      continue;
    }
    return current;
  }
  return id;
}

// A machine's table, checked as a table: every target has to be a state of
// this machine, and every state has to be reachable. The first is where it
// starts, so it needs nothing leading to it.
void Checker::CheckMachine(const Item& item) {
  const std::span<const StateId> states = ast_.Get(item.states);
  std::vector<bool> reached(states.size(), false);
  if (!reached.empty()) {
    reached[0] = true;
  }

  for (StateId state_id : states) {
    if (!ast_.Contains(state_id)) {
      continue;
    }
    const State& state = ast_.Get(state_id);
    for (TransitionId transition_id : ast_.Get(state.transitions)) {
      if (!ast_.Contains(transition_id)) {
        continue;
      }
      const Transition& transition = ast_.Get(transition_id);
      bool found = false;
      for (size_t i = 0; i < states.size(); ++i) {
        if (!ast_.Contains(states[i]) ||
            ast_.Get(states[i]).name != transition.target) {
          continue;
        }
        reached[i] = true;
        found = true;
        break;
      }
      if (!found) {
        std::string message = "`";
        message.append(interner_->Text(item.name));
        message.append("` has no state named `");
        message.append(interner_->Text(transition.target));
        message.append("`");
        Report(kUnknownState, std::move(message), transition.span);
      }
    }
  }

  for (size_t i = 0; i < states.size(); ++i) {
    if (reached[i] || !ast_.Contains(states[i])) {
      continue;
    }
    const State& state = ast_.Get(states[i]);
    std::string message = "state `";
    message.append(interner_->Text(state.name));
    message.append("` cannot be reached");
    Warn(kUnreachableState, std::move(message), state.span,
         "no transition leads to it, and it is not where the machine "
         "starts");
  }

  // The blocks are ordinary code, and `self` in them is the machine.
  const TypeId previous_self = self_type_;
  const Binding* binding = Lookup(item.name);
  self_type_ = binding != nullptr ? binding->type : TypeId{};
  for (StateId state_id : states) {
    if (!ast_.Contains(state_id)) {
      continue;
    }
    const State& state = ast_.Get(state_id);
    for (ExprId block : {state.enter, state.exit}) {
      if (ast_.Contains(block)) {
        Check(block, TypeId{});
      }
    }
  }
  self_type_ = previous_self;
}

TypeId Checker::CheckAssign(const Expr& expr) {
  const TypeId target = Check(expr.lhs, TypeId{});

  // Only a name, a field or an element can be assigned to, and a `let`
  // binding cannot be reassigned at all.
  if (ast_.Contains(expr.lhs)) {
    const ExprId root_id = RootOfPlace(expr.lhs);
    const bool through_field = root_id != expr.lhs;
    if (ast_.Contains(root_id)) {
      const Expr& root = ast_.Get(root_id);
      const bool is_self =
          root.kind == ExprKind::kSelf ||
          (root.kind == ExprKind::kPath && Lookup(root.name) == nullptr &&
           SelfMember(root.name) != nullptr);
      const TypeId root_type =
          is_self ? types_->StripWrappers(self_type_)
                  : types_->StripWrappers(result_.TypeOf(root_id));
      // A handle is not what it points at. Assigning to a field of a
      // class changes the object, which is why `const h` does not freeze
      // it and why a method of one may write to `self` -- the receiver is
      // the address, not a copy.
      // A handle is not what it points at, and neither is a view: writing
      // an element of a slice changes the storage it views, not the
      // binding that holds the view. So `const` on either freezes the
      // handle and not the thing.
      //
      // Which elements may be written is a question this language does
      // not ask yet -- there is no `[]mut T` -- and when it does, this is
      // where the answer goes.
      // A write through a mutable reference is the reference's business,
      // not the binding's: `fun Fill(into: &mut Request)` says the thing
      // referred to may be written, and the name holding the reference is
      // not what is being assigned (D112).
      const bool through_mutable_reference =
          (types_->KindOf(root_type) == TypeKind::kReference ||
           types_->KindOf(root_type) == TypeKind::kPointer) &&
          types_->Get(root_type).is_mutable;
      const bool through_handle =
          types_->IsHandle(root_type) ||
          types_->KindOf(root_type) == TypeKind::kSlice ||
          types_->KindOf(root_type) == TypeKind::kPointer ||
          through_mutable_reference;
      if (is_self && !through_handle &&
          (through_field || root.kind == ExprKind::kPath)) {
        Report(kSelfNotMutable,
               "a method of a value type cannot assign to a field of "
               "`self`",
               expr.span,
               "the receiver is a copy, so the write would be lost; return "
               "a new value, or make the type a `class`");
      } else if (through_field && !through_handle &&
                 root.kind == ExprKind::kPath) {
        // `p.x = v` needs `p` to be a `var`, the same way `p = v` does.
        const Binding* binding = Lookup(root.name);
        if (binding != nullptr && !binding->is_mutable &&
            (binding->kind == BindingKind::kLocal ||
             binding->kind == BindingKind::kValue)) {
          std::string message = "cannot assign to a field of `";
          message.append(interner_->Text(root.name));
          message.append("`, which is not `var`");
          Report(kAssignToLet, std::move(message), expr.span);
        }
      }
    }
    const Expr& left = ast_.Get(expr.lhs);
    if (left.kind == ExprKind::kPath) {
      const Binding* binding = Lookup(left.name);
      const Member* member =
          binding == nullptr ? SelfMember(left.name) : nullptr;
      if (member != nullptr && !member->is_mutable) {
        std::string message = "cannot assign to `";
        message.append(interner_->Text(left.name));
        message.append("`, which is not `var`");
        Report(kAssignToLet, std::move(message), expr.span);
      }
      if (binding != nullptr && !binding->is_mutable &&
          (binding->kind == BindingKind::kLocal ||
           binding->kind == BindingKind::kValue)) {
        std::string message = "cannot assign to `";
        message.append(interner_->Text(left.name));
        message.append("`, which is not `var`");
        Report(kAssignToLet, std::move(message), expr.span);
      }
    }
  }

  const TypeId value = Check(expr.rhs, target);
  if (!Accepts(target, value)) {
    ReportMismatch(target, value, expr.span);
  }
  // A field, a global or an element outlives the call that assigned it;
  // a local does not. Only the first three are an escape (D138).
  if (ast_.Contains(expr.lhs)) {
    const Expr& place = ast_.Get(expr.lhs);
    const bool is_local =
        place.kind == ExprKind::kPath && IsLocalName(place.name);
    if (!is_local) {
      // Storing `self` from its own `deinit` resurrects an object that is
      // being freed; the runtime traps on it too, this is the early word
      // for the plain case (D229).
      if (in_deinit_ && ast_.Contains(expr.rhs) &&
          ast_.Get(expr.rhs).kind == ExprKind::kSelf) {
        Report(kSelfEscapesDeinit,
               "`self` cannot be stored from its own `deinit`",
               expr.span,
               "the object is being freed; copy the fields you need");
      }
      RefuseEscape(expr.rhs, "stored");
    }
  }
  return types_->void_type();
}

TypeId Checker::CheckCall(ExprId id, const Expr& expr) {
  // A type name used as a callee is a constructor: `Vec3()` produces a Vec3.
  // Field types arrive with member resolution, so the arguments are checked
  // without expectations until then.
  if (ast_.Contains(expr.lhs)) {
    const Expr& callee_expr = ast_.Get(expr.lhs);
    if (callee_expr.kind == ExprKind::kPath) {
      const Binding* binding = Lookup(callee_expr.name);
      if (binding != nullptr && binding->kind == BindingKind::kType) {
        const TypeId type = Instantiate(id, expr, binding->type);
        CheckConstructor(expr, type);
        for (const ExprId argument : ast_.Get(expr.args)) {
          RefuseEscape(argument, "stored in a field");
        }
        Record(expr.lhs, type);
        return type;
      }
    }
  }

  RecordCallEffects(expr.lhs, id);
  CollectThreadArguments(expr);

  const Expr* previous_callee = callee_path_;
  callee_path_ = ast_.Contains(expr.lhs) ? &ast_.Get(expr.lhs) : nullptr;
  const TypeId callee = Check(expr.lhs, TypeId{});
  callee_path_ = previous_callee;
  const std::span<const ExprId> args = ast_.Get(expr.args);

  if (types_->KindOf(callee) != TypeKind::kFunction) {
    // The callee's type is unknown until member resolution lands, so an
    // unresolved call checks its arguments and yields an error type rather
    // than inventing a signature.
    for (ExprId arg : args) {
      Check(arg, TypeId{});
    }
    if (!types_->IsError(callee)) {
      std::string message = "`";
      message.append(Describe(callee));
      message.append("` is not callable");
      Report(kNotCallable, std::move(message), expr.span);
    }
    return types_->error();
  }

  std::vector<TypeId> params = types_->ArgumentsOf(callee);
  TypeId result = types_->ElementOf(callee);

  // A generic signature has its parameters inferred from the arguments:
  // `First(xs)` works out `T` from the type of `xs`, so no turbofish and no
  // explicit argument are needed.
  bool generic = types_->MentionsParameter(callee);
  if (generic) {
    TypeStore::Substitution substitution;
    // What the call wrote itself: `SoaNew<Particle>(1000)`. Needed as
    // well as inference, because a signature can mention a parameter only
    // in its *result* -- and then there is nothing to infer from.
    if (HasFlag(expr.flags, ExprFlags::kHasGenericArgs)) {
      std::vector<TypeId> parameters;
      types_->CollectParameters(callee, &parameters);
      size_t position = 0;
      for (TypeRefId written : ast_.Get(expr.generic_args)) {
        if (position >= parameters.size()) {
          break;
        }
        substitution.emplace_back(parameters[position++],
                                  resolver_.Resolve(ast_, written));
      }
    }
    std::vector<TypeId> found;
    found.reserve(args.size());
    for (size_t i = 0; i < args.size(); ++i) {
      // What this parameter is, as far as the arguments before it have
      // said. A parameter still mentioning a type variable gives no
      // expectation -- that is what is being inferred, and a literal
      // settles on its default type without one. One that is already
      // concrete does give it: `Sort(items, |a, b| a < b)` binds `T` from
      // the first argument, and the closure's parameter types come from
      // the second parameter with `T` filled in (D107).
      TypeId wanted;
      if (i < params.size()) {
        const TypeId narrowed = types_->Substitute(params[i], substitution);
        if (!types_->MentionsParameter(narrowed)) {
          wanted = narrowed;
        } else if (types_->KindOf(narrowed) == TypeKind::kFunction) {
          // Half an answer is enough for a closure: `fun Map<T>(f:
          // fun(int) -> T)` says what the closure *takes* even though
          // its result is what is being inferred, and that is exactly
          // what `|n| n * 2` needs to be checkable (D119).
          bool arguments_known = true;
          for (const TypeId argument : types_->ArgumentsOf(narrowed)) {
            arguments_known &= !types_->MentionsParameter(argument);
          }
          if (arguments_known) {
            wanted = narrowed;
          }
        }
      }
      // The hint goes into the check; only a *settled* type goes into
      // the adaptation, because adapting against `fun(int) -> T` would
      // report a mismatch with the very answer that binds `T` (D119).
      const TypeId adapt_to =
          wanted.is_valid() && !types_->MentionsParameter(wanted)
              ? wanted
              : TypeId{};
      const TypeId actual = Adapt(Check(args[i], wanted), adapt_to,
                                  ast_.Get(args[i]).span);
      found.push_back(actual);
      if (i < params.size()) {
        types_->Unify(params[i], actual, &substitution);
      }
    }

    for (size_t i = 0; i < params.size() && i < found.size(); ++i) {
      const TypeId expected = types_->Substitute(params[i], substitution);
      if (!Accepts(expected, found[i])) {
        ReportMismatch(expected, found[i], ast_.Get(args[i]).span);
      }
    }
    CheckBounds(substitution, expr.span);
    // A parameter nothing narrowed stands for itself.
    //
    // A generic body calling a generic -- itself, most often -- passes
    // its own `T` along, and inference has nothing to do: the two types
    // are already the same. Recording that as "no arguments" left the
    // call with no instantiation to find, and lowering turned a recursive
    // `Quicksort` into an `undef` (D107).
    {
      std::vector<TypeId> mentioned;
      types_->CollectParameters(callee, &mentioned);
      for (const TypeId parameter : mentioned) {
        bool named = false;
        for (const auto& [written, value] : substitution) {
          named |= written == parameter;
        }
        if (!named) {
          substitution.emplace_back(parameter, parameter);
        }
      }
    }
    RecordInstantiation(id, substitution);
    if (params.size() != args.size()) {
      std::string message = "expected ";
      message.append(std::to_string(params.size()));
      message.append(params.size() == 1 ? " argument, found "
                                        : " arguments, found ");
      message.append(std::to_string(args.size()));
      Report(kWrongArguments, std::move(message), expr.span);
    }
    return Normalize(types_->Substitute(result, substitution));
  }

  if (params.size() != args.size()) {
    std::string message = "expected ";
    message.append(std::to_string(params.size()));
    message.append(params.size() == 1 ? " argument, found " : " arguments, found ");
    message.append(std::to_string(args.size()));
    Report(kWrongArguments, std::move(message), expr.span);
  }

  for (size_t i = 0; i < args.size(); ++i) {
    const TypeId expected = i < params.size() ? params[i] : TypeId{};
    const TypeId found = Check(args[i], expected);
    if (expected.is_valid() && !Accepts(expected, found)) {
      ReportMismatch(expected, found, ast_.Get(args[i]).span);
    }
  }
  return result;
}

TypeId Checker::CheckIndex(const Expr& expr) {
  const TypeId held = Check(expr.lhs, TypeId{});
  Check(expr.rhs, TypeId{});

  // Through a reference, like a field and a method already are: a
  // `&mut [40]int` parameter is an array that happens to live in the
  // caller's frame, and making the callee write `(*xs)[i]` buys nothing
  // (D167).
  const TypeId container =
      types_->KindOf(held) == TypeKind::kReference
          ? types_->ElementOf(held)
          : held;
  const TypeKind kind = types_->KindOf(container);
  if (kind == TypeKind::kSlice || kind == TypeKind::kArray) {
    // Indexing with a range produces a slice of the same element type.
    if (ast_.Contains(expr.rhs) &&
        ast_.Get(expr.rhs).kind == ExprKind::kRange) {
      return types_->Slice(types_->ElementOf(container),
                           types_->Get(container).is_soa);
    }
    return types_->ElementOf(container);
  }
  if (!types_->IsError(container)) {
    std::string message = "`";
    message.append(Describe(container));
    message.append("` cannot be indexed");
    Report(kNotIndexable, std::move(message), expr.span);
  }
  return types_->error();
}

void Checker::RecordCallEffects(ExprId callee, ExprId call) {
  if (!ast_.Contains(callee)) {
    return;
  }
  const Expr& expr = ast_.Get(callee);
  Symbol name;
  if (expr.kind == ExprKind::kPath) {
    name = expr.name;
  } else if (expr.kind == ExprKind::kField ||
             expr.kind == ExprKind::kOptionalField) {
    name = expr.name;
  }
  if (!name.is_valid()) {
    return;
  }

  const Binding* binding = Lookup(name);
  if (binding == nullptr || binding->item == Binding::kNoItem) {
    return;
  }
  if (binding->is_task && call != started_) {
    std::string message = "`";
    message.append(interner_->Text(name));
    message.append("` is a `task`, so calling it starts work rather than "
                   "doing it");
    Report(kTaskNotWaited, std::move(message), ast_.Get(callee).span,
           "write `wait` to suspend until it finishes, or `spawn` to let it "
           "run alongside");
  }
  if (binding->is_foreign) {
    // Calling out of the language is the `unsafe` effect, which is how the
    // rules already in place apply to the FFI: a caller has to state it,
    // and a package that forbids `unsafe` cannot reach C at all. There is
    // no separate diagnostic to write and no second rule to keep in step.
    result_.direct_effects.Add(
        types_->BuiltinEffectId(BuiltinEffect::kUnsafe));
  }
  // `spawn` starts the work without waiting for it, so the spawner does not
  // suspend -- but it is still responsible for what it started, which is
  // why every other effect carries over. A sandboxed package cannot spawn
  // its way to the network.
  const bool drop_async = spawned_ && call == started_;
  ChargeBinding(binding, drop_async);
}

void Checker::ChargeBinding(const Binding* binding, bool drop_async) {
  NoteCall(binding->file, binding->item);
  if (const EffectRow* declared =
          resolution_->DeclaredRow(binding->file, binding->item);
      declared != nullptr) {
    // A declared `uses unsafe` on a function WITH A BODY absorbs the
    // obligation rather than
    // re-exporting it. That is the difference between `unsafe` and every
    // other effect: `io` and `net` are capabilities -- what the code needs
    // the outside world for, which a caller has to know about -- while
    // `unsafe` is an audit, and the declaration that states it is the one
    // taking responsibility. Without this a safe wrapper over an unsafe
    // primitive would be impossible, and `std.io.Print` would make every
    // program that prints an unsafe one. A bodiless primitive has nothing
    // vouching for it -- its `uses unsafe` is the promise the caller must
    // make, exactly like `extern` (D221).
    const EffectId unsafe_effect =
        types_->BuiltinEffectId(BuiltinEffect::kUnsafe);
    const EffectId async_effect =
        types_->BuiltinEffectId(BuiltinEffect::kAsync);
    for (EffectId effect : declared->effects()) {
      if (effect == unsafe_effect && binding->has_body) {
        continue;
      }
      if (drop_async && effect == async_effect) {
        continue;
      }
      result_.direct_effects.Add(effect);
    }
    return;
  }
  // Not stated here, so the row has to come from the module-wide fixpoint.
  result_.callees.emplace_back(binding->file, binding->item);
  if (drop_async) {
    // The fixpoint unions whole rows, so a spawn of an undeclared task is
    // recorded as a plain call and the `async` it brings in is accepted
    // here. Declaring the task's row is what makes the distinction exact,
    // which is one more reason an `offer`ed task states its effects.
    result_.spawned_callees.emplace_back(binding->file, binding->item);
  }
}

// `Vec3(x, y, z)` fills the type's fields in declaration order. A type with
// no fields known -- declared in a file this one cannot see -- still has its
// arguments checked, just without expectations.
// `Box<int>(7)` and `Box(7)`: which type a constructor is building.
//
// A generic type is its parameters until something binds them, and a
// constructor is where that happens -- from what the call wrote, or from
// the arguments when it wrote nothing. The result is the instance, so a
// field of it reads as `int` rather than as `T` and a layout can be taken
// of it at all.
TypeId Checker::Instantiate(ExprId id, const Expr& expr, TypeId declared) {
  const std::span<const TypeId> parameters =
      resolution_->ParametersOf(declared);
  if (parameters.empty()) {
    return declared;
  }

  TypeStore::Substitution substitution;
  if (HasFlag(expr.flags, ExprFlags::kHasGenericArgs)) {
    size_t position = 0;
    for (TypeRefId written : ast_.Get(expr.generic_args)) {
      if (position >= parameters.size()) {
        break;
      }
      substitution.emplace_back(parameters[position++],
                                resolver_.Resolve(ast_, written));
    }
  } else {
    // Inferred the same way a call's parameters are: the field a value is
    // being given to is the pattern, and the value is what matches it.
    const std::span<const ExprId> args = ast_.Get(expr.args);
    std::vector<TypeId> fields;
    for (const Member& member : resolution_->MembersOf(declared)) {
      if (member.kind == MemberKind::kField) {
        fields.push_back(member.type);
      }
    }
    for (size_t i = 0; i < args.size() && i < fields.size(); ++i) {
      types_->Unify(fields[i], Check(args[i], TypeId{}), &substitution);
    }
  }

  std::vector<TypeId> arguments;
  arguments.reserve(parameters.size());
  for (TypeId parameter : parameters) {
    TypeId bound = types_->Substitute(parameter, substitution);
    // Whether anything said what this parameter is. Asked of the
    // substitution rather than inferred from the answer, because a
    // parameter may legitimately be bound to *itself*: inside
    // `class Holder<T>`, `Holder<T>(value)` binds `T` to the `T` in
    // scope, and reading that as "nothing bound it" made a type unable to
    // construct one of itself (D102).
    bool named = false;
    for (const auto& [written, value] : substitution) {
      named |= written == parameter;
    }
    if (!named && bound == parameter) {
      // Nothing bound it: say so where it was written, rather than
      // letting a type parameter travel into a layout.
      std::string message = "`";
      message.append(Describe(declared));
      message.append("` needs its type argument written: `");
      message.append(Describe(declared));
      message.append("<...>`");
      Report(kCannotInfer, std::move(message), expr.span);
      bound = types_->error();
    }
    arguments.push_back(bound);
  }
  CheckBounds(substitution, expr.span);
  RecordInstantiation(id, substitution);
  return types_->Instance(declared, arguments);
}

void Checker::CheckConstructor(const Expr& expr, TypeId type) {
  if (types_->IsForeignClass(type)) {
    std::string message = "`";
    message.append(Describe(type));
    message.append("` is an `extern class`: the host makes its objects");
    Report(kForeignClass, std::move(message), expr.span, "ask the host for one, or take it as a parameter");
    return;
  }
  const std::span<const ExprId> args = ast_.Get(expr.args);

  // A generic type keeps its members on the declaration, with the
  // instance's arguments standing in for its parameters.
  TypeStore::Substitution substitution;
  TypeId named = type;
  if (types_->KindOf(named) == TypeKind::kInstance) {
    const TypeId base = types_->Get(named).base;
    const std::span<const TypeId> parameters = resolution_->ParametersOf(base);
    const std::vector<TypeId> arguments = types_->ArgumentsOf(named);
    for (size_t i = 0; i < parameters.size() && i < arguments.size(); ++i) {
      substitution.emplace_back(parameters[i], arguments[i]);
    }
    named = base;
  }

  // The fields a constructor takes, in the order the object lays them
  // out: what the class inherits first, root first, then its own. A
  // derived object *is* a base object at the same address (D84), so the
  // base's fields come first here too.
  std::vector<TypeId> chain;
  for (TypeId current = named; current.is_valid();
       current = resolution_->BaseOf(current)) {
    chain.push_back(current);
  }
  std::reverse(chain.begin(), chain.end());

  std::vector<TypeId> fields;
  for (TypeId current : chain) {
    for (const Member& member : resolution_->MembersOf(current)) {
      if (member.kind == MemberKind::kField) {
        fields.push_back(substitution.empty()
                             ? member.type
                             : types_->Substitute(member.type, substitution));
      }
    }
  }

  // No arguments is always allowed: it asks for the default value.
  if (!args.empty() && !fields.empty() && args.size() != fields.size()) {
    std::string message = "`";
    message.append(Describe(type));
    message.append("` has ");
    message.append(std::to_string(fields.size()));
    message.append(fields.size() == 1 ? " field, found " : " fields, found ");
    message.append(std::to_string(args.size()));
    message.append(args.size() == 1 ? " argument" : " arguments");
    Report(kWrongArguments, std::move(message), expr.span);
  }

  const bool positional = args.size() == fields.size();
  for (size_t i = 0; i < args.size(); ++i) {
    const TypeId expected = positional ? fields[i] : TypeId{};
    const TypeId found = Check(args[i], expected);
    if (expected.is_valid() && !Accepts(expected, found)) {
      ReportMismatch(expected, found, ast_.Get(args[i]).span);
    }
  }
}

std::vector<TypeId> Checker::BoundsOf(TypeId type) {
  if (types_->KindOf(type) == TypeKind::kParameter) {
    return types_->ArgumentsOf(type);
  }
  std::vector<TypeId> bounds;
  if (types_->KindOf(type) != TypeKind::kProjection) {
    return bounds;
  }
  const Symbol name = types_->Get(type).name;
  for (const TypeId owner : BoundsOf(types_->Get(type).base)) {
    const Member* associated = resolution_->FindMember(owner, name);
    if (associated != nullptr && associated->kind == MemberKind::kAssociatedType && associated->type.is_valid()) {
      bounds.push_back(associated->type);
    }
  }
  return bounds;
}

const Member* Checker::MemberThroughBounds(TypeId receiver, Symbol name, TypeId* ability) {
  for (const TypeId bound : BoundsOf(receiver)) {
    if (const Member* member = resolution_->FindMember(bound, name)) {
      *ability = bound;
      return member;
    }
  }
  return nullptr;
}

TypeId Checker::AsSelf(TypeId type, TypeId ability, TypeId receiver) {
  return types_->AnswerProjections(type, [&](TypeId owner, Symbol name) {
    return types_->Projection(owner == ability ? receiver : owner, name);
  });
}

TypeId Checker::Normalize(TypeId type) {
  return types_->AnswerProjections(type, [&](TypeId owner, Symbol name) {
    const TypeKind kind = types_->KindOf(owner);
    if (kind == TypeKind::kNamed || kind == TypeKind::kInstance) {
      const Member* answer = resolution_->FindMember(owner, name);
      if (answer != nullptr && answer->kind == MemberKind::kAssociatedType && answer->type.is_valid()) {
        return Normalize(answer->type);
      }
    }
    return types_->Projection(owner, name);
  });
}

TypeId Checker::CheckField(const Expr& expr) {
  const TypeId object = Check(expr.lhs, TypeId{});

  if (expr.kind == ExprKind::kField && types_->IsOptional(object)) {
    // Plain `.` on an optional is the bug nullability exists to catch; `?.`
    // is the form that handles absence.
    std::string message = "`";
    message.append(Describe(object));
    message.append("` may be absent");
    Report(kMaybeNull, std::move(message), expr.span,
           "use `?.` or check it with `if let` first");
    return types_->error();
  }

  // `?.` on an optional reaches through to the member of the inner type, and
  // the whole expression is optional again.
  const bool through_optional =
      expr.kind != ExprKind::kField && types_->IsOptional(object);
  TypeId subject = through_optional ? types_->ElementOf(object) : object;
  // A member reached through a reference is the member of what it refers
  // to.
  //
  // Without this an owning value could not be lent at all: handing a
  // `Builder` to a helper moves it (D87), so the way to let the helper
  // write into it is `&mut Builder` -- and a reference with no members
  // made that unusable (D109).
  if (types_->KindOf(subject) == TypeKind::kReference) {
    subject = types_->ElementOf(subject);
  }
  TypeId named = types_->StripWrappers(subject);

  // `Holder<int>` keeps its members on `Holder`, with the instance's
  // arguments standing in for the declaration's parameters. Without this a
  // field of a generic type resolves to nothing at all.
  TypeStore::Substitution substitution;
  if (types_->KindOf(named) == TypeKind::kInstance) {
    const TypeId base = types_->Get(named).base;
    const std::span<const TypeId> parameters =
        resolution_->ParametersOf(base);
    const std::vector<TypeId> arguments = types_->ArgumentsOf(named);
    for (size_t i = 0; i < parameters.size() && i < arguments.size(); ++i) {
      substitution.emplace_back(parameters[i], arguments[i]);
    }
    named = base;
  }

  // A member of a type parameter is a member of what it is bound to.
  // `fun Sum<T: Shape>(s: T)` can call `s.Area()` because the bound says
  // every T has one -- which is what a bound is *for*, and without this it
  // was a type the checker had nothing to say about. The same for `C.Walker`,
  // whose bound is the one its ability declared (D212).
  TypeId self_ability;
  const TypeId receiver = named;
  if (types_->KindOf(named) == TypeKind::kParameter || types_->KindOf(named) == TypeKind::kProjection) {
    if (MemberThroughBounds(named, expr.name, &self_ability) != nullptr) {
      named = self_ability;
    }
  }

  if (types_->IsError(named)) {
    return types_->error();
  }

  // A string is a pointer and a length, and saying so is what lets the
  // standard library write `Print` in Wolang: the platform floor needs the
  // bytes and how many there are, and nothing else about a string.
  if (types_->KindOf(named) == TypeKind::kStr ||
      types_->KindOf(named) == TypeKind::kString) {
    const std::string_view field = interner_->Text(expr.name);
    if (field == "data") {
      return types_->Pointer(types_->Int(IntWidth::k8, /*is_signed=*/false),
                             /*is_mutable=*/false);
    }
    if (field == "length") {
      return types_->Int(IntWidth::k64, /*is_signed=*/false);
    }
  }

  // The elements a view points at. The low-level half of a slice, for the
  // code that hands a buffer to something outside the language.
  if (interner_->Text(expr.name) == "data" &&
      types_->KindOf(named) == TypeKind::kSlice) {
    return types_->Pointer(types_->ElementOf(named), /*is_mutable=*/true);
  }

  // How many elements there are. An `int` rather than an unsigned word,
  // because it is read in arithmetic and compared with literals far more
  // often than it is handed to an allocator: `for i in 0..buf.len` is the
  // reason, and a mixed-width add is an error in this language (D33).
  if (interner_->Text(expr.name) == "len" &&
      (types_->KindOf(named) == TypeKind::kArray ||
       types_->KindOf(named) == TypeKind::kSlice ||
       types_->KindOf(named) == TypeKind::kStr ||
       types_->KindOf(named) == TypeKind::kString)) {
    return types_->default_int();
  }

  // `s.Emit(args)` and `s.Connect(handler)`: the two things a signal does.
  // Recognized here because a signal's type is the type of what listens
  // to it, so both sides check against the same thing -- and a handler of
  // the wrong shape is reported where it is written.
  if (const Member* signal = SignalOperand(expr.lhs); signal != nullptr) {
    const std::string_view operation = interner_->Text(expr.name);
    if (operation == "Emit") {
      // Emitting takes what the signal declares and gives nothing back.
      return types_->Function(types_->ArgumentsOf(signal->type),
                              types_->void_type());
    }
    if (operation == "Connect" || operation == "Disconnect") {
      return types_->Function({signal->type}, types_->void_type());
    }
    std::string message = "a signal has `Emit`, `Connect` and "
                          "`Disconnect`, and no `";
    message.append(operation);
    message.append("`");
    Report(kUnknownMember, std::move(message), expr.span);
    return types_->error();
  }

  // A type with no members at all: an array, a number, a tuple. Saying so
  // is the point -- a field access that resolved to nothing used to type
  // as an error with no diagnostic, and then lowering made something up.
  if (types_->KindOf(named) != TypeKind::kNamed &&
      types_->KindOf(named) != TypeKind::kInstance &&
      types_->KindOf(named) != TypeKind::kParameter &&
      types_->KindOf(named) != TypeKind::kTuple) {
    std::string message = "`";
    message.append(Describe(named));
    message.append("` has no member named `");
    message.append(interner_->Text(expr.name));
    message.append("`");
    Report(kUnknownMember, std::move(message), expr.span);
    return types_->error();
  }

  const Member* member = resolution_->FindMember(named, expr.name);
  if (member == nullptr) {
    // What the class inherits: a field or a method of a base is reached
    // through a handle to the derived class without any qualification,
    // because the object has it (D84).
    member = resolution_->FindInheritedMember(named, expr.name);
  }
  if (member == nullptr) {
    if (!resolution_->MembersOf(named).empty()) {
      std::string message = "`";
      message.append(Describe(named));
      message.append("` has no member named `");
      message.append(interner_->Text(expr.name));
      message.append("`");
      Report(kUnknownMember, std::move(message), expr.span);
    }
    // A type whose members are not known is declared in a file this one
    // cannot see. That is already reported as an unresolved import.
    return types_->error();
  }

  TypeId type = types_->Substitute(member->type, substitution);
  if (self_ability.is_valid()) {
    type = AsSelf(type, self_ability, receiver);
  }
  type = Normalize(type);
  return through_optional ? types_->Optional(type) : type;
}

// `if p != null { ... }` narrows `p` for the body, the same way `if let`
// does. Shadowing the name in the current scope is what makes it work for
// the body only, with no flow graph to carry.
void Checker::NarrowOnNullCheck(ExprId condition) {
  if (!ast_.Contains(condition)) {
    return;
  }
  const Expr& test = ast_.Get(condition);
  if (test.kind != ExprKind::kBinary || test.op != TokenKind::kBangEq) {
    return;
  }

  // Either order: `p != null` and `null != p` say the same thing.
  const ExprId sides[2] = {test.lhs, test.rhs};
  for (int i = 0; i < 2; ++i) {
    const ExprId name_side = sides[i];
    const ExprId null_side = sides[1 - i];
    if (!ast_.Contains(name_side) || !ast_.Contains(null_side)) {
      continue;
    }
    const Expr& name_expr = ast_.Get(name_side);
    if (ast_.Get(null_side).kind != ExprKind::kNullLiteral ||
        name_expr.kind != ExprKind::kPath) {
      continue;
    }
    const Binding* binding = Lookup(name_expr.name);
    if (binding == nullptr || !types_->IsOptional(binding->type)) {
      continue;
    }
    Bind(name_expr.name, types_->ElementOf(binding->type),
         binding->is_mutable);
    return;
  }
}

TypeId Checker::CheckIf(const Expr& expr, TypeId expected) {
  PushScope();
  if (HasFlag(expr.flags, ExprFlags::kHasBinding)) {
    // `if let x = optional` narrows the optional for the body, which is the
    // whole point of flow typing.
    const TypeId subject = Check(expr.lhs, TypeId{});
    const TypeId narrowed = types_->IsOptional(subject)
                                ? types_->ElementOf(subject)
                                : subject;
    BindPattern(expr.pattern, narrowed);
  } else {
    const TypeId condition = Check(expr.lhs, types_->bool_type());
    if (!Accepts(types_->bool_type(), condition)) {
      ReportMismatch(types_->bool_type(), condition, expr.span);
    }
    NarrowOnNullCheck(expr.lhs);
  }

  // Each branch stands where the `if` itself stands, so a `wait` is
  // allowed in one exactly when it would have been allowed in place of the
  // whole expression.
  const bool branch_allows_wait = block_allows_wait_;
  wait_allowed_ = branch_allows_wait;
  const TypeId then_type = Check(expr.rhs, expected);
  PopScope();

  if (!expr.extra.is_valid()) {
    // Without an `else` the value is only produced on one path, so the whole
    // expression is a statement.
    return types_->void_type();
  }
  wait_allowed_ = branch_allows_wait;
  const TypeId else_type =
      Check(expr.extra, expected.is_valid() ? expected : then_type);
  if (expected.is_valid()) {
    return expected;
  }
  if (types_->KindOf(then_type) == TypeKind::kNever) {
    return else_type;
  }
  if (types_->KindOf(else_type) == TypeKind::kNever) {
    return then_type;
  }
  if (then_type != else_type && !Accepts(then_type, else_type)) {
    ReportMismatch(then_type, else_type, expr.span);
  }
  return then_type;
}

// A `match` has to cover every value its subject can take, because a value
// that falls through has nowhere to go. Only bool, optionals and enums have a
// value set the compiler can enumerate; anything else needs a catch-all arm.
void Checker::CheckExhaustive(const Expr& expr, TypeId subject) {
  if (types_->IsError(subject)) {
    return;
  }

  // An enum's variants are its members. Gathered first, because a bare name
  // in an arm is the short form of a variant when the subject is that enum,
  // and an ordinary binding -- a catch-all -- when it is not.
  std::vector<Symbol> variants;
  for (const Member& member : resolution_->MembersOf(
           types_->StripWrappers(subject))) {
    if (member.kind == MemberKind::kVariant) {
      variants.push_back(member.name);
    }
  }
  const auto is_variant = [&variants](Symbol name) {
    for (Symbol variant : variants) {
      if (variant == name) {
        return true;
      }
    }
    return false;
  };

  bool has_catch_all = false;
  bool covers_true = false;
  bool covers_false = false;
  bool covers_null = false;
  bool covers_present = false;
  std::vector<Symbol> covered_variants;

  for (MatchArmId arm_id : ast_.Get(expr.arms)) {
    const MatchArm& arm = ast_.Get(arm_id);
    // A guarded arm may not run, so it covers nothing.
    if (arm.guard.is_valid()) {
      continue;
    }
    for (PatternId pattern_id : ast_.Get(arm.patterns)) {
      if (!ast_.Contains(pattern_id)) {
        continue;
      }
      const Pattern& pattern = ast_.Get(pattern_id);
      switch (pattern.kind) {
        case PatternKind::kBinding:
          if (is_variant(pattern.name)) {
            covered_variants.push_back(pattern.name);
          } else {
            has_catch_all = true;
          }
          break;
        case PatternKind::kWildcard:
        case PatternKind::kRest:
          has_catch_all = true;
          break;
        case PatternKind::kPresent:
          covers_present = true;
          break;
        case PatternKind::kLiteral: {
          if (!ast_.Contains(pattern.literal)) {
            break;
          }
          const Expr& literal = ast_.Get(pattern.literal);
          if (literal.kind == ExprKind::kBoolLiteral) {
            if (literal.aux != 0) {
              covers_true = true;
            } else {
              covers_false = true;
            }
          } else if (literal.kind == ExprKind::kNullLiteral) {
            covers_null = true;
          }
          break;
        }
        case PatternKind::kVariant:
        case PatternKind::kStruct: {
          // The variant name is the last segment of the pattern's path;
          // `name` holds the binding of a struct pattern's field instead.
          const std::span<const Symbol> path = ast_.Get(pattern.path);
          if (!path.empty()) {
            covered_variants.push_back(path.back());
          } else if (pattern.name.is_valid()) {
            covered_variants.push_back(pattern.name);
          }
          break;
        }
        default:
          break;
      }
    }
  }

  if (has_catch_all) {
    return;
  }

  const auto report = [&](std::string missing) {
    std::string message = "`match` does not cover every value of `";
    message.append(Describe(subject));
    message.append("`");
    Report(kNonExhaustive, std::move(message), expr.span,
           "add an arm for " + missing + ", or a `_` arm");
  };

  if (types_->KindOf(subject) == TypeKind::kBool) {
    if (!covers_true || !covers_false) {
      report(covers_true ? "`false`" : "`true`");
    }
    return;
  }

  if (types_->IsOptional(subject)) {
    if (!covers_null || !covers_present) {
      report(covers_null ? "`x?`" : "`null`");
    }
    return;
  }

  // The uncovered variants are known by name, so they can be listed.
  if (!variants.empty()) {
    std::string missing;
    for (Symbol variant : variants) {
      bool found = false;
      for (Symbol covered : covered_variants) {
        if (covered == variant) {
          found = true;
          break;
        }
      }
      if (found) {
        continue;
      }
      if (!missing.empty()) {
        missing.append(", ");
      }
      missing.push_back('`');
      missing.append(interner_->Text(variant));
      missing.push_back('`');
    }
    if (!missing.empty()) {
      report(std::move(missing));
    }
    return;
  }

  // Every other type has more values than a `match` can spell out, so the
  // only exhaustive form is one with a catch-all.
  report("every remaining value");
}

TypeId Checker::CheckMatch(const Expr& expr, TypeId expected) {
  const TypeId subject = Check(expr.lhs, TypeId{});
  TypeId result;

  for (MatchArmId arm_id : ast_.Get(expr.arms)) {
    const MatchArm& arm = ast_.Get(arm_id);
    PushScope();
    for (PatternId pattern : ast_.Get(arm.patterns)) {
      BindPattern(pattern, subject);
    }
    if (arm.guard.is_valid()) {
      const TypeId guard = Check(arm.guard, types_->bool_type());
      if (!Accepts(types_->bool_type(), guard)) {
        ReportMismatch(types_->bool_type(), guard, arm.span);
      }
    }
    // An arm stands where the `match` stands.
    wait_allowed_ = block_allows_wait_;
    const TypeId body = Check(arm.body, expected.is_valid() ? expected
                                                            : result);
    PopScope();

    if (!result.is_valid() ||
        types_->KindOf(result) == TypeKind::kNever) {
      result = body;
    } else if (types_->KindOf(body) != TypeKind::kNever &&
               !Accepts(result, body)) {
      ReportMismatch(result, body, arm.span);
    }
  }

  CheckExhaustive(expr, subject);

  if (expected.is_valid()) {
    return expected;
  }
  return result.is_valid() ? result : types_->void_type();
}

TypeId Checker::CheckLoop(const Expr& expr) {
  PushScope();
  ++loop_depth_;

  if (expr.kind == ExprKind::kWhile) {
    if (HasFlag(expr.flags, ExprFlags::kHasBinding)) {
      const TypeId subject = Check(expr.lhs, TypeId{});
      BindPattern(expr.pattern, types_->IsOptional(subject)
                                    ? types_->ElementOf(subject)
                                    : subject);
    } else {
      const TypeId condition = Check(expr.lhs, types_->bool_type());
      if (!Accepts(types_->bool_type(), condition)) {
        ReportMismatch(types_->bool_type(), condition, expr.span);
      }
    }
  } else if (expr.kind == ExprKind::kFor ||
             expr.kind == ExprKind::kForeach) {
    const TypeId sequence = Check(expr.lhs, TypeId{});
    // A slice or array yields its elements; a range yields its bound type.
    TypeId element = types_->ElementOf(sequence);
    // A query yields the first component it names: `Query<mut Position,
    // Velocity>` walks the entities that have both and binds the
    // `Position` of each. The arguments rather than the element, because
    // the components *are* the arguments of the instance (D101).
    bool through_reference = false;
    if (types_->KindOf(sequence) == TypeKind::kInstance &&
        types_->Get(sequence).base == types_->query()) {
      const std::vector<TypeId> named = types_->ArgumentsOf(sequence);
      if (!named.empty()) {
        element = named.front();
        if (types_->KindOf(element) == TypeKind::kReference) {
          // Written `mut`: the binding is assignable, which is what makes
          // a system that writes say so once, in its signature.
          through_reference = true;
          element = types_->ElementOf(element);
        }
      }
    }
    // A type that says how to walk itself yields what its iterator's
    // `Value` gives back (D96). Three names and no ability, because an ability
    // would need an associated type to say "what I walk over".
    if (const TypeId walked = IteratedElement(sequence); walked.is_valid()) {
      element = walked;
    } else if (!element.is_valid()) {
      element = types_->IsNumeric(sequence) ? sequence : types_->error();
      // A query is walked by the scheduler rather than by a method, so it
      // needs no `Iterate` (D82).
      const bool is_query =
          types_->KindOf(sequence) == TypeKind::kInstance &&
          types_->Get(sequence).base == types_->query();
      if (!types_->IsError(sequence) && !types_->IsNumeric(sequence) &&
          !is_query) {
        std::string message = "`";
        message.append(Describe(sequence));
        message.append("` cannot be walked over");
        Report(kNotIterable, std::move(message), expr.span,
               "give it `fun Iterate()` returning something with `Next` "
               "and `Value`");
      }
    }
    // A query yields the component it was declared over, and `mut` on
    // that component is what makes the loop's binding assignable: a
    // system that writes says so in its signature, so the body does not
    // have to repeat it.
    // `mut Position` resolves to a mutable reference to the component,
    // so the binding is assignable exactly when the query said `mut`.
    BindPattern(expr.pattern, element, through_reference);
  }

  // A loop body's value is discarded, so it is a statement position: a
  // `wait` as the last thing in it is as fine as one anywhere else.
  wait_allowed_ = true;
  Check(expr.rhs, TypeId{});
  --loop_depth_;
  PopScope();
  return types_->void_type();
}

TypeId Checker::CheckRange(const Expr& expr) {
  TypeId bound;
  if (expr.lhs.is_valid()) {
    bound = Check(expr.lhs, TypeId{});
  }
  if (expr.rhs.is_valid()) {
    const TypeId upper = Check(expr.rhs, bound);
    if (!bound.is_valid()) {
      bound = upper;
    } else if (!Accepts(bound, upper)) {
      ReportMismatch(bound, upper, expr.span);
    }
  }
  if (!bound.is_valid()) {
    bound = types_->default_int();
  }
  // A range is a sequence of its bound type, so `for i in 0..10` binds an
  // integer without a special case.
  return types_->Slice(Adapt(bound, TypeId{}, expr.span), false);
}

TypeId Checker::Check(ExprId id, TypeId expected, bool keep_open) {
  // A `wait` is only allowed where nothing of the surrounding expression is
  // still needed afterwards, so the permission does not reach any child.
  const bool wait_allowed_here = wait_allowed_;
  wait_allowed_ = false;
  if (!ast_.Contains(id)) {
    return types_->error();
  }
  const Expr& expr = ast_.Get(id);
  TypeId type;

  switch (expr.kind) {
    case ExprKind::kError:
      type = types_->error();
      break;

    case ExprKind::kIntLiteral:
    case ExprKind::kFloatLiteral:
    case ExprKind::kStringLiteral:
    case ExprKind::kCharLiteral:
    case ExprKind::kBoolLiteral:
    case ExprKind::kNullLiteral:
    case ExprKind::kPointerNull:
      type = CheckLiteral(expr, expected);
      break;

    case ExprKind::kStringText:
      type = types_->str();
      break;

    case ExprKind::kPath:
      type = CheckPath(expr);
      break;

    case ExprKind::kSelf:
      type = self_type_.is_valid() ? self_type_ : types_->error();
      break;

    case ExprKind::kUnary:
      type = CheckUnary(expr, expected);
      break;
    case ExprKind::kBinary:
      type = CheckBinary(expr, expected);
      break;
    case ExprKind::kAssign:
      type = CheckAssign(expr);
      break;
    case ExprKind::kRange:
      type = CheckRange(expr);
      break;

    case ExprKind::kCast: {
      type = resolver_.Resolve(ast_, expr.type);
      // A literal too big for `int` is read at the type it is cast to:
      // `4294967297 as s64` is that number, not the `int` it would have been
      // cut to first (D225).
      const bool wide_literal =
          types_->IsNumeric(type) && IsWideLiteral(expr.lhs);
      const TypeId source = Check(expr.lhs, wide_literal ? type : TypeId{});
      // Bytes as text, and back. Both are a pointer and a length, so the
      // conversion costs nothing at run time -- but a `text` is valid
      // UTF-8 and a `[]u8` is not, so going that way is a promise the
      // caller makes, and `unsafe` is how this language records a promise.
      // `std` will offer the checked version; this is the floor it needs.
      const bool views = IsBytesAndText(source, type);
      // An address as a number: comparing one with zero, hashing one,
      // handing one to C as a word. `unsafe`, because a number is not an
      // address and the promise that this one is belongs to the caller
      // (D115).
      const bool address =
          (types_->KindOf(source) == TypeKind::kPointer ||
           types_->KindOf(source) == TypeKind::kReference) &&
          types_->IsInteger(type);
      // And a number as an address, which is how a binding says NULL:
      // `0 as *mut u8` for a handle that is not one yet, and for the
      // `null` that half of C's functions take and give back. Without
      // it a binding has to invent a sentinel -- which is what `lib/tls`
      // did, and it was a freed address (D156).
      const bool pointer =
          types_->IsInteger(source) &&
          types_->KindOf(type) == TypeKind::kPointer;
      if (views || address || pointer) {
        result_.direct_effects.Add(
            types_->BuiltinEffectId(BuiltinEffect::kUnsafe));
      }
      // `as` is a numeric conversion and nothing else. It is the escape
      // hatch D33 left open by removing implicit widening, not a
      // reinterpretation of arbitrary bits -- that is what `unsafe` is for.
      if (!views && !address && !pointer && type.is_valid() &&
          source.is_valid() &&
          !types_->IsError(source) && !types_->IsError(type) &&
          types_->KindOf(source) != TypeKind::kNever && source != type &&
          !(types_->IsNumeric(source) && types_->IsNumeric(type))) {
        std::string message = "cannot convert `";
        message.append(Describe(source));
        message.append("` to `");
        message.append(Describe(type));
        message.append("` with `as`");
        Report(kBadConversion, std::move(message), expr.span);
      }
      break;
    }

    case ExprKind::kCall:
      type = CheckCall(id, expr);
      break;
    case ExprKind::kIndex:
      type = CheckIndex(expr);
      break;
    case ExprKind::kField:
    case ExprKind::kOptionalField:
      type = CheckField(expr);
      break;

    case ExprKind::kTry: {
      const TypeId inner = Check(expr.lhs, TypeId{});
      // `?` unwraps one layer of fallibility or absence.
      type = types_->IsFallible(inner) || types_->IsOptional(inner)
                 ? types_->ElementOf(inner)
                 : inner;
      break;
    }

    case ExprKind::kAwait:
    case ExprKind::kSpawn: {
      // A call under `wait` or `spawn` is the one place a task may be
      // called, so the operand is checked knowing that.
      const ExprId previous = started_;
      const bool previous_spawned = spawned_;
      started_ = expr.lhs;
      spawned_ = expr.kind == ExprKind::kSpawn;
      type = Check(expr.lhs, TypeId{});
      started_ = previous;
      spawned_ = previous_spawned;
      if (expr.kind == ExprKind::kSpawn) {
        // `spawn` starts the work and hands back a handle to it, not the
        // result it will have. A handle is an integer until there is a
        // `Task<T>` to make it a type of its own.
        type = types_->Int(IntWidth::k32, /*is_signed=*/true);
      }
      if (expr.kind == ExprKind::kAwait && !wait_allowed_here) {
        Report(kWaitNotWholeExpression,
               "`wait` has to be the whole of the expression it is in",
               expr.span,
               "give it a name first: `const x = wait ...`, then use `x`");
      }
      if (expr.kind == ExprKind::kAwait) {
        // `wait` is where a function suspends, and `async` in the row is
        // how that becomes checkable: a context that forbids the effect
        // forbids suspending, the same way it forbids `rand`.
        result_.direct_effects.Add(
            types_->BuiltinEffectId(BuiltinEffect::kAsync));
        if (!in_task_) {
          Report(kWaitOutsideTask,
                 "`wait` suspends, and only a `task fun` may suspend",
                 expr.span, "mark the function `task`");
        }
      }
      break;
    }

    case ExprKind::kStructLiteral:
      Check(expr.lhs, TypeId{});
      for (ExprId field : ast_.Get(expr.args)) {
        Check(field, TypeId{});
      }
      type = types_->error();
      break;

    case ExprKind::kArrayLiteral: {
      const std::span<const ExprId> elements = ast_.Get(expr.args);
      // An expected array or slice type tells the elements what to be.
      TypeId element_expectation;
      const TypeKind expected_kind = types_->KindOf(expected);
      if (expected_kind == TypeKind::kArray ||
          expected_kind == TypeKind::kSlice) {
        element_expectation = types_->ElementOf(expected);
      }

      TypeId element = element_expectation;
      for (ExprId item : elements) {
        const TypeId found = Check(item, element_expectation.is_valid()
                                             ? element_expectation
                                             : element);
        if (!element.is_valid() || types_->IsLiteral(element)) {
          element = found;
        } else if (!Accepts(element, found)) {
          ReportMismatch(element, found, ast_.Get(item).span);
        }
      }
      if (!element.is_valid()) {
        element = types_->error();
      }
      element = Adapt(element, element_expectation, expr.span);
      type = types_->Array(element, elements.size(), false);
      break;
    }

    case ExprKind::kTupleLiteral: {
      std::vector<TypeId> elements;
      const std::vector<TypeId> expectations =
          types_->KindOf(expected) == TypeKind::kTuple
              ? types_->ArgumentsOf(expected)
              : std::vector<TypeId>();
      const std::span<const ExprId> items = ast_.Get(expr.args);
      for (size_t i = 0; i < items.size(); ++i) {
        const TypeId expectation =
            i < expectations.size() ? expectations[i] : TypeId{};
        elements.push_back(
            Adapt(Check(items[i], expectation), expectation,
                  ast_.Get(items[i]).span));
      }
      type = types_->Tuple(elements);
      break;
    }

    case ExprKind::kClosure: {
      // What the context wants, so that `|n| n + 1` where a
      // `fun(int) -> int` is expected needs no annotation: the parameter
      // types come from the type the closure is being given to, which is
      // the only place they can come from (D106).
      const TypeId wanted =
          expected.is_valid() && types_->KindOf(expected) ==
                                     TypeKind::kFunction
              ? expected
              : TypeId{};
      const std::vector<TypeId> from_context =
          wanted.is_valid() ? types_->ArgumentsOf(wanted)
                            : std::vector<TypeId>();

      PushScope();
      // A closure sees its own parameters, the file's names, and -- by
      // copy -- whatever of the enclosing function's locals its body
      // reads. The locals are hidden from the *scope* walk so that each
      // read is noticed and recorded as a capture (D138).
      const size_t enclosing = HideLocals();
      closures_.push_back(id);
      {
        bool known = false;
        for (const ClosureFacts& facts : closure_facts_) {
          known |= facts.closure == id;
        }
        if (!known) {
          closure_facts_.push_back({id, false, {}});
        }
      }
      std::vector<TypeId> params;
      size_t position = 0;
      for (ParamId param_id : ast_.Get(expr.params)) {
        const Param& param = ast_.Get(param_id);
        TypeId param_type = param.type.is_valid()
                                ? resolver_.Resolve(ast_, param.type)
                                : TypeId{};
        if (!param_type.is_valid() && position < from_context.size()) {
          param_type = from_context[position];
        }
        if (!param_type.is_valid()) {
          std::string message = "the type of `";
          message.append(interner_->Text(param.name));
          message.append("` cannot be worked out here");
          Report(kClosureNeedsTypes, std::move(message), param.span,
                 "write it, or use the closure where a function type says "
                 "what it is");
          param_type = types_->error();
        }
        params.push_back(param_type);
        Bind(param.name, param_type, param.is_mutable, param.span);
        ++position;
      }
      TypeId declared = expr.type.is_valid()
                            ? resolver_.Resolve(ast_, expr.type)
                            : (wanted.is_valid()
                                   ? types_->ElementOf(wanted)
                                   : TypeId{});
      // A result the context has not worked out yet says nothing: what
      // the body gives back is the answer, and it is what the caller's
      // `T` is then inferred *from* (D119).
      if (declared.is_valid() && types_->MentionsParameter(declared)) {
        declared = TypeId{};
      }
      const TypeId body = Check(expr.lhs, declared);
      closures_.pop_back();
      ShowLocals(enclosing);
      PopScope();
      type = types_->Function(params,
                              declared.is_valid() ? declared : body);
      break;
    }

    case ExprKind::kBlock:
      block_allows_wait_ = wait_allowed_here;
      CheckBlock(id, expected);
      return result_.TypeOf(id);

    case ExprKind::kIf:
      block_allows_wait_ = wait_allowed_here;
      type = CheckIf(expr, expected);
      break;
    case ExprKind::kMatch:
      block_allows_wait_ = wait_allowed_here;
      type = CheckMatch(expr, expected);
      break;
    case ExprKind::kLoop:
    case ExprKind::kWhile:
    case ExprKind::kFor:
    case ExprKind::kForeach:
      type = CheckLoop(expr);
      break;

    case ExprKind::kUnsafe:
      // `unsafe { ... }` is itself the `unsafe` effect, so a caller that
      // forbids it does not have to look inside the block.
      result_.direct_effects.Add(
          types_->BuiltinEffectId(BuiltinEffect::kUnsafe));
      type = Check(expr.lhs, expected);
      break;

    case ExprKind::kRegion:
      type = Check(expr.lhs, expected);
      break;
  }

  // An expectation that still mentions a type parameter is a *pattern*,
  // not a type: it says what shape the argument has while inference works
  // out the rest, and adapting against it would report the very answer
  // that binds the parameter as a mismatch (D119).
  const bool pattern =
      expected.is_valid() && types_->MentionsParameter(expected);
  // What the expression produced, before the context had its say: an
  // owned string adapted to a borrowed one *is* an owned string, and
  // asking afterwards gets the answer the context gave (D133).
  const TypeId produced = type;
  if (!(keep_open && !expected.is_valid()) && !pattern) {
    if (!expected.is_valid() &&
        types_->KindOf(type) == TypeKind::kIntLiteral &&
        IsWideLiteral(id)) {
      // The literal is about to become an `int` and would be cut to fit.
      Report(kLiteralOutOfRange,
             "the literal does not fit in `int`; give it a type with "
             "`as s64` or an annotation",
             expr.span);
    }
    type = Adapt(type, expected, expr.span);
  }
  // An owned string handed where a borrowed one is wanted is a borrow, and
  // the flow analysis has to know: a borrow does not move.
  if (expected.is_valid() &&
      types_->KindOf(expected) == TypeKind::kStr &&
      types_->KindOf(produced) == TypeKind::kString) {
    result_.borrows.push_back(id);
  }
  Record(id, type);
  return type;
}

// -------------------------------------------------------------- patterns ----

void Checker::BindPattern(PatternId id, TypeId type,
                          bool is_mutable) {
  if (!ast_.Contains(id)) {
    return;
  }
  const Pattern& pattern = ast_.Get(id);
  switch (pattern.kind) {
    case PatternKind::kBinding: {
      // A bare name that is one of the errors a `T!` carries is a test
      // for that error, not a binding (D90).
      if (types_->KindOf(type) == TypeKind::kFallible) {
        const Binding* tag = Lookup(pattern.name);
        if (tag != nullptr && tag->kind == BindingKind::kErrorTag) {
          return;
        }
      }
      // A bare name that is a variant of the subject's enum is the short
      // form of that variant, not a new binding that would shadow it.
      const Member* member = resolution_->FindMember(
          types_->StripWrappers(type), pattern.name);
      if (member != nullptr && member->kind == MemberKind::kVariant) {
        return;
      }
      Bind(pattern.name, type,
           is_mutable || HasFlag(pattern.flags, PatternFlags::kMutable),
           pattern.span);
      return;
    }

    case PatternKind::kPresent: {
      // `n?` matches a present optional and binds what is inside.
      const TypeId inner = types_->IsOptional(type)
                               ? types_->ElementOf(type)
                               : type;
      for (PatternId element : ast_.Get(pattern.elements)) {
        BindPattern(element, inner, is_mutable);
      }
      return;
    }

    case PatternKind::kTuple: {
      const std::vector<TypeId> elements =
          types_->KindOf(type) == TypeKind::kTuple
              ? types_->ArgumentsOf(type)
              : std::vector<TypeId>();
      const std::span<const PatternId> patterns = ast_.Get(pattern.elements);
      for (size_t i = 0; i < patterns.size(); ++i) {
        BindPattern(patterns[i],
                    i < elements.size() ? elements[i] : types_->error(),
                    is_mutable);
      }
      return;
    }

    case PatternKind::kVariant:
    case PatternKind::kStruct: {
      // What the variant carries, in the order it was declared: its
      // member is a function type, and what it takes is what it carries
      // (D89).
      Symbol name = pattern.name;
      if (const std::span<const Symbol> path = ast_.Get(pattern.path);
          !path.empty()) {
        name = path.back();
      }
      std::vector<TypeId> carried;
      // `Ok(value)` on a `T!` is the built-in variant: what it carries is
      // the `T`. Without this the binding got an error type, which
      // propagates silently -- so the first thing to notice was a
      // reference to it, a long way from the cause (D144).
      if (types_->KindOf(type) == TypeKind::kFallible &&
          interner_->Text(name) == "Ok") {
        carried.push_back(types_->ElementOf(type));
      } else if (const Member* member = resolution_->FindMember(
                     types_->StripWrappers(type), name);
                 member != nullptr && member->kind == MemberKind::kVariant &&
                 types_->KindOf(member->type) == TypeKind::kFunction) {
        carried = types_->ArgumentsOf(member->type);
      }
      size_t position = 0;
      for (PatternId element : ast_.Get(pattern.elements)) {
        BindPattern(element, position < carried.size() ? carried[position]
                                                      : types_->error());
        ++position;
      }
      return;
    }

    case PatternKind::kLiteral: {
      if (!pattern.literal.is_valid()) {
        return;
      }
      // `null` matches the optional itself; every other literal matches what
      // is inside it, so `match count { 0 => ... }` on a `u8?` still catches
      // a literal that does not fit.
      const bool matches_absence =
          ast_.Contains(pattern.literal) &&
          ast_.Get(pattern.literal).kind == ExprKind::kNullLiteral;
      Check(pattern.literal,
            matches_absence ? type : types_->StripWrappers(type));
      return;
    }

    case PatternKind::kWildcard:
    case PatternKind::kRest:
    case PatternKind::kError:
      return;
  }
}

// ------------------------------------------------------------ statements ----

void Checker::CheckStmt(StmtId id) {
  if (!ast_.Contains(id)) {
    return;
  }
  const Stmt& stmt = ast_.Get(id);

  switch (stmt.kind) {
    case StmtKind::kLet:
    case StmtKind::kVar: {
      const TypeId declared = stmt.type.is_valid()
                                  ? resolver_.Resolve(ast_, stmt.type)
                                  : TypeId{};
      TypeId value = declared;
      if (stmt.expr.is_valid()) {
        wait_allowed_ = true;
        value = Check(stmt.expr, declared);
        if (declared.is_valid() && !Accepts(declared, value)) {
          ReportMismatch(declared, value, ast_.Get(stmt.expr).span);
          value = declared;
        }
      }
      // Without an annotation and without an initializer there is nothing to
      // infer from; the binding gets the error type and says so once.
      if (!value.is_valid()) {
        value = types_->error();
      }
      BindPattern(stmt.pattern, Adapt(value, declared, stmt.span),
                  stmt.kind == StmtKind::kVar);
      return;
    }

    case StmtKind::kExpr:
      wait_allowed_ = true;
      Check(stmt.expr, TypeId{});
      return;

    case StmtKind::kReturn: {
      if (!stmt.expr.is_valid()) {
        return;
      }
      wait_allowed_ = true;
      const TypeId found = Check(stmt.expr, return_type_);
      RefuseEscape(stmt.expr, "returned");
      if (return_type_.is_valid() && !Accepts(return_type_, found)) {
        ReportMismatch(return_type_, found, ast_.Get(stmt.expr).span);
      }
      return;
    }

    case StmtKind::kBreak:
    case StmtKind::kContinue:
      return;

    case StmtKind::kDefer:
      Check(stmt.expr, TypeId{});
      return;

    case StmtKind::kItem:
      {
        // A declaration inside a body: its type may be inferred, because
        // nothing outside the function can name it.
        const bool outer = at_module_level_;
        at_module_level_ = false;
        CheckItem(stmt.item);
        at_module_level_ = outer;
      }
      return;

    case StmtKind::kError:
      return;
  }
}

bool Checker::Diverges(StmtId id) const {
  if (!ast_.Contains(id)) {
    return false;
  }
  const Stmt& stmt = ast_.Get(id);
  switch (stmt.kind) {
    case StmtKind::kReturn:
    case StmtKind::kBreak:
    case StmtKind::kContinue:
      return true;
    case StmtKind::kExpr:
      return types_->KindOf(result_.TypeOf(stmt.expr)) == TypeKind::kNever;
    default:
      return false;
  }
}

void Checker::CheckBlock(ExprId id, TypeId expected) {
  const Expr& block = ast_.Get(id);
  PushScope();
  // A block's value stands where the block itself stands, so a `wait` is
  // allowed there exactly when it would have been allowed in place of the
  // block. `Check` cleared the flag on the way in, so it is restored for
  // the tail expression only.
  const bool tail_allows_wait = block_allows_wait_;

  const std::span<const StmtId> stmts = ast_.Get(block.stmts);
  TypeId type = types_->void_type();
  // Set by the first statement that cannot fall through, so that everything
  // after it is reported once rather than statement by statement.
  bool diverged = false;
  for (size_t i = 0; i < stmts.size(); ++i) {
    const bool is_last = i + 1 == stmts.size();
    const Stmt& stmt = ast_.Get(stmts[i]);

    if (diverged) {
      Warn(kUnreachableCode, "this code is never reached", stmt.span);
      diverged = false;  // One message per block, not one per statement.
    }

    // A block's value is its final expression statement, which is what makes
    // `let x = { ... }` and an expression-bodied arm work.
    if (is_last && stmt.kind == StmtKind::kExpr) {
      wait_allowed_ = tail_allows_wait;
      type = Check(stmt.expr, expected);
      continue;
    }
    CheckStmt(stmts[i]);

    if (!is_last && Diverges(stmts[i])) {
      diverged = true;
    }
  }

  PopScope();
  Record(id, type);
}

// ---------------------------------------------------------- declarations ----

// Whether a value of |type| has a C representation. The boundary is narrow
// on purpose: everything that crosses it has to mean the same thing on both
// sides, and a type whose layout this compiler decides unilaterally -- a
// slice, a string, an optional -- does not.
bool Checker::HasForeignRepresentation(TypeId type) const {
  // An object of the host's crosses as the address it already is.
  if (types_->IsForeignClass(type) && !types_->IsOptional(type)) {
    return true;
  }
  switch (types_->KindOf(type)) {
    case TypeKind::kBool:
    case TypeKind::kChar:
    case TypeKind::kInt:
    case TypeKind::kFloat:
    case TypeKind::kVoid:
    case TypeKind::kPointer:
    // A function as a value is an address, which is exactly what C takes
    // for a callback: `wolang_thread_spawn(body, argument)` is the shape
    // every threading API in C has (D97). The reference tier passes a
    // token instead and turns it back into the value, the same way a
    // signal's handler list already crosses.
    case TypeKind::kFunction:
    // An error type has already been reported; saying so twice is noise.
    case TypeKind::kError:
      return true;
    case TypeKind::kQuantity:
      // A dimension is erased before code generation, so `Length` crosses
      // as the `f32` it is.
      return HasForeignRepresentation(types_->ElementOf(type));
    default:
      return false;
  }
}

void Checker::CheckForeignType(TypeId type, SourceSpan span, bool is_result) {
  if (HasForeignRepresentation(type)) {
    return;
  }
  std::string message = "`";
  message.append(Describe(type));
  message.append(is_result ? "` cannot be returned from an `extern` function"
                           : "` cannot be passed to an `extern` function");
  Report(kNotForeignType, std::move(message), span,
         "the C boundary takes numbers, `bool` and pointers; pass anything "
         "else as a pointer");
}

void Checker::CheckFunction(const Item& item, uint32_t owner) {
  PushScope();

  const size_t pushed = PushGenerics(item, owner);
  // A method of an `extern class` with no body is the host's, like an
  // `extern` function; one an `extend` gives a body is Wolang's.
  const bool is_foreign = HasFlag(item.flags, ItemFlags::kExtern) || (self_type_.is_valid() && types_->IsForeignClass(self_type_) && !item.body.is_valid());

  if (is_foreign && item.body.is_valid()) {
    Report(kForeignWithBody,
           "an `extern` function is implemented outside the language, so it "
           "has no body here",
           item.name_span, "remove the body, or remove `extern`");
  }

  for (ParamId param_id : ast_.Get(item.params)) {
    const Param& param = ast_.Get(param_id);
    const TypeId type = param.type.is_valid()
                            ? resolver_.Resolve(ast_, param.type)
                            : types_->error();
    if (is_foreign) {
      CheckForeignType(type, param.span, /*is_result=*/false);
    }
    if (param.default_value.is_valid()) {
      const TypeId found = Check(param.default_value, type);
      if (!Accepts(type, found)) {
        ReportMismatch(type, found, ast_.Get(param.default_value).span);
      }
    }
    // A declaration with no body -- an ability method, an `extern` -- has
    // nowhere to use its parameters, so an unused one says nothing. An
    // invalid span is what opts a binding out of W0101.
    Bind(param.name, type, param.is_mutable,
         item.body.is_valid() ? param.span : SourceSpan());
  }

  const TypeId previous_return = return_type_;
  const bool previous_task = in_task_;
  in_task_ = HasFlag(item.flags, ItemFlags::kAsync);
  const bool previous_unsafe = caller_unsafe_;
  caller_unsafe_ = false;
  for (TypeRefId effect : ast_.Get(item.effects)) {
    const std::span<const Symbol> path = ast_.Get(ast_.Get(effect).path);
    caller_unsafe_ |= !path.empty() &&
                      interner_->Text(path.back()) == "unsafe";
  }
  const bool previous_deinit = in_deinit_;
  in_deinit_ = item.kind == ItemKind::kDeinit;
  return_type_ = item.type.is_valid() ? resolver_.Resolve(ast_, item.type)
                                      : types_->void_type();

  // Every type parameter has to appear in the signature.
  //
  // Not a style rule: a call says what `T` is by what it passes or by
  // what it is used as, and a parameter the signature never mentions
  // cannot be said at all -- `Has<Position>(world, entity)` on a
  // `fun Has<T>(world: World, entity: u64) -> bool` was accepted, bound
  // nothing, and lowered to a call with no body behind it (D100).
  if (!item.generics.empty()) {
    std::vector<TypeId> mentioned;
    for (ParamId param_id : ast_.Get(item.params)) {
      const Param& param = ast_.Get(param_id);
      if (param.type.is_valid()) {
        types_->CollectParameters(resolver_.Resolve(ast_, param.type),
                                  &mentioned);
      }
    }
    types_->CollectParameters(return_type_, &mentioned);
    for (GenericParamId generic_id : ast_.Get(item.generics)) {
      const GenericParam& generic = ast_.Get(generic_id);
      bool found = false;
      for (TypeId seen : mentioned) {
        found |= types_->KindOf(seen) == TypeKind::kParameter &&
                 types_->Get(seen).name == generic.name;
      }
      if (!found) {
        std::string message = "type parameter `";
        message.append(interner_->Text(generic.name));
        message.append("` appears nowhere in the signature of `");
        message.append(interner_->Text(item.name));
        message.append("`");
        Report(kUnboundParameter, std::move(message), generic.span,
               "a call says what a type parameter is by what it passes or "
               "by what the result is used as, so one that is mentioned "
               "nowhere can never be said");
      }
    }
  }
  if (is_foreign) {
    CheckForeignType(return_type_,
                     item.type.is_valid() ? ast_.Get(item.type).span
                                          : item.name_span,
                     /*is_result=*/true);
  }

  if (item.body.is_valid()) {
    if (HasFlag(item.flags, ItemFlags::kExpressionBody)) {
      // `fn f() -> T => expr` checks the expression against the return type,
      // with no `return` statement in sight.
      wait_allowed_ = true;
      const TypeId found = Check(item.body, return_type_);
      if (!Accepts(return_type_, found)) {
        ReportMismatch(return_type_, found, ast_.Get(item.body).span);
      }
    } else {
      // A function body is a statement position: a `wait` as its last
      // expression is as safe as one anywhere else in it.
      wait_allowed_ = true;
      Check(item.body, TypeId{});
    }
  }

  return_type_ = previous_return;
  in_task_ = previous_task;
  caller_unsafe_ = previous_unsafe;
  in_deinit_ = previous_deinit;
  resolver_.PopTypeParameters(pushed);
  PopScope();
}

size_t Checker::PushGenerics(const Item& item, uint32_t owner) {
  size_t pushed = 0;
  std::vector<Symbol> seen;
  for (GenericParamId generic_id : ast_.Get(item.generics)) {
    const GenericParam& generic = ast_.Get(generic_id);

    // `<T, T>` would make one of the two unreachable, and which one depends
    // on a shadowing rule nobody should have to know.
    bool duplicate = false;
    for (Symbol earlier : seen) {
      duplicate |= earlier == generic.name;
    }
    seen.push_back(generic.name);
    if (duplicate) {
      std::string message = "type parameter `";
      message.append(interner_->Text(generic.name));
      message.append("` is declared twice");
      Report(kDuplicateParameter, std::move(message), generic.span);
    }
    std::vector<TypeId> bounds;
    for (TypeRefId bound : ast_.Get(generic.bounds)) {
      bounds.push_back(resolver_.Resolve(ast_, bound));
    }
    resolver_.PushTypeParameter(
        generic.name,
        types_->Parameter(generic.name, owner,
                          static_cast<uint32_t>(pushed), bounds));
    ++pushed;
  }

  // Inside an ability its own `type Item` is the associated type of
  // whatever implements it, the same reading the signatures got (D212).
  if (item.kind == ItemKind::kAbility) {
    const Binding* binding = Lookup(item.name);
    const TypeId ability = binding != nullptr ? binding->type : TypeId{};
    for (ItemId member : ast_.Get(item.members)) {
      if (ability.is_valid() && ast_.Contains(member) && ast_.Get(member).kind == ItemKind::kTypeAlias) {
        resolver_.PushTypeParameter(ast_.Get(member).name, types_->Projection(ability, ast_.Get(member).name));
        ++pushed;
      }
    }
  }
  return pushed;
}

void Checker::CheckBinding(const Item& item) {
  const TypeId declared = item.type.is_valid()
                              ? resolver_.Resolve(ast_, item.type)
                              : TypeId{};

  // A declaration at module level has to say its type.
  //
  // Not a style rule: what a name means to *another* file is worked out
  // from signatures alone, and inferring this one from its initializer
  // would make a signature depend on a body -- which is the thing the
  // incremental design is built not to do (D88). Inside a function the
  // same `const x = 5` is fine, because nothing outside the function can
  // see it.
  if (!self_type_.is_valid() && !item.type.is_valid() &&
      item.init.is_valid() && at_module_level_) {
    std::string message = "`";
    message.append(interner_->Text(item.name));
    message.append("` needs its type written");
    Report(kModuleBindingNeedsType, std::move(message), item.name_span,
           "a declaration other files can see is a signature, and a "
           "signature cannot be read off a body");
  }

  // References are second class: a parameter or a local may hold one, a
  // field may not. That is what gives memory safety without a borrow
  // checker -- a reference can never outlive the frame it was made in.
  if (self_type_.is_valid() &&
      types_->KindOf(declared) == TypeKind::kReference) {
    std::string message = "a field cannot hold a reference; `";
    message.append(interner_->Text(item.name));
    message.append("` would outlive what it points at");
    Report(kReferenceInField, std::move(message), item.name_span,
           "store a `Box<T>`, an `Rc<T>` or the value itself");
  }
  if (!item.init.is_valid()) {
    return;
  }
  const TypeId found = Check(item.init, declared);
  if (declared.is_valid() && !Accepts(declared, found)) {
    ReportMismatch(declared, found, ast_.Get(item.init).span);
  }
}

// `override` and the method it replaces have to agree that one is replacing
// the other: silently shadowing a base method, or announcing an override of
// something that is not there, are the two halves of the same bug.
// A parameter carries its bounds, so once it is inferred the bound is
// checked against what it was inferred to be. Conformance is declared --
// `extend T as Ability` -- rather than inferred from the methods present.
// Keeps what a generic call was inferred to be, in the callee's parameter
// order. The order matters: it is what names the specialization, and two
// call sites that inferred the same arguments have to land on the same name
// or the module would carry a copy per call rather than per instantiation.
void Checker::RecordInstantiation(
    ExprId id,
    const TypeStore::Substitution& substitution) {
  if (!id.is_valid() || substitution.empty()) {
    return;
  }
  std::vector<std::pair<uint32_t, TypeId>> ordered;
  for (const auto& [parameter, inferred] : substitution) {
    if (!inferred.is_valid()) {
      continue;
    }
    // An argument that is itself a type parameter is kept rather than
    // dropped: it happens in a generic body calling another generic, and
    // the specialization that instantiates the outer one is what turns it
    // into something concrete.
    ordered.emplace_back(types_->Get(parameter).declaring_item, inferred);
  }
  if (ordered.size() != substitution.size()) {
    return;
  }
  std::sort(ordered.begin(), ordered.end(),
            [](const std::pair<uint32_t, TypeId>& lhs,
               const std::pair<uint32_t, TypeId>& rhs) {
              return lhs.first < rhs.first;
            });
  std::vector<TypeId> arguments;
  arguments.reserve(ordered.size());
  for (const auto& [index, type] : ordered) {
    arguments.push_back(type);
  }
  result_.instantiations.emplace_back(id, std::move(arguments));
}

void Checker::CheckBounds(const TypeStore::Substitution& substitution,
                          SourceSpan span) {
  for (const auto& [parameter, inferred] : substitution) {
    for (TypeId bound : types_->ArgumentsOf(parameter)) {
      if (!bound.is_valid() || types_->IsError(bound) ||
          resolution_->Satisfies(inferred, bound)) {
        continue;
      }
      std::string message = "`";
      message.append(Describe(inferred));
      message.append("` does not satisfy `");
      message.append(Describe(bound));
      message.append("`");
      Report(kBoundNotSatisfied, std::move(message), span);
    }
  }
}

void Checker::CheckOverride(const Item& item) {
  if (!self_type_.is_valid() || !item.name.is_valid()) {
    return;
  }
  const bool declared = HasFlag(item.flags, ItemFlags::kOverride);
  const Member* base =
      resolution_->FindInheritedMember(self_type_, item.name);
  const bool replaces = base != nullptr && base->kind == MemberKind::kMethod;

  if (declared && !replaces) {
    std::string message = "`";
    message.append(interner_->Text(item.name));
    message.append("` is marked `override` but replaces no base method");
    Report(kBadOverride, std::move(message), item.name_span);
    return;
  }
  if (!declared && replaces) {
    std::string message = "`";
    message.append(interner_->Text(item.name));
    message.append("` hides a base method; write `override`");
    Report(kBadOverride, std::move(message), item.name_span);
    return;
  }
  if (declared && replaces && !base->is_virtual && !base->is_override) {
    std::string message = "`";
    message.append(interner_->Text(item.name));
    message.append("` overrides a method that is not `virtual`");
    Report(kBadOverride, std::move(message), item.name_span);
  }
}

// A value type stores its fields inline, so one of them being the type
// itself -- directly or through other value types -- describes something of
// infinite size. A class is a reference to a heap object, so it may contain
// itself; so may a slice, a pointer or a reference, all of which are one
// indirection wide.
bool Checker::ReachesByValue(TypeId type, TypeId target, int depth) const {
  // The depth guard is load-bearing, not defensive: a cycle that does not
  // involve |target| -- `B` containing `B` while asking about `A` -- would
  // otherwise walk forever. `B` is reported on its own pass.
  if (!type.is_valid() || depth > 64) {
    return false;
  }
  if (type == target) {
    return true;
  }

  switch (types_->KindOf(type)) {
    case TypeKind::kOptional:
    case TypeKind::kFallible:
    case TypeKind::kArray:
    case TypeKind::kQuantity:
      // Still stored inline: `Node?` inside a struct is a Node plus a tag.
      return ReachesByValue(types_->ElementOf(type), target, depth + 1);

    case TypeKind::kTuple:
      for (TypeId argument : types_->ArgumentsOf(type)) {
        if (ReachesByValue(argument, target, depth + 1)) {
          return true;
        }
      }
      return false;

    case TypeKind::kInstance: {
      // `W<W<T>>` inside `W<T>` is the declaration again with a bigger
      // argument, so the bases are compared, not the instances (D225).
      const TypeId base = types_->Get(type).base;
      const TypeId target_base = types_->KindOf(target) == TypeKind::kInstance
                                     ? types_->Get(target).base
                                     : target;
      if (base == target_base) {
        return true;
      }
      if (!resolution_->IsValueType(base)) {
        return false;
      }
      // What the instance stores is the base's fields with the arguments
      // put in; a class or a slice in there ends the walk as before.
      const std::span<const TypeId> parameters =
          resolution_->ParametersOf(base);
      const std::vector<TypeId> arguments = types_->ArgumentsOf(type);
      TypeStore::Substitution substitution;
      for (size_t i = 0; i < parameters.size() && i < arguments.size(); ++i) {
        substitution.emplace_back(parameters[i], arguments[i]);
      }
      bool is_enum = false;
      for (const Member& member : resolution_->MembersOf(base)) {
        is_enum |= member.kind == MemberKind::kVariant;
        if (member.kind == MemberKind::kField &&
            ReachesByValue(types_->Substitute(member.type, substitution),
                           target, depth + 1)) {
          return true;
        }
      }
      // An enum's payloads are not in the member table, so what it holds is
      // approximated by what it was given.
      if (is_enum) {
        for (TypeId argument : arguments) {
          if (ReachesByValue(argument, target, depth + 1)) {
            return true;
          }
        }
      }
      return false;
    }

    case TypeKind::kNamed:
      if (!resolution_->IsValueType(type)) {
        return false;
      }
      for (const Member& member : resolution_->MembersOf(type)) {
        // Only fields. A variant's own type is its enum, so counting one
        // would make every enum look self-containing; what a variant
        // actually stores is its parameters, which the member table does not
        // carry yet.
        if (member.kind != MemberKind::kField) {
          continue;
        }
        if (ReachesByValue(member.type, target, depth + 1)) {
          return true;
        }
      }
      return false;

    default:
      // A slice, pointer, reference or function is one indirection wide.
      return false;
  }
}

// A class that derives from itself, directly or through a chain, has no base
// to resolve against. The lookups that walk the chain are already bounded, so
// this is about reporting the declaration rather than about terminating.
void Checker::CheckBaseChain(const Item& item, TypeId type) {
  if (!type.is_valid()) {
    return;
  }
  TypeId current = resolution_->BaseOf(type);
  for (size_t depth = 0; current.is_valid(); ++depth) {
    if (current == type) {
      std::string message = "`";
      message.append(Describe(type));
      message.append("` derives from itself");
      Report(kCyclicDeclaration, std::move(message), item.name_span);
      return;
    }
    if (depth > 64) {
      return;
    }
    current = resolution_->BaseOf(current);
  }
}

void Checker::CheckSized(const Item& item, TypeId type) {
  // A generic's `self` is an instance of its own parameters; the member
  // table belongs to the declaration (D225).
  if (type.is_valid() && types_->KindOf(type) == TypeKind::kInstance) {
    type = types_->Get(type).base;
  }
  if (!type.is_valid() || !resolution_->IsValueType(type)) {
    return;
  }
  for (const Member& member : resolution_->MembersOf(type)) {
    if (member.kind != MemberKind::kField) {
      continue;
    }
    if (!ReachesByValue(member.type, type, 0)) {
      continue;
    }
    std::string message = "`";
    message.append(Describe(type));
    message.append("` contains itself, so it has no size");
    Report(kInfiniteType, std::move(message), item.name_span,
           "store it behind a `Box<T>`, a slice or a reference");
    return;
  }
}

// `extend T as Ability` promises that T has every method the ability declares.
// The methods may come from T's own declaration or from any extension of it,
// which is why this works from the member table rather than from this block.
void Checker::CheckAbilityConformance(const Item& item, TypeId target) {
  if (!item.super_type.is_valid() || !target.is_valid()) {
    return;
  }
  const TypeId ability = resolver_.Resolve(ast_, item.super_type);
  if (types_->IsError(ability) || types_->IsError(target)) {
    return;
  }

  std::string missing;
  for (const Member& required : resolution_->MembersOf(ability)) {
    if (required.kind != MemberKind::kMethod && required.kind != MemberKind::kAssociatedType) {
      continue;
    }
    const Member* found = resolution_->FindMember(target, required.name);
    if (found != nullptr && found->kind == required.kind && (required.kind == MemberKind::kMethod || found->type.is_valid())) {
      continue;
    }
    if (!missing.empty()) {
      missing.append(", ");
    }
    missing.push_back('`');
    missing.append(interner_->Text(required.name));
    missing.push_back('`');
  }
  if (missing.empty()) {
    return;
  }

  std::string message = "`";
  message.append(Describe(target));
  message.append("` does not implement ");
  message.append(missing);
  message.append(" from `");
  message.append(Describe(ability));
  message.append("`");
  Report(kMissingAbilityMethod, std::move(message), item.name_span);
}

// `@allow(W0101, reason = "...")` silences one code for one declaration. The
// reason is mandatory: a suppression without one is how a codebase forgets
// why it stopped checking something.
size_t Checker::PushAllowed(const Item& item) {
  size_t pushed = 0;
  for (AttributeId id : ast_.Get(item.attributes)) {
    const Attribute& attribute = ast_.Get(id);
    if (interner_->Text(attribute.name) != "allow") {
      continue;
    }

    std::vector<std::string> codes;
    bool has_reason = false;
    for (ExprId arg : ast_.Get(attribute.args)) {
      if (!ast_.Contains(arg)) {
        continue;
      }
      const Expr& value = ast_.Get(arg);
      if (value.kind == ExprKind::kPath) {
        codes.emplace_back(interner_->Text(value.name));
        continue;
      }
      // `reason = "..."` parses as an assignment.
      if (value.kind == ExprKind::kAssign && ast_.Contains(value.lhs) &&
          ast_.Get(value.lhs).kind == ExprKind::kPath &&
          interner_->Text(ast_.Get(value.lhs).name) == "reason") {
        has_reason = ast_.Contains(value.rhs) &&
                     ast_.Get(value.rhs).kind == ExprKind::kStringLiteral;
      }
    }

    if (codes.empty()) {
      diagnostics_.Error(kBadAllow, "`@allow` needs a diagnostic code")
          .Primary(attribute.span, "write `@allow(W0101, reason = \"...\")`");
      continue;
    }
    if (!has_reason) {
      diagnostics_.Error(kBadAllow, "`@allow` needs a reason")
          .Primary(attribute.span,
                   "write `reason = \"...\"`, so the next reader knows why");
      // Suppressing anyway would hide the very code the author named while
      // they fix the attribute, so the codes are still honoured.
    }
    for (std::string& code : codes) {
      allowed_.push_back(std::move(code));
      ++pushed;
    }
  }
  return pushed;
}

void Checker::CheckItem(ItemId id) {
  if (!ast_.Contains(id)) {
    return;
  }
  const Item& item = ast_.Get(id);
  const size_t allowed = PushAllowed(item);
  const ScopedAllow restore(this, allowed);

  switch (item.kind) {
    case ItemKind::kFunction:
      // Only a method can override anything, and `self_type_` is what says
      // whether this is one.
      CheckOverride(item);
      CheckFunction(item, IndexOf(id));
      return;

    case ItemKind::kSystem:
    case ItemKind::kOn:
    // A destructor is a method in everything but how it is reached, so its
    // body is checked the same way -- with `self` bound, which is what
    // lets it read the fields it is tidying up (D87).
    case ItemKind::kDeinit:
      CheckFunction(item, IndexOf(id));
      return;

    case ItemKind::kTypeAlias: {
      for (uint32_t cyclic : resolution_->cyclic_aliases) {
        if (cyclic != IndexOf(id)) {
          continue;
        }
        std::string message = "`";
        message.append(interner_->Text(item.name));
        message.append("` is an alias for itself");
        Report(kCyclicDeclaration, std::move(message), item.name_span);
        break;
      }
      // `type Item` with nothing after it only means something inside an
      // ability, where an implementation answers it (D212).
      if (at_module_level_ && !item.type.is_valid()) {
        std::string message = "`type ";
        message.append(interner_->Text(item.name));
        message.append("` names nothing");
        Report(kTypeWithoutTarget, std::move(message), item.name_span, "write `type Name = SomeType`");
      }
      return;
    }

    case ItemKind::kBinding:
      CheckBinding(item);
      return;

    case ItemKind::kMachine:
      CheckMachine(item);
      return;

    case ItemKind::kClass:
    case ItemKind::kStruct:
    case ItemKind::kAbility:
    case ItemKind::kComponent:
    case ItemKind::kEnum:
    case ItemKind::kExtend: {
      // A generic type's parameters stay in scope for its members: the `T`
      // of `struct Holder<T>` is what the type of its `value` field names,
      // and what a method's signature names too.
      const size_t pushed = PushGenerics(item, IndexOf(id));

      // Inside a type's members, `self` is that type.
      const TypeId previous_self = self_type_;
      if (item.kind == ItemKind::kExtend) {
        // Resolved once: resolving the target again would report an
        // unknown type twice.
        self_type_ = item.target_type.is_valid()
                         ? resolver_.Resolve(ast_, item.target_type)
                         : TypeId{};
        CheckAbilityConformance(item, self_type_);
      } else {
        const Binding* binding = Lookup(item.name);
        self_type_ = binding != nullptr ? binding->type : TypeId{};
        // Inside `class List<T>`, `self` is a `List<T>` rather than a
        // `List`: the parameters are in scope, and a method that hands
        // `self` to something expecting the instance would otherwise be
        // handing over the declaration (D78).
        if (self_type_.is_valid()) {
          const std::span<const TypeId> parameters =
              resolution_->ParametersOf(self_type_);
          if (!parameters.empty()) {
            self_type_ = types_->Instance(
                self_type_,
                std::vector<TypeId>(parameters.begin(), parameters.end()));
          }
        }
        CheckBaseChain(item, self_type_);
        CheckSized(item, self_type_);
      }
      bool reported_unsized = false;
      for (ItemId member : ast_.Get(item.members)) {
        if (item.kind == ItemKind::kClass && HasFlag(item.flags, ItemFlags::kExtern) && ast_.Contains(member) && ast_.Get(member).kind != ItemKind::kFunction) {
          std::string message = "an `extern class` has methods and nothing else; `";
          message.append(interner_->Text(ast_.Get(member).name));
          message.append("` is something else");
          Report(kForeignClass, std::move(message), ast_.Get(member).name_span, "keep the data on the host's side and read it through a method");
        }
        {
          // A member of a type: its type comes from the declaration, and
          // the member table already carries it.
          const bool outer = at_module_level_;
          at_module_level_ = false;
          CheckItem(member);
          at_module_level_ = outer;
        }
        if (ast_.Contains(member) && HasFlag(ast_.Get(member).flags, ItemFlags::kWeak)) {
          const Item& field = ast_.Get(member);
          const TypeId held = field.type.is_valid() ? resolver_.Resolve(ast_, field.type) : TypeId{};
          const bool optional = held.is_valid() && types_->KindOf(held) == TypeKind::kOptional;
          const TypeId pointee = optional ? types_->ElementOf(held) : TypeId{};
          const TypeKind kind = pointee.is_valid() ? types_->KindOf(pointee) : TypeKind::kError;
          const bool counted = (kind == TypeKind::kNamed || kind == TypeKind::kInstance) && types_->IsHandle(pointee);
          if (item.kind != ItemKind::kClass || !counted) {
            std::string message = "`weak` is for a field of a class that holds a `C?` of a class; `";
            message.append(interner_->Text(field.name));
            message.append("` is not one");
            Report(kBadWeak, std::move(message), field.name_span, "write `weak var name: Node?` inside a `class`");
          }
        }
        // What a variant carries is resolved here as well, so that an
        // import used only by a payload counts as used: `Array(items:
        // List<Json>)` is the only mention of `List` in a module, and
        // reporting it as unused was wrong (D108).
        if (ast_.Contains(member) &&
            ast_.Get(member).kind == ItemKind::kVariant) {
          for (ParamId carried : ast_.Get(ast_.Get(member).params)) {
            if (ast_.Contains(carried) &&
                ast_.Get(carried).type.is_valid()) {
              const TypeId payload =
                  resolver_.Resolve(ast_, ast_.Get(carried).type);
              // A payload is stored inline like a field, so an enum that
              // carries itself has no size either (D225).
              if (!reported_unsized && item.kind == ItemKind::kEnum &&
                  self_type_.is_valid() &&
                  ReachesByValue(payload, self_type_, 0)) {
                reported_unsized = true;
                std::string message = "`";
                message.append(interner_->Text(item.name));
                message.append("` contains itself, so it has no size");
                Report(kInfiniteType, std::move(message), item.name_span,
                       "store it behind a `Box<T>`, a slice or a reference");
              }
            }
          }
        }
      }
      self_type_ = previous_self;
      resolver_.PopTypeParameters(pushed);
      return;
    }

    case ItemKind::kUnit:
      // Resolved while building the file's scope, where the dimensions are.
      return;

    default:
      return;
  }
}

TypedBody Checker::Run(ItemId item, uint32_t item_index) {
  result_.item_index = item_index;
  PushScope();
  at_module_level_ = true;
  CheckItem(item);
  PopScope();
  SettleThreadClosures();
  result_.diagnostics = diagnostics_.diagnostics();
  return std::move(result_);
}

}  // namespace

TypedBody TypeCheckItem(TypeStore* types,
                        const StringInterner* interner,
                        const FileResolution* resolution,
                        const Ast& ast,
                        ItemId item,
                        uint32_t item_index,
                        const ItemIndexMap* item_indices) {
  Checker checker(types, interner, resolution, ast, item_indices);
  TypedBody body = checker.Run(item, item_index);

  // Definite assignment and moves are a separate walk over the same tree:
  // both are about paths rather than about types, and keeping them apart is
  // what stops the type checker from growing a second analysis inside it.
  DiagnosticEngine flow;
  CheckFlow(ast, *interner, *types, *resolution, body, item, &flow);
  for (const Diagnostic& diagnostic : flow.diagnostics()) {
    body.diagnostics.push_back(diagnostic);
  }
  return body;
}

}  // namespace wolang
