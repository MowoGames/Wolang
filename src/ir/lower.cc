// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/ir/lower.h"

#include <algorithm>
#include <utility>

#include "wolang/base/diagnostic.h"
#include "wolang/ir/builder.h"

namespace wolang {
namespace ir {
namespace {

// What a name means while a body is being lowered: either a slot holding a
// mutable binding, or a value directly.
struct Local {
  Symbol name;
  // Valid for a `var`: the alloca holding it.
  InstId slot;
  // Valid for a `let` and for a parameter: the value itself.
  InstId value;
  TypeId type;
};

// Where `break` and `continue` go, innermost last.
//
// `break_to` is created on demand. A loop nothing breaks out of has no exit
// block at all, which is what keeps lowering from emitting one the verifier
// would rightly call unreachable.
struct LoopTarget {
  Symbol label;
  BlockId break_to;
  BlockId continue_to;
  // How deep the scope stack was when the loop started, so a `break` knows
  // which deferred statements it is leaving behind.
  size_t scope = 0;
};

class Lowerer {
 public:
  Lowerer(StringInterner* interner,
          TypeStore* types,
          const FileResolution& resolution,
          const Ast& ast,
          const TypedBody& typed,
          std::span<const std::pair<ItemId, EffectRow>> effects,
          Module* module)
      : interner_(interner),
        types_(types),
        resolution_(resolution),
        ast_(ast),
        typed_(typed),
        effects_(effects),
        module_(module),
        resolver_(types, interner, &resolution, &discarded_) {}

  void Run() {
    // Two passes: every function gets a declaration first, so a call can
    // name one that is defined later in the file.
    for (ItemId id : ast_.Get(ast_.root())) {
      Declare(id);
    }
    for (ItemId id : ast_.Get(ast_.root())) {
      Define(id);
    }

    // A generic function has no code of its own; it gets one copy per
    // instantiation. Lowering a specialization can discover more of them --
    // a generic body calling another generic -- so this is a worklist
    // rather than a second pass.
    while (!pending_.empty()) {
      const Pending work = pending_.back();
      pending_.pop_back();
      substitution_ = work.substitution;
      if (ast_.Contains(work.item)) {
        LowerFunction(ast_.Get(work.item), work.index);
      }
      substitution_.clear();
    }
  }

 private:
  // ------------------------------------------------------- declarations --

  void Declare(ItemId id) {
    if (!ast_.Contains(id)) {
      return;
    }
    const Item& item = ast_.Get(id);
    switch (item.kind) {
      case ItemKind::kFunction: {
        const Binding* binding = resolution_.Find(item.name);
        const TypeId signature =
            binding != nullptr ? binding->type : TypeId{};
        // A generic declaration is not a function: it is a recipe for one.
        // Emitting it would put `T` in the IR, which every back end would
        // then need an answer for -- and the verifier rightly rejects a
        // call that passes an `i32` where a `T` is declared.
        if (signature.is_valid() && types_->MentionsParameter(signature)) {
          generics_.emplace_back(item.name, id);
          break;
        }
        Function function;
        function.name = item.name;
        function.type = Erase(signature);
        function.effects = EffectsOf(id, &function.effects_known);
        if (binding != nullptr) {
          function.source_file = binding->file;
          function.source_item = binding->item;
        }
        function.is_task = HasFlag(item.flags, ItemFlags::kAsync);
        if (HasFlag(item.flags, ItemFlags::kExtern)) {
          // `extern "C" fun puts(...) = "puts"` names the symbol; without
          // the clause the symbol is the declaration's own name.
          function.foreign_symbol =
              item.link_name.is_valid() ? item.link_name : item.name;
        }
        indices_.emplace_back(id, module_->functions.size());
        module_->functions.push_back(std::move(function));
        break;
      }
      case ItemKind::kClass:
      case ItemKind::kStruct:
      case ItemKind::kComponent:
      case ItemKind::kEnum:
      case ItemKind::kExtend:
        // Methods are functions too; their bodies are lowered the same way.
        for (ItemId member : ast_.Get(item.members)) {
          Declare(member);
        }
        break;
      default:
        break;
    }
  }

  // What a declaration does, as the module-wide inference found it. An
  // absence is not an empty row: see `Function::effects_known`.
  EffectRow EffectsOf(ItemId id, bool* known) const {
    for (const auto& [item, row] : effects_) {
      if (item == id) {
        *known = true;
        return row;
      }
    }
    *known = false;
    return EffectRow();
  }

  // The substitution in force, applied. Inside a specialization every `T`
  // the checker recorded stands for something concrete.
  TypeId Apply(TypeId type) {
    if (substitution_.empty() || !type.is_valid()) {
      return type;
    }
    return types_->Substitute(type, substitution_);
  }

  ItemId FindGeneric(Symbol name) const {
    for (const auto& [generic, id] : generics_) {
      if (generic == name) {
        return id;
      }
    }
    return ItemId{};
  }

  const std::vector<TypeId>* FindInstantiation(ExprId call) const {
    for (const auto& [id, arguments] : typed_.instantiations) {
      if (id == call) {
        return &arguments;
      }
    }
    return nullptr;
  }

  // The type parameters a signature mentions, in declaration order. The
  // order is what matches them to the arguments inference recorded.
  std::vector<TypeId> ParametersIn(TypeId type) const {
    std::vector<std::pair<uint32_t, TypeId>> found;
    CollectParameters(type, &found);
    std::sort(found.begin(), found.end(),
              [](const std::pair<uint32_t, TypeId>& lhs,
                 const std::pair<uint32_t, TypeId>& rhs) {
                return lhs.first < rhs.first;
              });
    std::vector<TypeId> parameters;
    for (const auto& [index, parameter] : found) {
      if (parameters.empty() || parameters.back() != parameter) {
        parameters.push_back(parameter);
      }
    }
    return parameters;
  }

  void CollectParameters(
      TypeId type,
      std::vector<std::pair<uint32_t, TypeId>>* found) const {
    if (!type.is_valid()) {
      return;
    }
    if (types_->KindOf(type) == TypeKind::kParameter) {
      found->emplace_back(types_->Get(type).declaring_item, type);
      return;
    }
    const TypeId element = types_->ElementOf(type);
    if (element.is_valid() && element != type) {
      CollectParameters(element, found);
    }
    for (TypeId argument : types_->ArgumentsOf(type)) {
      if (argument != type) {
        CollectParameters(argument, found);
      }
    }
  }

