// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/ir/lower.h"

#include <utility>

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
struct LoopTarget {
  Symbol label;
  BlockId break_to;
  BlockId continue_to;
};

class Lowerer {
 public:
  Lowerer(TypeStore* types,
          const FileResolution& resolution,
          const Ast& ast,
          const TypedBody& typed,
          Module* module)
      : types_(types),
        resolution_(resolution),
        ast_(ast),
        typed_(typed),
        module_(module) {}

  void Run() {
    // Two passes: every function gets a declaration first, so a call can
    // name one that is defined later in the file.
    for (ItemId id : ast_.Get(ast_.root())) {
      Declare(id);
    }
    for (ItemId id : ast_.Get(ast_.root())) {
      Define(id);
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
        Function function;
        function.name = item.name;
        function.type =
            binding != nullptr ? Erase(binding->type) : TypeId{};
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
  uint32_t DeclareExternal(Symbol name, TypeId type) {
    for (uint32_t i = 0; i < module_->functions.size(); ++i) {
      if (module_->functions[i].name == name) {
        return i;
      }
    }
    Function declaration;
    declaration.name = name;
    declaration.type = Erase(type);
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
    builder_ = &builder;
    locals_.clear();
    loops_.clear();

    const BlockId entry = builder.AddBlock("entry");
    builder.SetCurrent(entry);

    for (ParamId param_id : ast_.Get(item.params)) {
      const Param& param = ast_.Get(param_id);
      const TypeId type =
          ParamTypeOf(module_->functions[index], locals_.size());
      const InstId value = builder.AddParam(entry, type);
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
    return Erase(recorded.is_valid() ? recorded : types_->error());
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
      for (StmtId stmt : ast_.Get(expr.stmts)) {
        LowerStmt(stmt);
      }
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

      case StmtKind::kReturn:
        if (stmt.expr.is_valid()) {
          builder_->Return(Value(stmt.expr));
        } else {
          builder_->ReturnVoid();
        }
        return;

      case StmtKind::kBreak:
      case StmtKind::kContinue: {
        const LoopTarget* target = FindLoop(stmt.label);
        if (target == nullptr) {
          // The checker already reported a `break` with no loop.
          builder_->Unreachable();
          return;
        }
        builder_->Jump(stmt.kind == StmtKind::kBreak ? target->break_to
                                                     : target->continue_to);
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

    const TypeId type = stmt.expr.is_valid() ? TypeOf(stmt.expr)
                                             : types_->error();
    const InstId value = stmt.expr.is_valid() ? Value(stmt.expr) : InstId{};

    if (!mutable_binding &&
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

  const LoopTarget* FindLoop(Symbol label) const {
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
        return builder_->ConstInt(type, number.integer_value);
      }
      case ExprKind::kFloatLiteral: {
        const NumberLiteral& number = ast_.number(expr.aux);
        return builder_->ConstFloat(type, number.float_value);
      }
      case ExprKind::kBoolLiteral:
        return builder_->ConstBool(expr.aux != 0);
      case ExprKind::kNullLiteral:
        return builder_->ConstNull(type);
      case ExprKind::kStringLiteral:
        return LowerString(expr, type);

      case ExprKind::kPath:
        return LowerPath(expr, type);

      case ExprKind::kUnary:
        return LowerUnary(expr, type);
      case ExprKind::kBinary:
        return LowerBinary(id, expr, type);
      case ExprKind::kAssign:
        return LowerAssign(expr);

      case ExprKind::kField:
        return LowerField(expr, type);
      case ExprKind::kOptionalField:
        return LowerOptionalField(expr, type);

      case ExprKind::kCall:
        return LowerCall(expr, type);

      case ExprKind::kBlock:
        return LowerBlockValue(id, expr, type);

      case ExprKind::kIf:
        return LowerIf(expr, type);

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
    // Interpolation builds a string at runtime, which needs the formatting
    // machinery from `core`. Until then the pieces are evaluated for their
    // effects and the result is undefined.
    for (ExprId part : parts) {
      if (ast_.Contains(part) &&
          ast_.Get(part).kind != ExprKind::kStringText) {
        Value(part);
      }
    }
    return builder_->Undef(type);
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
    }
    // An assignment is a statement, so it produces nothing a caller can use.
    return InstId{};
  }

  InstId LowerCall(const Expr& expr, TypeId type) {
    std::vector<InstId> arguments;
    for (ExprId argument : ast_.Get(expr.args)) {
      arguments.push_back(Value(argument));
    }

    if (ast_.Contains(expr.lhs) &&
        ast_.Get(expr.lhs).kind == ExprKind::kPath) {
      const Symbol name = ast_.Get(expr.lhs).name;
      const uint32_t callee = IndexOfName(name);
      if (callee != 0xFFFFFFFFu) {
        return builder_->Call(callee, type, std::move(arguments));
      }

      const Binding* binding = resolution_.Find(name);
      // A constructor: the callee is a type, and the arguments are its
      // fields in order.
      if (binding != nullptr && binding->kind == BindingKind::kType) {
        return builder_->Add(Opcode::kMakeStruct, type,
                             std::move(arguments));
      }
      // A function from another file. Lowering is per file, so it gets a
      // declaration here -- the same shape a trait method or an `extern`
      // has, and what the linker will resolve.
      if (binding != nullptr && binding->kind == BindingKind::kFunction) {
        return builder_->Call(DeclareExternal(name, binding->type), type,
                              std::move(arguments));
      }
    }
    return builder_->Undef(type);
  }

  InstId LowerBlockValue(ExprId id, const Expr& expr, TypeId type) {
    const size_t depth = locals_.size();
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

  void LowerLoop(const Expr& expr) {
    const BlockId header = builder_->AddBlock("loop.header");
    const BlockId body = builder_->AddBlock("loop.body");
    const BlockId exit = builder_->AddBlock("loop.exit");

    builder_->Jump(header);
    builder_->SetCurrent(header);

    if (expr.kind == ExprKind::kWhile) {
      const InstId condition = Value(expr.lhs);
      builder_->Branch(condition, body, exit);
    } else {
      builder_->Jump(body);
    }

    loops_.push_back(LoopTarget{expr.name, exit, header});
    builder_->SetCurrent(body);
    // Both forms keep the body in `rhs`; a `while` also has its condition
    // in `lhs`.
    Lower(expr.rhs);
    if (!builder_->Terminated()) {
      builder_->Jump(header);
    }
    loops_.pop_back();

    builder_->SetCurrent(exit);
  }

  TypeStore* types_;
  const FileResolution& resolution_;
  const Ast& ast_;
  const TypedBody& typed_;
  Module* module_;

  Builder* builder_ = nullptr;
  TypeId return_type_;
  std::vector<Local> locals_;
  std::vector<LoopTarget> loops_;
  std::vector<std::pair<ItemId, size_t>> indices_;
};

}  // namespace

void LowerFile(TypeStore* types,
               const FileResolution& resolution,
               const Ast& ast,
               const TypedBody& typed,
               Module* module) {
  Lowerer lowerer(types, resolution, ast, typed, module);
  lowerer.Run();
}

}  // namespace ir
}  // namespace wolang