  // One specialization per instantiation, named after what it was
  // instantiated with: two call sites that inferred the same arguments
  // reach the same copy.
  uint32_t Instantiate(ItemId item, ExprId call) {
    const std::vector<TypeId>* arguments = FindInstantiation(call);
    const Binding* binding = resolution_.Find(ast_.Get(item).name);
    if (arguments == nullptr || binding == nullptr) {
      return 0xFFFFFFFFu;
    }
    const std::vector<TypeId> parameters = ParametersIn(binding->type);
    if (parameters.size() != arguments->size()) {
      return 0xFFFFFFFFu;
    }

    TypeStore::Substitution substitution;
    std::string suffix = "<";
    for (size_t i = 0; i < parameters.size(); ++i) {
      // Mapped through the substitution in force: a generic body calling
      // another generic passes its own parameters along, and by the time
      // this copy exists they stand for something.
      const TypeId concrete = Erase(Apply((*arguments)[i]));
      if (!concrete.is_valid() || types_->MentionsParameter(concrete)) {
        return 0xFFFFFFFFu;
      }
      substitution.emplace_back(parameters[i], concrete);
      if (i != 0) {
        suffix.append(", ");
      }
      suffix.append(types_->Describe(*interner_, concrete));
    }
    suffix.push_back('>');

    std::string mangled(interner_->Text(ast_.Get(item).name));
    mangled.append(suffix);
    const Symbol name = interner_->Intern(mangled);
    for (const auto& [existing, index] : specializations_) {
      if (existing == name) {
        return index;
      }
    }

    Function function;
    function.name = name;
    function.type = Erase(types_->Substitute(binding->type, substitution));
    function.effects = EffectsOf(item, &function.effects_known);
    function.source_file = binding->file;
    function.source_item = binding->item;
    function.is_task = HasFlag(ast_.Get(item).flags, ItemFlags::kAsync);
    const auto index = static_cast<uint32_t>(module_->functions.size());
    module_->functions.push_back(std::move(function));
    specializations_.emplace_back(name, index);
    pending_.push_back(Pending{item, substitution, index});
    return index;
  }

  uint32_t IndexOf(ItemId id) const {
    for (const auto& [item, index] : indices_) {
      if (item == id) {
        return static_cast<uint32_t>(index);
      }
    }
    return 0xFFFFFFFFu;
  }

  // A call names a function by its binding, so the binding has to be mapped
  // back to whichever declaration it came from.
  // Adds a signature-only entry for a function this file calls but does not
  // define, reusing one if it is already there.
  uint32_t DeclareExternal(Symbol name,
                           TypeId type,
                           uint32_t file = Function::kNoSource,
                           uint32_t item = Function::kNoSource) {
    for (uint32_t i = 0; i < module_->functions.size(); ++i) {
      if (module_->functions[i].name == name) {
        return i;
      }
    }
    Function declaration;
    declaration.name = name;
    declaration.type = Erase(type);
    // Where the definition is, so that something linking the module's
    // files together knows which one this names. An intrinsic the compiler
    // synthesized has no declaration to point at.
    declaration.source_file = file;
    declaration.source_item = item;
    module_->functions.push_back(std::move(declaration));
    return static_cast<uint32_t>(module_->functions.size() - 1);
  }

  uint32_t IndexOfName(Symbol name) const {
    for (const auto& [item, index] : indices_) {
      if (ast_.Contains(item) && ast_.Get(item).name == name) {
        return static_cast<uint32_t>(index);
      }
    }
    return 0xFFFFFFFFu;
  }

  void Define(ItemId id) {
    if (!ast_.Contains(id)) {
      return;
    }
    const Item& item = ast_.Get(id);
    switch (item.kind) {
      case ItemKind::kFunction:
        LowerFunction(item, IndexOf(id));
        break;
      case ItemKind::kClass:
      case ItemKind::kStruct:
      case ItemKind::kComponent:
      case ItemKind::kEnum:
      case ItemKind::kExtend:
        for (ItemId member : ast_.Get(item.members)) {
          Define(member);
        }
        break;
      default:
        break;
    }
  }

  void LowerFunction(const Item& item, uint32_t index) {
    if (index >= module_->functions.size() || !item.body.is_valid()) {
      // A declaration with no body -- a trait method, an `extern` -- has
      // nothing to lower. It keeps its signature so a call can still name
      // it.
      return;
    }

    Builder builder(module_, FunctionId{index});
    builder.types = types_;
    current_function_ = index;
    builder_ = &builder;
    locals_.clear();
    loops_.clear();
    deferred_.clear();
    scope_depth_ = 0;

    const BlockId entry = builder.AddBlock("entry");
    builder.SetCurrent(entry);

    in_task_ = module_->functions[index].is_task;

    for (ParamId param_id : ast_.Get(item.params)) {
      const Param& param = ast_.Get(param_id);
      const TypeId type =
          ParamTypeOf(module_->functions[index], locals_.size());
      const InstId value = builder.AddParam(entry, type);
      if (in_task_) {
        // A parameter outlives every suspension, so in a task it lives in
        // the frame like everything else.
        const InstId slot = builder.Add(Opcode::kAlloca, type);
        builder.Add(Opcode::kStore, TypeId{}, {slot, value});
        locals_.push_back(Local{param.name, slot, InstId{}, type});
        continue;
      }
      locals_.push_back(Local{param.name, InstId{}, value, type});
    }

    const TypeId signature = module_->functions[index].type;
    return_type_ = types_->KindOf(signature) == TypeKind::kFunction
                       ? types_->ElementOf(signature)
                       : types_->void_type();

    if (HasFlag(item.flags, ItemFlags::kExpressionBody)) {
      const InstId value = Value(item.body);
      if (types_->KindOf(return_type_) == TypeKind::kVoid) {
        builder.ReturnVoid();
      } else {
        builder.Return(value);
      }
    } else {
      Lower(item.body);
      // A body that falls off the end returns nothing; the checker already
      // refused one that owed a value.
      if (!builder.Terminated()) {
        builder.ReturnVoid();
      }
    }

    builder_ = nullptr;
  }

  TypeId ParamTypeOf(const Function& function, size_t position) const {
    const std::vector<TypeId> params = types_->ArgumentsOf(function.type);
    return position < params.size() ? params[position] : types_->error();
  }

  // ------------------------------------------------------------- lookups --

  // `Accepts(T?, T)` holds in the checker -- a value fits an optional of
  // itself -- so wherever a value flows into an optional position the IR
  // needs the wrap the source did not write.
  InstId Coerce(InstId value, TypeId from, TypeId to) {
    if (!value.is_valid() || !to.is_valid()) {
      return value;
    }
    const TypeId target = Erase(to);
    if (types_->KindOf(target) != TypeKind::kOptional) {
      return value;
    }
    if (types_->KindOf(Erase(from)) == TypeKind::kOptional) {
      return value;
    }
    return builder_->Add(Opcode::kOptionalWrap, target, {value});
  }

  // The type a value is built at, when the context asked for an optional of
  // it.
  TypeId Unwrapped(TypeId type) {
    const TypeId erased = Erase(type);
    return types_->KindOf(erased) == TypeKind::kOptional
               ? Erase(types_->ElementOf(erased))
               : erased;
  }

  const Local* Find(Symbol name) const {
    for (auto it = locals_.rbegin(); it != locals_.rend(); ++it) {
      if (it->name == name) {
        return &*it;
      }
    }
    return nullptr;
  }

  TypeId TypeOf(ExprId id) {
    const TypeId recorded = typed_.TypeOf(id);
    return Erase(Apply(recorded.is_valid() ? recorded : types_->error()));
  }

  // Dimensions participate in type checking and are gone by code
  // generation, so `Length` and `Length/Time` are both `f32` here. Erasing
  // at the boundary rather than in every back end is what makes "unit safe
  // code costs nothing at runtime" true rather than aspirational.
  TypeId Erase(TypeId type) {
    if (!type.is_valid()) {
      return type;
    }
    switch (types_->KindOf(type)) {
      case TypeKind::kQuantity:
        return Erase(types_->ElementOf(type));

      // An open literal is a checking-time notion exactly like a dimension:
      // the IR has no "integer of whatever width the context wants". One
      // that reached here had no expectation to narrow it, which is the same
      // situation the checker settles with the default type.
      case TypeKind::kIntLiteral:
        return types_->default_int();
      case TypeKind::kFloatLiteral:
        return types_->default_float();
      case TypeKind::kOptional:
        return types_->Optional(Erase(types_->ElementOf(type)));
      case TypeKind::kSlice:
        return types_->Slice(Erase(types_->ElementOf(type)),
                             types_->Get(type).is_soa);
      case TypeKind::kArray:
        return types_->Array(Erase(types_->ElementOf(type)),
                             types_->Get(type).length,
                             types_->Get(type).is_soa);
      case TypeKind::kTuple: {
        std::vector<TypeId> elements;
        for (TypeId element : types_->ArgumentsOf(type)) {
          elements.push_back(Erase(element));
        }
        return types_->Tuple(elements);
      }
      case TypeKind::kFunction: {
        std::vector<TypeId> params;
        for (TypeId param : types_->ArgumentsOf(type)) {
          params.push_back(Erase(param));
        }
        return types_->Function(params, Erase(types_->ElementOf(type)));
      }
      default:
        return type;
    }
  }

  // ---------------------------------------------------------- statements --

  void Lower(ExprId id) {
    if (!ast_.Contains(id) || builder_->Terminated()) {
      return;
    }
    const Expr& expr = ast_.Get(id);
    if (expr.kind == ExprKind::kBlock) {
      const size_t depth = locals_.size();
      ++scope_depth_;
      for (StmtId stmt : ast_.Get(expr.stmts)) {
        LowerStmt(stmt);
      }
      RunDeferred(scope_depth_);
      ForgetDeferred(scope_depth_);
      --scope_depth_;
      locals_.resize(depth);
      return;
    }
    Value(id);
  }

  void LowerStmt(StmtId id) {
    if (!ast_.Contains(id) || builder_->Terminated()) {
      return;
    }
    const Stmt& stmt = ast_.Get(id);
    switch (stmt.kind) {
      case StmtKind::kLet:
        LowerLet(stmt, /*mutable_binding=*/false);
        return;

      case StmtKind::kVar:
        LowerLet(stmt, /*mutable_binding=*/true);
        return;

      case StmtKind::kExpr:
        Value(stmt.expr);
        return;

      case StmtKind::kDefer:
        if (stmt.expr.is_valid()) {
          deferred_.push_back(Deferred{scope_depth_, stmt.expr});
        }
        return;

      case StmtKind::kReturn: {
        // The value is computed before the deferred statements run, because
        // a `defer` that touches what is being returned must not change the
        // answer.
        const InstId value =
            stmt.expr.is_valid()
                ? Coerce(Value(stmt.expr), TypeOf(stmt.expr), return_type_)
                : InstId{};
        RunDeferred(0);
        if (stmt.expr.is_valid()) {
          builder_->Return(value);
        } else {
          builder_->ReturnVoid();
        }
        return;
      }

      case StmtKind::kBreak:
      case StmtKind::kContinue: {
        LoopTarget* target = FindLoop(stmt.label);
        if (target == nullptr) {
          // The checker already reported a `break` with no loop.
          builder_->Unreachable();
          return;
        }
        RunDeferred(target->scope);
        if (stmt.kind == StmtKind::kContinue) {
          builder_->Jump(target->continue_to);
          return;
        }
        if (!target->break_to.is_valid()) {
          target->break_to = builder_->AddBlock("loop.exit");
        }
        builder_->Jump(target->break_to);
        return;
      }

      default:
        if (stmt.expr.is_valid()) {
          Value(stmt.expr);
        }
        return;
    }
  }

  void LowerLet(const Stmt& stmt, bool mutable_binding) {
    const Pattern* pattern = ast_.Contains(stmt.pattern)
                                 ? &ast_.Get(stmt.pattern)
                                 : nullptr;
    if (pattern == nullptr || pattern->kind != PatternKind::kBinding) {
      // Destructuring waits for the pass that knows field offsets; the
      // initializer still has to run for its effects.
      if (stmt.expr.is_valid()) {
        Value(stmt.expr);
      }
      return;
    }

    // A `var x: int` with no initializer has no expression to read a type
    // from, so the annotation is the only place it comes from. Before this
    // the slot was typed `<error>`, which nothing noticed until slot
    // promotion put the value on an edge and the verifier compared it.
    TypeId type = stmt.expr.is_valid() ? TypeOf(stmt.expr) : TypeId{};
    if ((!type.is_valid() || types_->IsError(type)) && stmt.type.is_valid()) {
      type = Erase(Apply(resolver_.Resolve(ast_, stmt.type)));
    }
    if (!type.is_valid()) {
      type = types_->error();
    }
    const InstId value = stmt.expr.is_valid() ? Value(stmt.expr) : InstId{};

    // Inside a `task`, even a `const` goes in a slot. The transform that
    // splits a task into a state machine moves slots into the frame and
    // leaves values alone, so a value that outlived a suspension would be
    // one the machine has nowhere to keep. Making every binding a slot is
    // what makes that impossible rather than merely unlikely.
    if (!mutable_binding && !in_task_ &&
        !HasFlag(pattern->flags, PatternFlags::kMutable)) {
      locals_.push_back(Local{pattern->name, InstId{}, value, type});
      return;
    }

    // A `var` goes in a slot. Lowering every one of them and letting SROA
    // promote the ones that can be is simpler than deciding here.
    const InstId slot = builder_->Add(Opcode::kAlloca, type);
    if (value.is_valid()) {
      builder_->Add(Opcode::kStore, TypeId{}, {slot, value});
    }
    locals_.push_back(Local{pattern->name, slot, InstId{}, type});
  }

  // Everything a scope has to run on the way out, innermost last. A `defer`
  // is the one statement whose position in the source is not where it runs,
  // so lowering is where it becomes ordinary code -- emitted at every exit
  // from the scope that deferred it, which includes the ones a `return` or
  // a `break` take.
  struct Deferred {
    size_t scope = 0;
    ExprId expr;
  };

  // Runs everything deferred at or below |scope|, innermost first, without
  // forgetting it: an early exit leaves the list alone for the paths that
  // have not run yet.
  void RunDeferred(size_t scope) {
    for (size_t i = deferred_.size(); i > 0; --i) {
      if (deferred_[i - 1].scope < scope) {
        break;
      }
      if (builder_->Terminated()) {
        return;
      }
      Value(deferred_[i - 1].expr);
    }
  }

  // Drops the entries a scope owned, after its exit has been emitted.
  void ForgetDeferred(size_t scope) {
    while (!deferred_.empty() && deferred_.back().scope >= scope) {
      deferred_.pop_back();
    }
  }

  LoopTarget* FindLoop(Symbol label) {
    for (auto it = loops_.rbegin(); it != loops_.rend(); ++it) {
      if (!label.is_valid() || it->label == label) {
        return &*it;
      }
    }
    return nullptr;
  }

  // --------------------------------------------------------- expressions --

  InstId Value(ExprId id) {
    if (!ast_.Contains(id) || builder_->Terminated()) {
      return InstId{};
    }
    const Expr& expr = ast_.Get(id);
    const TypeId type = TypeOf(id);

    switch (expr.kind) {
      case ExprKind::kIntLiteral: {
        const NumberLiteral& number = ast_.number(expr.aux);
        const TypeId built = Unwrapped(type);
        return Coerce(builder_->ConstInt(built, number.integer_value),
                      built, type);
      }
      case ExprKind::kFloatLiteral: {
        const NumberLiteral& number = ast_.number(expr.aux);
        const TypeId built = Unwrapped(type);
        return Coerce(builder_->ConstFloat(built, number.float_value),
                      built, type);
      }
      case ExprKind::kBoolLiteral:
        return Coerce(builder_->ConstBool(expr.aux != 0),
                      types_->bool_type(), type);
      case ExprKind::kNullLiteral:
        return builder_->ConstNull(type);
      case ExprKind::kStringLiteral:
        return LowerString(expr, type);

      case ExprKind::kPath:
        return LowerPath(expr, type);

      case ExprKind::kCast: {
        // `as` is the only numeric conversion there is (D33), so it has to
        // reach the IR rather than being read off the recorded type: the
        // operand's type and the result's differ, and that difference is
        // the whole instruction.
        const InstId operand = Value(expr.lhs);
        const TypeId built = Unwrapped(type);
        return Coerce(builder_->Add(Opcode::kCast, built, {operand}), built,
                      type);
      }

      case ExprKind::kUnary:
        return LowerUnary(expr, type);
      case ExprKind::kBinary:
        return LowerBinary(id, expr, type);
      case ExprKind::kAssign:
        return LowerAssign(expr);

      case ExprKind::kArrayLiteral: {
        std::vector<InstId> elements;
        for (ExprId element : ast_.Get(expr.args)) {
          elements.push_back(Value(element));
        }
        return builder_->Add(Opcode::kMakeArray, type, std::move(elements));
      }

      case ExprKind::kIndex: {
        const InstId array = Value(expr.lhs);
        const InstId index = Value(expr.rhs);
        return builder_->Add(Opcode::kIndex, type, {array, index});
      }

      case ExprKind::kField:
        return LowerField(expr, type);
      case ExprKind::kOptionalField:
        return LowerOptionalField(expr, type);

      case ExprKind::kCall:
        return LowerCall(id, expr, type);

      case ExprKind::kAwait:
        // `wait f(args)` is a call that suspends. The call itself is
        // lowered as usual and marked; the transform in `ir/tasks.h` turns
        // the mark into a state machine.
        return LowerWait(expr, type);

      case ExprKind::kBlock:
        return LowerBlockValue(id, expr, type);

      case ExprKind::kIf:
        return LowerIf(expr, type);

      case ExprKind::kMatch:
        return LowerMatch(expr, type);

      case ExprKind::kLoop:
      case ExprKind::kWhile:
        LowerLoop(expr);
        return InstId{};

      case ExprKind::kUnsafe:
      case ExprKind::kRegion:
        // Neither changes the value; both are checked, not lowered.
        return Value(expr.lhs);

      default:
        // Anything the lowering does not cover yet still has to produce a
        // typed value, so that one gap does not cascade into a broken
        // function.
        return builder_->Undef(type);
    }
  }

  InstId LowerString(const Expr& expr, TypeId type) {
    const std::span<const ExprId> parts = ast_.Get(expr.args);
    if (parts.size() == 1 && ast_.Contains(parts[0]) &&
        ast_.Get(parts[0]).kind == ExprKind::kStringText) {
      const Expr& text = ast_.Get(parts[0]);
      return builder_->ConstString(type,
                                   ast_.string(text.aux));
    }
    // Interpolation builds a string at runtime, by turning each piece into
    // text and concatenating. The conversions are intrinsics -- the one
    // thing here the language cannot say about itself.
    const TypeId str = types_->str();
    InstId result;
    for (ExprId part : parts) {
      if (!ast_.Contains(part)) {
        continue;
      }
      const Expr& piece = ast_.Get(part);
      InstId text;
      if (piece.kind == ExprKind::kStringText) {
        text = builder_->ConstString(str, ast_.string(piece.aux));
      } else {
        text = Stringify(Value(part), TypeOf(part));
      }
      if (!text.is_valid()) {
        continue;
      }
      result = result.is_valid() ? Intrinsic("ConcatStr", str,
                                             {result, text})
                                 : text;
    }
    return result.is_valid() ? result
                             : builder_->ConstString(str, std::string());
  }

  // Calls a host-provided function by name, declaring it if this file has
  // not named it yet.
  InstId Intrinsic(std::string_view name,
                   TypeId result,
                   std::vector<InstId> arguments) {
    const Symbol symbol = interner_->Intern(name);
    std::vector<TypeId> params;
    for (InstId argument : arguments) {
      params.push_back(
          module_->functions[current_function_].Contains(argument)
              ? module_->functions[current_function_].Get(argument).type
              : types_->error());
    }
    const uint32_t callee =
        DeclareExternal(symbol, types_->Function(params, result));
    return builder_->Call(callee, result, std::move(arguments));
  }

  // Turns a value into text, picking the conversion by its type.
  InstId Stringify(InstId value, TypeId type) {
    if (!value.is_valid()) {
      return value;
    }
    const TypeId str = types_->str();
    switch (types_->KindOf(Erase(type))) {
      case TypeKind::kStr:
      case TypeKind::kString:
        return value;
      case TypeKind::kInt:
        return Intrinsic("IntToStr", str, {value});
      case TypeKind::kFloat:
        return Intrinsic("FloatToStr", str, {value});
      case TypeKind::kBool:
        return Intrinsic("BoolToStr", str, {value});
      default:
        // Anything else needs a formatting trait, which arrives with the
        // rest of `core`.
        return builder_->ConstString(str, "<?>");
    }
  }

  InstId LowerPath(const Expr& expr, TypeId type) {
    const Local* local = Find(expr.name);
    if (local == nullptr) {
      return builder_->Undef(type);
    }
    if (local->slot.is_valid()) {
      return builder_->Add(Opcode::kLoad, local->type, {local->slot});
    }
    return local->value;
  }

  InstId LowerUnary(const Expr& expr, TypeId type) {
    const InstId operand = Value(expr.lhs);
    switch (expr.op) {
      case TokenKind::kMinus:
        return builder_->Add(Opcode::kNeg, type, {operand});
      case TokenKind::kBang:
        return builder_->Add(Opcode::kLogicalNot, types_->bool_type(),
                             {operand});
      case TokenKind::kTilde:
        return builder_->Add(Opcode::kBitNot, type, {operand});
      case TokenKind::kStar:
        // Reading through a pointer is the same load as reading a slot: the
        // operand is an address either way.
        return builder_->Add(Opcode::kLoad, type, {operand});
      case TokenKind::kAmp:
        return Address(expr.lhs, operand);
      default:
        return operand;
    }
  }

  InstId LowerBinary(ExprId id, const Expr& expr, TypeId type) {
    // `&&` and `||` do not evaluate their right side unless they have to, so
    // they are control flow rather than operations.
    if (expr.op == TokenKind::kAmpAmp || expr.op == TokenKind::kPipePipe) {
      return LowerShortCircuit(expr);
    }
    // `a ?? b` does not evaluate `b` unless `a` is absent, so it is control
    // flow too.
    if (expr.op == TokenKind::kQuestionQuestion) {
      return LowerCoalesce(expr, type);
    }

    const InstId left = Value(expr.lhs);
    const InstId right = Value(expr.rhs);
    const Opcode opcode = BinaryOpcode(expr.op);
    if (opcode == Opcode::kUndef) {
      return builder_->Undef(type);
    }
    const bool compare = opcode >= Opcode::kEq && opcode <= Opcode::kGe;
    return builder_->Add(opcode, compare ? types_->bool_type() : type,
                         {left, right});
  }

  static Opcode BinaryOpcode(TokenKind op) {
    switch (op) {
      case TokenKind::kPlus:
        return Opcode::kAdd;
      case TokenKind::kMinus:
        return Opcode::kSub;
      case TokenKind::kStar:
        return Opcode::kMul;
      case TokenKind::kSlash:
        return Opcode::kDiv;
      case TokenKind::kPercent:
        return Opcode::kRem;
      case TokenKind::kAmp:
        return Opcode::kBitAnd;
      case TokenKind::kPipe:
        return Opcode::kBitOr;
      case TokenKind::kCaret:
        return Opcode::kBitXor;
      case TokenKind::kShl:
        return Opcode::kShl;
      case TokenKind::kShr:
        return Opcode::kShr;
      case TokenKind::kEqEq:
        return Opcode::kEq;
      case TokenKind::kBangEq:
        return Opcode::kNe;
      case TokenKind::kLess:
        return Opcode::kLt;
      case TokenKind::kLessEq:
        return Opcode::kLe;
      case TokenKind::kGreater:
        return Opcode::kGt;
      case TokenKind::kGreaterEq:
        return Opcode::kGe;
      default:
        return Opcode::kUndef;
    }
  }

  InstId LowerShortCircuit(const Expr& expr) {
    const bool is_and = expr.op == TokenKind::kAmpAmp;
    const InstId left = Value(expr.lhs);

    const BlockId rest = builder_->AddBlock(is_and ? "and.rhs" : "or.rhs");
    const BlockId join = builder_->AddBlock(is_and ? "and.join" : "or.join");
    const InstId result = builder_->AddParam(join, types_->bool_type());

    const BlockId shortcut = builder_->AddBlock(
        is_and ? "and.false" : "or.true");
    if (is_and) {
      builder_->Branch(left, rest, shortcut);
    } else {
      builder_->Branch(left, shortcut, rest);
    }

    builder_->SetCurrent(shortcut);
    const InstId constant = builder_->ConstBool(!is_and);
    builder_->Jump(join, {constant});

    builder_->SetCurrent(rest);
    const InstId right = Value(expr.rhs);
    builder_->Jump(join, {right});

    builder_->SetCurrent(join);
    return result;
  }

  InstId LowerCoalesce(const Expr& expr, TypeId type) {
    const InstId optional = Value(expr.lhs);
    const InstId present = builder_->Add(
        Opcode::kOptionalIsPresent, types_->bool_type(), {optional});

    const BlockId have = builder_->AddBlock("coalesce.have");
    const BlockId fallback = builder_->AddBlock("coalesce.else");
    const BlockId join = builder_->AddBlock("coalesce.join");
    const InstId result = builder_->AddParam(join, type);

    builder_->Branch(present, have, fallback);

    builder_->SetCurrent(have);
    const InstId inside =
        builder_->Add(Opcode::kOptionalValue, type, {optional});
    builder_->Jump(join, {inside});

    builder_->SetCurrent(fallback);
    const InstId other = Value(expr.rhs);
    builder_->Jump(join, {other});

    builder_->SetCurrent(join);
    return result;
  }

  // Which slot of its aggregate a field occupies. Member tables keep
  // declaration order, so counting the fields before it is the offset.
  uint32_t FieldIndex(TypeId owner, Symbol name) const {
    uint32_t index = 0;
    TypeId named = owner;
    // A string's two parts are built in rather than declared, so there is
    // no member table to look them up in.
    if (types_->KindOf(named) == TypeKind::kStr ||
        types_->KindOf(named) == TypeKind::kString) {
      return interner_->Text(name) == "length" ? 1 : 0;
    }
    if (types_->KindOf(named) == TypeKind::kInstance) {
      named = types_->Get(named).base;
    }
    for (const Member& member : resolution_.MembersOf(named)) {
      if (member.kind != MemberKind::kField) {
        continue;
      }
      if (member.name == name) {
        return index;
      }
      ++index;
    }
    return 0;
  }

  InstId LowerField(const Expr& expr, TypeId type) {
    const InstId object = Value(expr.lhs);
    const TypeId owner = TypeOf(expr.lhs);
    return builder_->Extract(type, object, FieldIndex(owner, expr.name));
  }

  // `p?.x` reads the field only when the optional holds a value, and the
  // result is optional again.
  InstId LowerOptionalField(const Expr& expr, TypeId type) {
    const InstId optional = Value(expr.lhs);
    const TypeId owner = Erase(types_->ElementOf(TypeOf(expr.lhs)));
    const InstId present = builder_->Add(
        Opcode::kOptionalIsPresent, types_->bool_type(), {optional});

    const BlockId have = builder_->AddBlock("field.have");
    const BlockId absent = builder_->AddBlock("field.absent");
    const BlockId join = builder_->AddBlock("field.join");
    const InstId result = builder_->AddParam(join, type);

    builder_->Branch(present, have, absent);

    builder_->SetCurrent(have);
    const InstId object =
        builder_->Add(Opcode::kOptionalValue, owner, {optional});
    const TypeId inner = Erase(types_->ElementOf(type));
    const InstId field =
        builder_->Extract(inner, object, FieldIndex(owner, expr.name));
    const InstId wrapped =
        builder_->Add(Opcode::kOptionalWrap, type, {field});
    builder_->Jump(join, {wrapped});

    builder_->SetCurrent(absent);
    builder_->Jump(join, {builder_->ConstNull(type)});

    builder_->SetCurrent(join);
    return result;
  }

  // The address of a place, for `&x`. A `var` already has one -- its slot;
  // anything else is copied into a fresh one, which is what makes taking a
  // reference to a temporary mean something. References are second class
  // (`E0120`), so the slot cannot outlive the frame it is in.
  InstId Address(ExprId place, InstId value) {
    if (ast_.Contains(place) && ast_.Get(place).kind == ExprKind::kPath) {
      const Local* local = Find(ast_.Get(place).name);
      if (local != nullptr && local->slot.is_valid()) {
        return local->slot;
      }
    }
    if (!value.is_valid()) {
      return value;
    }
    const InstId slot = builder_->Add(Opcode::kAlloca, TypeOf(place));
    builder_->Add(Opcode::kStore, TypeId{}, {slot, value});
    return slot;
  }

  InstId LowerAssign(const Expr& expr) {
    const InstId value = Value(expr.rhs);
    if (!ast_.Contains(expr.lhs)) {
      return value;
    }
    const Expr& target = ast_.Get(expr.lhs);
    if (target.kind == ExprKind::kPath) {
      const Local* local = Find(target.name);
      if (local != nullptr && local->slot.is_valid()) {
        builder_->Add(Opcode::kStore, TypeId{}, {local->slot, value});
      }
    } else if (target.kind == ExprKind::kUnary &&
               target.op == TokenKind::kStar) {
      // `*p = v`: the address is what the operand evaluates to.
      const InstId address = Value(target.lhs);
      if (address.is_valid()) {
        builder_->Add(Opcode::kStore, TypeId{}, {address, value});
      }
    }
    // An assignment is a statement, so it produces nothing a caller can use.
    return InstId{};
  }

  InstId LowerWait(const Expr& expr, TypeId type) {
    if (!ast_.Contains(expr.lhs) ||
        ast_.Get(expr.lhs).kind != ExprKind::kCall) {
      // The checker reports a `wait` on anything but a call (`E0303`).
      return builder_->Undef(type);
    }
    const InstId call = Value(expr.lhs);
    if (call.is_valid() &&
        module_->functions[current_function_].Get(call).opcode ==
            Opcode::kCall) {
      module_->functions[current_function_].Get(call).opcode =
          Opcode::kWaitCall;
    }
    return call;
  }

  InstId LowerCall(ExprId id, const Expr& expr, TypeId type) {
    const TypeId callee_type =
        ast_.Contains(expr.lhs) ? TypeOf(expr.lhs) : TypeId{};
    const std::vector<TypeId> params =
        types_->KindOf(callee_type) == TypeKind::kFunction
            ? types_->ArgumentsOf(callee_type)
            : std::vector<TypeId>();

    std::vector<InstId> arguments;
    const std::span<const ExprId> written = ast_.Get(expr.args);
    for (size_t i = 0; i < written.size(); ++i) {
      const InstId value = Value(written[i]);
      arguments.push_back(i < params.size()
                              ? Coerce(value, TypeOf(written[i]), params[i])
                              : value);
    }

    if (ast_.Contains(expr.lhs) &&
        ast_.Get(expr.lhs).kind == ExprKind::kPath) {
      const Symbol name = ast_.Get(expr.lhs).name;
      const ItemId generic = FindGeneric(name);
      if (generic.is_valid()) {
        const uint32_t specialization = Instantiate(generic, id);
        if (specialization != 0xFFFFFFFFu) {
          return builder_->Call(specialization, type,
                                std::move(arguments));
        }
        // Inference did not reach a concrete type for every parameter,
        // which only happens in a body that is itself generic and not
        // instantiated. There is nothing to call.
        return builder_->Undef(type);
      }
      const uint32_t callee = IndexOfName(name);
      if (callee != 0xFFFFFFFFu) {
        return builder_->Call(callee, type, std::move(arguments));
      }

      const Binding* binding = resolution_.Find(name);
      // A constructor: the callee is a type, and the arguments are its
      // fields in order.
      if (binding != nullptr && binding->kind == BindingKind::kType) {
        // The checker records the type the context wanted, so `type` may
        // already be `Pawn?` for a `Pawn(...)`. What the instruction builds
        // is the struct, and the wrap is what bridges the two.
        const TypeId built = Erase(binding->type);
        const InstId value = builder_->Add(
            Opcode::kMakeStruct, built.is_valid() ? built : type,
            std::move(arguments));
        return Coerce(value, built, type);
      }
      // A function from another file. Lowering is per file, so it gets a
      // declaration here -- the same shape a trait method or an `extern`
      // has, and what the linker will resolve.
      if (binding != nullptr && binding->kind == BindingKind::kFunction) {
        return builder_->Call(
            DeclareExternal(name, binding->type, binding->file,
                            binding->item),
            type, std::move(arguments));
      }
    }
    return builder_->Undef(type);
  }

  InstId LowerBlockValue(ExprId id, const Expr& expr, TypeId type) {
    const size_t depth = locals_.size();
    ++scope_depth_;
    const std::span<const StmtId> stmts = ast_.Get(expr.stmts);
    InstId result;
    for (size_t i = 0; i < stmts.size(); ++i) {
      const bool last = i + 1 == stmts.size();
      if (last && ast_.Contains(stmts[i]) &&
          ast_.Get(stmts[i]).kind == StmtKind::kExpr) {
        result = Value(ast_.Get(stmts[i]).expr);
        continue;
      }
      LowerStmt(stmts[i]);
    }
    RunDeferred(scope_depth_);
    ForgetDeferred(scope_depth_);
    --scope_depth_;
    locals_.resize(depth);
    if (result.is_valid() || types_->KindOf(type) == TypeKind::kVoid) {
      return result;
    }
    return builder_->Undef(type);
  }

  InstId LowerIf(const Expr& expr, TypeId type) {
    const bool produces = type.is_valid() &&
                          types_->KindOf(type) != TypeKind::kVoid &&
                          expr.extra.is_valid();

    // `if let q = p` tests whether the optional holds a value and binds what
    // is inside for the then-branch only, which is the whole of flow typing
    // at this level.
    InstId subject;
    InstId condition;
    if (HasFlag(expr.flags, ExprFlags::kHasBinding)) {
      subject = Value(expr.lhs);
      condition = builder_->Add(Opcode::kOptionalIsPresent,
                                types_->bool_type(), {subject});
    } else {
      condition = Value(expr.lhs);
    }
    const BlockId yes = builder_->AddBlock("if.then");
    const BlockId no = builder_->AddBlock("if.else");
    const BlockId join = builder_->AddBlock("if.join");
    const InstId result = produces ? builder_->AddParam(join, type)
                                   : InstId{};

    builder_->Branch(condition, yes, no);

    builder_->SetCurrent(yes);
    const size_t depth = locals_.size();
    if (subject.is_valid() && ast_.Contains(expr.pattern)) {
      const Pattern& pattern = ast_.Get(expr.pattern);
      if (pattern.kind == PatternKind::kBinding) {
        const TypeId inner = Erase(types_->ElementOf(TypeOf(expr.lhs)));
        const InstId value =
            builder_->Add(Opcode::kOptionalValue, inner, {subject});
        locals_.push_back(Local{pattern.name, InstId{}, value, inner});
      }
    }
    // `Value` lowers the block and yields its final expression; `Lower`
    // does the same without a result. Calling both would run the body twice.
    const InstId then_value = produces ? Value(expr.rhs) : InstId{};
    if (!produces) {
      Lower(expr.rhs);
    }
    if (!builder_->Terminated()) {
      builder_->Jump(join, produces ? std::vector<InstId>{then_value}
                                    : std::vector<InstId>{});
    }
    locals_.resize(depth);

    builder_->SetCurrent(no);
    InstId else_value;
    if (expr.extra.is_valid()) {
      if (produces) {
        else_value = Value(expr.extra);
      } else {
        Lower(expr.extra);
      }
    } else if (produces) {
      else_value = builder_->Undef(type);
    }
    if (!builder_->Terminated()) {
      builder_->Jump(join, produces ? std::vector<InstId>{else_value}
                                    : std::vector<InstId>{});
    }

    builder_->SetCurrent(join);
    return result;
  }

  // A `match` becomes a switch when every arm tests an integer-shaped
  // constant, and a chain of comparisons otherwise. The checker already
  // proved the arms cover every value, so the default arm is either the
  // catch-all or unreachable.
  InstId LowerMatch(const Expr& expr, TypeId type) {
    const bool produces =
        type.is_valid() && types_->KindOf(type) != TypeKind::kVoid;
    const InstId subject = Value(expr.lhs);
    const TypeId subject_type = TypeOf(expr.lhs);

    const BlockId join = builder_->AddBlock("match.join");
    const InstId result = produces ? builder_->AddParam(join, type)
                                   : InstId{};

    // Each arm gets a block; the arms are tested in order, so each test
    // needs somewhere to go when it fails.
    struct Arm {
      MatchArmId id;
      BlockId body;
      BlockId next;
      bool catch_all = false;
    };
    std::vector<Arm> arms;
    const std::span<const MatchArmId> arm_ids = ast_.Get(expr.arms);
    for (size_t i = 0; i < arm_ids.size(); ++i) {
      Arm arm;
      arm.id = arm_ids[i];
      arm.body = builder_->AddBlock("match.arm");
      // Only an arm with another one after it needs somewhere to go when its
      // test fails. Giving the last one a block would leave it empty, which
      // the verifier rightly refuses.
      if (i + 1 < arm_ids.size()) {
        arm.next = builder_->AddBlock("match.test");
      }
      arm.catch_all = IsCatchAll(arm_ids[i]);
      arms.push_back(arm);
    }

    for (size_t i = 0; i < arms.size(); ++i) {
      const MatchArm& arm = ast_.Get(arms[i].id);
      const BlockId otherwise = arms[i].next;

      if (arms[i].catch_all) {
        builder_->Jump(arms[i].body);
      } else {
        const InstId test = ArmTest(arm, subject, subject_type);
        if (test.is_valid() && otherwise.is_valid()) {
          builder_->Branch(test, arms[i].body, otherwise);
        } else if (test.is_valid()) {
          // The last arm with no catch-all after it: the checker proved it
          // covers what is left, so failing it cannot happen.
          const BlockId dead = builder_->AddBlock("match.dead");
          builder_->Branch(test, arms[i].body, dead);
          builder_->SetCurrent(dead);
          builder_->Unreachable();
        } else {
          builder_->Jump(arms[i].body);
        }
      }

      builder_->SetCurrent(arms[i].body);
      const size_t depth = locals_.size();
      BindArm(arm, subject, subject_type);
      if (produces) {
        const InstId value = Value(arm.body);
        if (!builder_->Terminated()) {
          builder_->Jump(join, {value});
        }
      } else {
        Lower(arm.body);
        if (!builder_->Terminated()) {
          builder_->Jump(join);
        }
      }
      locals_.resize(depth);

      if (otherwise.is_valid()) {
        builder_->SetCurrent(otherwise);
      }
    }

    if (arms.empty()) {
      builder_->Jump(join);
    }

    builder_->SetCurrent(join);
    return result;
  }

  bool IsCatchAll(MatchArmId id) const {
    const MatchArm& arm = ast_.Get(id);
    if (arm.guard.is_valid()) {
      return false;
    }
    for (PatternId pattern_id : ast_.Get(arm.patterns)) {
      if (!ast_.Contains(pattern_id)) {
        continue;
      }
      const Pattern& pattern = ast_.Get(pattern_id);
      if (pattern.kind == PatternKind::kWildcard ||
          pattern.kind == PatternKind::kRest) {
        return true;
      }
      // A bare name that is not a variant binds everything.
      if (pattern.kind == PatternKind::kBinding) {
        return true;
      }
    }
    return false;
  }

  // What has to be true for |arm| to run. An invalid id means "always".
  InstId ArmTest(const MatchArm& arm, InstId subject, TypeId subject_type) {
    InstId combined;
    for (PatternId pattern_id : ast_.Get(arm.patterns)) {
      if (!ast_.Contains(pattern_id)) {
        continue;
      }
      const InstId test =
          PatternTest(ast_.Get(pattern_id), subject, subject_type);
      if (!test.is_valid()) {
        continue;
      }
      combined = combined.is_valid()
                     ? builder_->Add(Opcode::kBitOr, types_->bool_type(),
                                     {combined, test})
                     : test;
    }
    if (!arm.guard.is_valid()) {
      return combined;
    }
    const InstId guard = Value(arm.guard);
    if (!combined.is_valid()) {
      return guard;
    }
    return builder_->Add(Opcode::kBitAnd, types_->bool_type(),
                         {combined, guard});
  }

  InstId PatternTest(const Pattern& pattern,
                     InstId subject,
                     TypeId subject_type) {
    switch (pattern.kind) {
      case PatternKind::kLiteral: {
        if (!ast_.Contains(pattern.literal)) {
          return InstId{};
        }
        const Expr& literal = ast_.Get(pattern.literal);
        if (literal.kind == ExprKind::kNullLiteral) {
          const InstId present = builder_->Add(
              Opcode::kOptionalIsPresent, types_->bool_type(), {subject});
          return builder_->Add(Opcode::kLogicalNot, types_->bool_type(),
                               {present});
        }
        const InstId value = Value(pattern.literal);
        return builder_->Add(Opcode::kEq, types_->bool_type(),
                             {subject, value});
      }

      case PatternKind::kPresent:
        return builder_->Add(Opcode::kOptionalIsPresent,
                             types_->bool_type(), {subject});

      case PatternKind::kVariant:
      case PatternKind::kStruct: {
        // A variant is identified by the tag the enum stores first. Reading
        // it as a field keeps the layout decision in one place.
        const InstId tag = builder_->Extract(types_->default_int(), subject,
                                             0);
        const InstId expected = builder_->ConstInt(
            types_->default_int(), VariantIndex(subject_type, pattern));
        return builder_->Add(Opcode::kEq, types_->bool_type(),
                             {tag, expected});
      }

      default:
        return InstId{};
    }
  }

  uint64_t VariantIndex(TypeId owner, const Pattern& pattern) const {
    Symbol name = pattern.name;
    const std::span<const Symbol> path = ast_.Get(pattern.path);
    if (!path.empty()) {
      name = path.back();
    }
    uint64_t index = 0;
    for (const Member& member : resolution_.MembersOf(owner)) {
      if (member.kind != MemberKind::kVariant) {
        continue;
      }
      if (member.name == name) {
        return index;
      }
      ++index;
    }
    return 0;
  }

  void BindArm(const MatchArm& arm, InstId subject, TypeId subject_type) {
    for (PatternId pattern_id : ast_.Get(arm.patterns)) {
      if (!ast_.Contains(pattern_id)) {
        continue;
      }
      const Pattern& pattern = ast_.Get(pattern_id);
      if (pattern.kind == PatternKind::kBinding &&
          !IsVariantName(subject_type, pattern.name)) {
        locals_.push_back(
            Local{pattern.name, InstId{}, subject, subject_type});
        continue;
      }
      if (pattern.kind == PatternKind::kPresent) {
        const TypeId inner = Erase(types_->ElementOf(subject_type));
        const InstId value =
            builder_->Add(Opcode::kOptionalValue, inner, {subject});
        for (PatternId element : ast_.Get(pattern.elements)) {
          if (ast_.Contains(element) &&
              ast_.Get(element).kind == PatternKind::kBinding) {
            locals_.push_back(
                Local{ast_.Get(element).name, InstId{}, value, inner});
          }
        }
      }
    }
  }

  bool IsVariantName(TypeId owner, Symbol name) const {
    for (const Member& member : resolution_.MembersOf(owner)) {
      if (member.kind == MemberKind::kVariant && member.name == name) {
        return true;
      }
    }
    return false;
  }

  void LowerLoop(const Expr& expr) {
    const BlockId header = builder_->AddBlock("loop.header");
    const BlockId body = builder_->AddBlock("loop.body");

    builder_->Jump(header);
    builder_->SetCurrent(header);

    // A `while` leaves the loop when its condition fails, so it needs an
    // exit before the body is seen; a `loop` only gets one if something
    // breaks.
    BlockId exit;
    if (expr.kind == ExprKind::kWhile) {
      exit = builder_->AddBlock("loop.exit");
      const InstId condition = Value(expr.lhs);
      builder_->Branch(condition, body, exit);
    } else {
      builder_->Jump(body);
    }

    loops_.push_back(LoopTarget{expr.name, exit, header,
                                scope_depth_ + 1});
    builder_->SetCurrent(body);
    // Both forms keep the body in `rhs`; a `while` also has its condition
    // in `lhs`.
    Lower(expr.rhs);
    if (!builder_->Terminated()) {
      builder_->Jump(header);
    }
    const BlockId reached = loops_.back().break_to;
    loops_.pop_back();

    if (reached.is_valid()) {
      builder_->SetCurrent(reached);
      return;
    }
    // Nothing leaves the loop. Everything after it is unreachable, which the
    // checker reported as W0102; leaving the builder on a block that is
    // already terminated is what stops anything more being emitted.
    builder_->SetCurrent(body);
  }


  struct Pending {
    ItemId item;
    TypeStore::Substitution substitution;
    uint32_t index = 0;
  };

  StringInterner* interner_;
  TypeStore* types_;
  const FileResolution& resolution_;
  const Ast& ast_;
  const TypedBody& typed_;
  std::span<const std::pair<ItemId, EffectRow>> effects_;
  Module* module_;
  // Lowering runs only on a file the checker accepted, so nothing here can
  // report: the engine is where the resolver's complaints go to be
  // dropped.
  DiagnosticEngine discarded_;
  TypeResolver resolver_;

  // Generic declarations by name: a recipe, not a function.
  std::vector<std::pair<Symbol, ItemId>> generics_;
  std::vector<std::pair<Symbol, uint32_t>> specializations_;
  std::vector<Pending> pending_;
  std::vector<Deferred> deferred_;
  // Whether the function being lowered is a `task`.
  bool in_task_ = false;
  // How many blocks deep lowering is. A `defer` belongs to the block it was
  // written in, and the count of locals cannot say which block that is -- a
  // block that declares nothing does not change it.
  size_t scope_depth_ = 0;
  TypeStore::Substitution substitution_;

  Builder* builder_ = nullptr;
  uint32_t current_function_ = 0;
  TypeId return_type_;
  std::vector<Local> locals_;
  std::vector<LoopTarget> loops_;
  std::vector<std::pair<ItemId, size_t>> indices_;
};

}  // namespace

void LowerFile(StringInterner* interner,
               TypeStore* types,
               const FileResolution& resolution,
               const Ast& ast,
               const TypedBody& typed,
               std::span<const std::pair<ItemId, EffectRow>> effects,
               Module* module) {
  Lowerer lowerer(interner, types, resolution, ast, typed, effects, module);
  lowerer.Run();
}

}  // namespace ir
}  // namespace wolang
