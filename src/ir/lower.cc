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
          uint32_t file_index,
          const FileResolution& resolution,
          const Ast& ast,
          const TypedBody& typed,
          std::span<const std::pair<ItemId, EffectRow>> effects,
          Module* module)
      : interner_(interner),
        types_(types),
        file_index_(file_index),
        resolution_(resolution),
        ast_(ast),
        typed_(typed),
        effects_(effects),
        module_(module),
        resolver_(types, interner, &resolution, &discarded_),
        self_name_(interner->Intern("self")) {}

  void Run() {
    // Two passes: every function gets a declaration first, so a call can
    // name one that is defined later in the file.
    for (ItemId id : ast_.Get(ast_.root())) {
      Declare(id, TypeId{});
    }
    for (ItemId id : ast_.Get(ast_.root())) {
      Define(id, TypeId{});
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

  // |owner| is the type whose body this declaration is in, or an invalid id
  // at file scope. A `fun` inside a type is a method: it takes `self` and it
  // is not a file-scope name, so neither its signature nor its identity can
  // be looked up the way a free function's is.
  void Declare(ItemId id, TypeId owner) {
    if (!ast_.Contains(id)) {
      return;
    }
    const Item& item = ast_.Get(id);
    switch (item.kind) {
      case ItemKind::kFunction: {
        if (owner.is_valid()) {
          DeclareMethod(id, item, owner);
          break;
        }
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
        function.is_primitive_task =
            function.is_task && !item.body.is_valid();
        if (HasFlag(item.flags, ItemFlags::kExtern)) {
          // `extern "C" fun puts(...) = "puts"` names the symbol; without
          // the clause the symbol is the declaration's own name.
          function.foreign_symbol =
              item.link_name.is_valid() ? item.link_name : item.name;
        }
        Remember(function.source_file, function.source_item,
                 module_->functions.size());
        indices_.emplace_back(id, module_->functions.size());
        module_->functions.push_back(std::move(function));
        break;
      }
      case ItemKind::kBinding: {
        if (owner.is_valid()) {
          // A `derived var` inside a type becomes a method that computes
          // it: the expression lives in the owner's file, and a read from
          // another one has to be able to reach it.
          DeclareDerived(id, item, owner);
          break;
        }
        // A module-level `const` becomes a function that returns it, and a
        // use becomes a call the inliner removes. Two reasons for that
        // shape: the value may be in another file, where only a function
        // can be reached; and a constant is a compile-time value, so
        // there is nothing to initialize at startup.
        //
        // Reading one used to lower to an undefined value, which is to say
        // a program that named a constant computed with nothing.
        if (!item.name.is_valid() || !item.init.is_valid()) {
          break;
        }
        const Binding* binding = resolution_.Find(item.name);
        Function function;
        function.name = ConstantName(item.name);
        function.type = types_->Function(
            {}, Erase(binding != nullptr ? binding->type : TypeId{}));
        if (binding != nullptr) {
          function.source_file = binding->file;
          function.source_item = binding->item;
        }
        Remember(function.source_file, function.source_item,
                 module_->functions.size());
        constants_.emplace_back(item.name, module_->functions.size());
        indices_.emplace_back(id, module_->functions.size());
        module_->functions.push_back(std::move(function));
        break;
      }

      case ItemKind::kMachine:
        DeclareMachine(id, item);
        break;

      case ItemKind::kClass:
      case ItemKind::kStruct:
      case ItemKind::kComponent:
      case ItemKind::kEnum:
      case ItemKind::kExtend:
        // Methods are functions too; their bodies are lowered the same way.
        for (ItemId member : ast_.Get(item.members)) {
          Declare(member, OwnerOf(item));
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
                           uint32_t item = Function::kNoSource,
                           bool is_task = false,
                           bool has_body = false,
                           uint32_t slot = 0) {
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
    declaration.source_slot = slot;
    declaration.is_task = is_task;
    // A task with no body anywhere is the suspension primitive, and this
    // file may write its machine; one whose body is elsewhere may not.
    declaration.is_primitive_task = is_task && !has_body;
    module_->functions.push_back(std::move(declaration));
    return static_cast<uint32_t>(module_->functions.size() - 1);
  }

  // The type a declaration's members belong to: the type it declares, or
  // for an `extend` block the type being extended.
  TypeId OwnerOf(const Item& item) {
    if (item.kind == ItemKind::kExtend) {
      return item.target_type.is_valid()
                 ? Erase(resolver_.Resolve(ast_, item.target_type))
                 : TypeId{};
    }
    const Binding* binding = resolution_.Find(item.name);
    return binding != nullptr ? Erase(binding->type) : TypeId{};
  }

  // A class is a handle, so constructing one allocates and then writes the
  // fields through the handle. The size is the tier's answer -- lowering
  // has no layout -- which is why `new` is an operation.
  InstId NewObject(TypeId type, const std::vector<InstId>& fields) {
    const InstId object = builder_->Add(Opcode::kNew, type);
    uint32_t index = 0;
    for (InstId value : fields) {
      const TypeId field_type = Erase(FieldTypeAt(type, index));
      const InstId address = builder_->FieldAddress(
          types_->Pointer(field_type, /*is_mutable=*/true), object, index);
      builder_->Add(Opcode::kStore, TypeId{}, {address, value});
      ++index;
    }
    return object;
  }

  // A method: `self` first, then what was written. The receiver is passed
  // by address, so a method that assigns to a field changes the object it
  // was called on rather than a copy of it -- which is the difference
  // between a method and a function that happens to be written inside a
  // type.
  void DeclareMethod(ItemId id, const Item& item, TypeId owner) {
    const Member* member = resolution_.FindMember(owner, item.name);
    const TypeId declared = member != nullptr ? member->type : TypeId{};
    if (!declared.is_valid() ||
        types_->KindOf(declared) != TypeKind::kFunction) {
      return;
    }
    if (types_->MentionsParameter(declared) ||
        types_->MentionsParameter(owner)) {
      // A generic type's method is a recipe, the same way a generic
      // function is. Instantiation is wave 8's machinery and this is not
      // wired into it yet, so the interpreter keeps them.
      return;
    }

    std::vector<TypeId> params;
    params.push_back(owner);
    for (TypeId param : types_->ArgumentsOf(declared)) {
      params.push_back(Erase(param));
    }

    Function function;
    function.name = MethodName(owner, item.name);
    function.type = types_->Function(params,
                                     Erase(types_->ElementOf(declared)));
    function.effects = EffectsOf(id, &function.effects_known);
    function.source_file = member->file;
    function.source_item = member->item;
    function.is_task = HasFlag(item.flags, ItemFlags::kAsync);
    function.is_primitive_task =
        function.is_task && !item.body.is_valid();
    function.self_type = owner;

    Remember(function.source_file, function.source_item,
             module_->functions.size());
    indices_.emplace_back(id, module_->functions.size());
    module_->functions.push_back(std::move(function));
  }

  // `Point.Sum`, so that two types may each have a `Sum` and a dump says
  // which one it is looking at.
  Symbol MethodName(TypeId owner, Symbol name) {
    std::string text = types_->Describe(*interner_, owner);
    text.push_back('.');
    text.append(interner_->Text(name));
    return interner_->Intern(text);
  }

  void Remember(uint32_t file, uint32_t item, size_t index,
                uint32_t slot = 0) {
    if (file == Function::kNoSource || item == Function::kNoSource) {
      return;
    }
    by_source_.push_back(
        Source{file, item, slot, static_cast<uint32_t>(index)});
  }

  uint32_t IndexOfSource(uint32_t file, uint32_t item,
                         uint32_t slot = 0) const {
    for (const Source& source : by_source_) {
      if (source.file == file && source.item == item &&
          source.slot == slot) {
        return source.index;
      }
    }
    return 0xFFFFFFFFu;
  }

  uint32_t IndexOfName(Symbol name) const {
    for (const auto& [item, index] : indices_) {
      if (ast_.Contains(item) && ast_.Get(item).name == name) {
        return static_cast<uint32_t>(index);
      }
    }
    return 0xFFFFFFFFu;
  }

  void Define(ItemId id, TypeId owner) {
    if (!ast_.Contains(id)) {
      return;
    }
    const Item& item = ast_.Get(id);
    switch (item.kind) {
      case ItemKind::kFunction:
        self_type_ = owner;
        LowerFunction(item, IndexOf(id));
        self_type_ = TypeId{};
        break;
      case ItemKind::kBinding:
        if (owner.is_valid()) {
          // The body of a derived field's computation, with `self` bound
          // the way a method's is.
          self_type_ = owner;
          LowerConstant(item, IndexOf(id));
          self_type_ = TypeId{};
        } else {
          LowerConstant(item, IndexOf(id));
        }
        break;
      case ItemKind::kMachine:
        DefineMachine(id, item);
        break;

      case ItemKind::kClass:
      case ItemKind::kStruct:
      case ItemKind::kComponent:
      case ItemKind::kEnum:
      case ItemKind::kExtend:
        for (ItemId member : ast_.Get(item.members)) {
          Define(member, OwnerOf(item));
        }
        break;
      default:
        break;
    }
  }

  // A machine's generated functions, waiting for their bodies. The
  // transition table is the source; these are what it means.
  struct MachineBlock {
    Symbol state;
    bool is_enter = false;
    ExprId body;
    uint32_t index = 0;
  };
  struct MachineMethod {
    TypeId machine;
    Symbol name;
    uint32_t index = 0;
  };

  // A machine's methods: one per event, one per state, and the blocks that
  // run on the way in and out of a state.
  //
  // Generated rather than written, which is the whole point of the
  // declaration: the transition table is the source, and the methods are
  // what it means.
  void DeclareMachine(ItemId id, const Item& item) {
    const Binding* binding = resolution_.Find(item.name);
    if (binding == nullptr) {
      return;
    }
    const TypeId machine = Erase(binding->type);
    const TypeId answer = types_->bool_type();

    // The blocks first, so an event can call them.
    for (StateId state_id : ast_.Get(item.states)) {
      if (!ast_.Contains(state_id)) {
        continue;
      }
      const State& state = ast_.Get(state_id);
      for (int which = 0; which < 2; ++which) {
        const ExprId block = which == 0 ? state.enter : state.exit;
        if (!ast_.Contains(block)) {
          continue;
        }
        Function function;
        function.name = MachineBlockName(machine, state.name,
                                         which == 0 ? "enter" : "exit");
        function.type = types_->Function({machine}, types_->void_type());
        function.self_type = machine;
        function.source_file = binding->file;
        function.source_item = binding->item;
        machine_blocks_.push_back(
            MachineBlock{state.name, which == 0, block,
                         static_cast<uint32_t>(module_->functions.size())});
        module_->functions.push_back(std::move(function));
      }
    }

    for (const Member& member : resolution_.MembersOf(machine)) {
      if (member.kind != MemberKind::kMethod) {
        continue;
      }
      Function function;
      function.name = MethodName(machine, member.name);
      function.type = types_->Function({machine}, answer);
      function.self_type = machine;
      function.source_file = member.file;
      function.source_item = member.item;
      function.source_slot = member.slot;
      Remember(member.file, member.item, module_->functions.size(),
               member.slot);
      machine_methods_.push_back(
          MachineMethod{machine, member.name,
                        static_cast<uint32_t>(module_->functions.size())});
      module_->functions.push_back(std::move(function));
    }
    (void)id;
  }

  // The bodies. An event method is the transition table as code: which
  // state we are in decides what happens, and nothing happens when the
  // current state has no transition for that event.
  void DefineMachine(ItemId id, const Item& item) {
    const Binding* binding = resolution_.Find(item.name);
    if (binding == nullptr) {
      return;
    }
    const TypeId machine = Erase(binding->type);

    // The states, in the order they were declared: that order is the
    // numbering, and the first one is where a machine starts.
    std::vector<StateId> states;
    for (StateId state_id : ast_.Get(item.states)) {
      if (ast_.Contains(state_id)) {
        states.push_back(state_id);
      }
    }

    for (const MachineBlock& block : machine_blocks_) {
      LowerMachineBlock(machine, block);
    }
    for (const MachineMethod& method : machine_methods_) {
      if (method.machine != machine) {
        continue;
      }
      LowerMachineMethod(machine, states, method);
    }
    machine_blocks_.clear();
    machine_methods_.clear();
    (void)id;
  }

  void LowerMachineBlock(TypeId machine, const MachineBlock& block) {
    Builder builder(module_, FunctionId{block.index});
    builder.types = types_;
    current_function_ = block.index;
    builder_ = &builder;
    locals_.clear();
    loops_.clear();
    deferred_.clear();
    arenas_.clear();
    scope_depth_ = 0;
    in_task_ = false;
    self_type_ = machine;
    return_type_ = types_->void_type();

    const BlockId entry = builder.AddBlock("entry");
    builder.SetCurrent(entry);
    self_ = builder.AddParam(entry, machine);
    locals_.push_back(Local{self_name_, InstId{}, self_, machine});
    Lower(block.body);
    if (!builder.Terminated()) {
      builder.ReturnVoid();
    }
    self_type_ = TypeId{};
    self_ = InstId{};
    builder_ = nullptr;
  }

  void LowerMachineMethod(TypeId machine,
                          const std::vector<StateId>& states,
                          const MachineMethod& method) {
    Builder builder(module_, FunctionId{method.index});
    builder.types = types_;
    current_function_ = method.index;
    builder_ = &builder;
    locals_.clear();
    loops_.clear();
    deferred_.clear();
    arenas_.clear();
    scope_depth_ = 0;
    in_task_ = false;
    self_type_ = machine;
    return_type_ = types_->bool_type();

    const BlockId entry = builder.AddBlock("entry");
    builder.SetCurrent(entry);
    self_ = builder.AddParam(entry, machine);

    const std::string_view name = interner_->Text(method.name);
    const bool predicate = name.size() > 2 && name.substr(0, 2) == "Is";
    const TypeId state_type = types_->Int(IntWidth::k32, true);
    const InstId state_address = builder.FieldAddress(
        types_->Pointer(state_type, true), self_,
        MachineStateIndex(machine));

    if (predicate) {
      // `IsOpen()`: which state, asked by name.
      const uint32_t which =
          IndexOfState(states, interner_->Intern(name.substr(2)));
      const InstId current =
          builder.Add(Opcode::kLoad, state_type, {state_address});
      builder.Return(builder.Add(
          Opcode::kEq, types_->bool_type(),
          {current, builder.ConstInt(state_type, which)}));
      Finish();
      return;
    }

    // An event: a switch over the states that have a transition for it.
    std::vector<uint32_t> froms;
    std::vector<uint32_t> targets;
    for (uint32_t i = 0; i < states.size(); ++i) {
      const State& state = ast_.Get(states[i]);
      for (TransitionId transition_id : ast_.Get(state.transitions)) {
        if (!ast_.Contains(transition_id)) {
          continue;
        }
        const Transition& transition = ast_.Get(transition_id);
        if (transition.event != method.name) {
          continue;
        }
        froms.push_back(i);
        targets.push_back(Settle(states, transition.target));
        break;
      }
    }

    const InstId current =
        builder.Add(Opcode::kLoad, state_type, {state_address});
    const BlockId nothing = builder.AddBlock("event.none");
    std::vector<BlockId> arms;
    for (size_t i = 0; i < froms.size(); ++i) {
      arms.push_back(builder.AddBlock("event.arm"));
    }
    // One compare per state that answers to this event. A switch would do
    // as well; this keeps the shape the same whether there is one arm or
    // ten, and the optimizer turns a chain of compares on a small integer
    // into what it wants anyway.
    for (size_t i = 0; i < froms.size(); ++i) {
      const BlockId next = builder.AddBlock("event.test");
      const InstId matches = builder.Add(
          Opcode::kEq, types_->bool_type(),
          {current, builder.ConstInt(state_type,
                                     static_cast<uint64_t>(froms[i]))});
      builder.Branch(matches, arms[i], next);
      builder.SetCurrent(next);
    }
    builder.Jump(nothing);

    for (size_t i = 0; i < froms.size(); ++i) {
      builder.SetCurrent(arms[i]);
      // Out of the old state, into the new one, and through whatever
      // `always` leads from there -- every state on the way gets its
      // blocks run.
      CallMachineBlock(machine, ast_.Get(states[froms[i]]).name,
                       /*is_enter=*/false);
      builder.Add(Opcode::kStore, TypeId{},
                  {state_address,
                   builder.ConstInt(state_type,
                                    static_cast<uint64_t>(targets[i]))});
      for (uint32_t step : PathTo(states, froms[i], method.name)) {
        CallMachineBlock(machine, ast_.Get(states[step]).name,
                         /*is_enter=*/true);
      }
      builder.Return(builder.ConstBool(true));
    }

    builder.SetCurrent(nothing);
    builder.Return(builder.ConstBool(false));
    Finish();
  }

  void Finish() {
    self_type_ = TypeId{};
    self_ = InstId{};
    builder_ = nullptr;
  }

  // The states entered by taking |event| out of state |from|: the target,
  // and then whatever `always` leads on from it.
  std::vector<uint32_t> PathTo(const std::vector<StateId>& states,
                               uint32_t from,
                               Symbol event) {
    std::vector<uint32_t> path;
    const State& state = ast_.Get(states[from]);
    for (TransitionId transition_id : ast_.Get(state.transitions)) {
      if (!ast_.Contains(transition_id)) {
        continue;
      }
      const Transition& transition = ast_.Get(transition_id);
      if (transition.event != event) {
        continue;
      }
      uint32_t at = IndexOfState(states, transition.target);
      // `always` is followed until it stops or comes back round: a cycle
      // of them would otherwise be a loop with no way out.
      for (size_t guard = 0; guard <= states.size(); ++guard) {
        if (at >= states.size() ||
            std::find(path.begin(), path.end(), at) != path.end()) {
          break;
        }
        path.push_back(at);
        const uint32_t next = AlwaysFrom(states, at);
        if (next == at || next >= states.size()) {
          break;
        }
        at = next;
      }
      break;
    }
    return path;
  }

  uint32_t AlwaysFrom(const std::vector<StateId>& states, uint32_t from) {
    if (from >= states.size()) {
      return from;
    }
    const State& state = ast_.Get(states[from]);
    for (TransitionId transition_id : ast_.Get(state.transitions)) {
      if (!ast_.Contains(transition_id)) {
        continue;
      }
      const Transition& transition = ast_.Get(transition_id);
      if (transition.is_always) {
        return IndexOfState(states, transition.target);
      }
    }
    return from;
  }

  // Where a transition really ends up, following `always` from its target.
  uint32_t Settle(const std::vector<StateId>& states, Symbol target) {
    uint32_t at = IndexOfState(states, target);
    for (size_t guard = 0; guard <= states.size(); ++guard) {
      const uint32_t next = AlwaysFrom(states, at);
      if (next == at) {
        return at;
      }
      at = next;
    }
    return at;
  }

  uint32_t IndexOfState(const std::vector<StateId>& states, Symbol name) {
    for (uint32_t i = 0; i < states.size(); ++i) {
      if (ast_.Get(states[i]).name == name) {
        return i;
      }
    }
    return static_cast<uint32_t>(states.size());
  }

  void CallMachineBlock(TypeId machine, Symbol state, bool is_enter) {
    for (const MachineBlock& block : machine_blocks_) {
      if (block.state != state || block.is_enter != is_enter) {
        continue;
      }
      builder_->Call(block.index, types_->void_type(), {self_});
      return;
    }
  }

  uint32_t MachineStateIndex(TypeId machine) {
    uint32_t index = 0;
    for (const Member& member : resolution_.MembersOf(machine)) {
      if (!HasStorage(member.kind)) {
        continue;
      }
      if (member.kind == MemberKind::kMachineState) {
        return index;
      }
      ++index;
    }
    return 0;
  }

  Symbol MachineBlockName(TypeId machine, Symbol state, const char* which) {
    std::string text = types_->Describe(*interner_, machine);
    text.push_back('.');
    text.append(interner_->Text(state));
    text.push_back('$');
    text.append(which);
    return interner_->Intern(text);
  }

  // A derived field's computation, as a method of the type: `self` in,
  // the value out. Declared even when the field is read from another file,
  // because the linker resolves it by the declaration it came from.
  void DeclareDerived(ItemId id, const Item& item, TypeId owner) {
    if (!HasFlag(item.flags, ItemFlags::kDerived) ||
        !item.name.is_valid() || !item.init.is_valid()) {
      return;
    }
    const Member* member = resolution_.FindMember(owner, item.name);
    if (member == nullptr || member->kind != MemberKind::kDerived) {
      return;
    }
    Function function;
    function.name = DerivedName(owner, item.name);
    function.type = types_->Function({owner}, Erase(member->type));
    function.effects = EffectsOf(id, &function.effects_known);
    function.source_file = member->file;
    function.source_item = member->item;
    function.self_type = owner;
    Remember(function.source_file, function.source_item,
             module_->functions.size());
    indices_.emplace_back(id, module_->functions.size());
    module_->functions.push_back(std::move(function));
  }

  Symbol DerivedName(TypeId owner, Symbol name) {
    std::string text = types_->Describe(*interner_, owner);
    text.push_back('.');
    text.append(interner_->Text(name));
    text.append("$compute");
    return interner_->Intern(text);
  }

  // `K$name`, so that a constant and a function of the same name cannot
  // collide in the module.
  Symbol ConstantName(Symbol name) {
    std::string text = "K$";
    text.append(interner_->Text(name));
    return interner_->Intern(text);
  }

  // The body of a module-level constant: return what it was written as.
  void LowerConstant(const Item& item, uint32_t index) {
    if (index >= module_->functions.size() || !item.init.is_valid()) {
      return;
    }
    Builder builder(module_, FunctionId{index});
    builder.types = types_;
    current_function_ = index;
    builder_ = &builder;
    locals_.clear();
    loops_.clear();
    deferred_.clear();
    arenas_.clear();
    scope_depth_ = 0;
    in_task_ = false;

    const BlockId entry = builder.AddBlock("entry");
    builder.SetCurrent(entry);

    // A derived field's computation is a method: the receiver comes in
    // first, and the expression reads its fields through it.
    self_ = InstId{};
    if (self_type_.is_valid()) {
      self_ = builder.AddParam(entry, self_type_);
      locals_.push_back(Local{self_name_, InstId{}, self_, self_type_});
    }

    return_type_ = types_->ElementOf(module_->functions[index].type);
    const InstId value = Value(item.init);
    if (types_->KindOf(return_type_) == TypeKind::kVoid) {
      builder.ReturnVoid();
    } else {
      builder.Return(Coerce(value, TypeOf(item.init), return_type_));
    }
    builder_ = nullptr;
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

    self_ = InstId{};
    if (self_type_.is_valid()) {
      // The receiver is an ordinary first parameter of the object's own
      // type. Which means a method of a value type gets a copy -- and
      // assigning to a field of it is rejected by the checker rather than
      // silently writing to the copy. A class is a handle, so a method of
      // one will mutate the object it was called on; that is D61 and it
      // needs the heap first.
      self_ = builder.AddParam(entry, self_type_);
      locals_.push_back(Local{self_name_, InstId{}, self_, self_type_});
    }

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
  // An array where a slice is wanted: the pair that views it. The spec
  // says an array converts to a slice, and this is what that costs --
  // the address of the first element and the length the type knows.
  InstId SliceOf(InstId array, TypeId from, TypeId to, ExprId source) {
    const TypeId element = Erase(types_->ElementOf(to));
    // The elements have to be somewhere for a view to point at them, and
    // *which* somewhere is the whole question: a view of a copy is a view
    // nobody else can see, so a callee that fills it fills nothing.
    //
    // So: the array's own storage when the expression has any -- a local,
    // a field, an element -- and a copy only for a value that lives
    // nowhere, like the result of a call.
    InstId storage;
    if (const Place place = PlaceOf(source); place.addressable()) {
      storage = AddressOfPlace(place);
    } else {
      storage = builder_->Add(Opcode::kAlloca, Erase(from));
      builder_->Add(Opcode::kStore, TypeId{}, {storage, array});
    }
    const InstId zero = builder_->ConstInt(types_->default_int(), 0);
    const InstId length = builder_->ConstInt(
        types_->default_int(),
        static_cast<uint64_t>(types_->Get(Erase(from)).length));
    const InstId first = builder_->IndexAddress(
        types_->Pointer(element, /*is_mutable=*/true), storage, zero,
        length);
    const InstId raw = builder_->Add(
        Opcode::kCast, types_->Int(IntWidth::k64, /*is_signed=*/false),
        {length});
    return builder_->Add(Opcode::kMakeStruct, Erase(to), {first, raw});
  }

  // |source| is the expression the value came from, when there is one.
  // Only the array-to-slice conversion needs it, and it needs it badly:
  // a view has to point at the array itself and not at a copy.
  InstId Coerce(InstId value,
                TypeId from,
                TypeId to,
                ExprId source = ExprId{}) {
    if (!value.is_valid() || !to.is_valid()) {
      return value;
    }
    const TypeId target = Erase(to);
    if (types_->KindOf(target) == TypeKind::kSlice &&
        types_->KindOf(Erase(from)) == TypeKind::kArray) {
      return SliceOf(value, from, to, source);
    }
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
                ? Coerce(Value(stmt.expr), TypeOf(stmt.expr), return_type_,
                         stmt.expr)
                : InstId{};
        RunDeferred(0);
        // Every arena this return is leaving goes with it.
        ReleaseArenas(0);
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
        ReleaseArenas(target->scope);
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
        // Bytes as text and back: both are a pointer and a length, so the
        // value carries across unchanged and the conversion is a fact
        // about types rather than an operation.
        if (IsView(TypeOf(expr.lhs)) && IsView(type)) {
          return operand;
        }
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
        // Through the address when there is one, so that reading one
        // element of a buffer does not copy the buffer. A place that is
        // only readable -- an aggregate that is a value and not anywhere
        // -- reads out of the value, and either way the operands are
        // lowered once: asking twice would evaluate the index twice.
        if (const Place place = PlaceOf(id); place.readable()) {
          return Coerce(ReadPlace(place), TypeAt(place), type);
        }
        const InstId array = Value(expr.lhs);
        const InstId index = Value(expr.rhs);
        return builder_->Add(Opcode::kIndex, type, {array, index});
      }

      case ExprKind::kSelf:
        return self_.is_valid() ? self_ : builder_->Undef(type);

      case ExprKind::kField:
        return LowerField(id, expr, type);
      case ExprKind::kOptionalField:
        return LowerOptionalField(expr, type);

      case ExprKind::kCall:
        return LowerCall(id, expr, type);

      case ExprKind::kAwait:
        // `wait f(args)` is a call that suspends. The call itself is
        // lowered as usual and marked; the transform in `ir/tasks.h` turns
        // the mark into a state machine.
        return LowerWait(expr, type, Opcode::kWaitCall);

      case ExprKind::kSpawn:
        // `spawn f(args)` starts the work and produces a handle. Marked the
        // same way, expanded into a frame and a call to the scheduler.
        return LowerWait(expr, type, Opcode::kSpawnCall);

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

      case ExprKind::kFor:
      case ExprKind::kForeach:
        LowerForLoop(expr);
        return InstId{};

      case ExprKind::kUnsafe:
        // `unsafe` does not change the value; it is checked, not lowered.
        return Value(expr.lhs);

      case ExprKind::kRegion:
        return LowerArena(expr, type);

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
    // text and concatenating. The conversions are calls into the runtime
    // -- the floor the language stands on -- and both tiers make the same
    // ones.
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
      result = result.is_valid() ? ConcatText(result, text) : text;
    }
    return result.is_valid() ? result
                             : builder_->ConstString(str, std::string());
  }

  // Calls a host-provided function by name, declaring it if this file has
  // not named it yet.
  // A call into the runtime: the floor the language stands on, reached the
  // same way an `extern` declaration is. Which is what makes a string work
  // in both tiers -- the interpreter calls the same C function the machine
  // code does, rather than having an idea of its own about strings.
  InstId RuntimeCall(std::string_view symbol,
                     TypeId result,
                     std::vector<InstId> arguments) {
    const Symbol name = interner_->Intern(symbol);
    std::vector<TypeId> params;
    const Function& current = module_->functions[current_function_];
    for (InstId argument : arguments) {
      params.push_back(current.Contains(argument)
                           ? current.Get(argument).type
                           : types_->error());
    }
    const uint32_t callee = DeclareRuntime(name,
                                           types_->Function(params, result));
    return builder_->Call(callee, result, std::move(arguments));
  }

  // The runtime takes a 64-bit value, so a narrower one is converted
  // first: the C boundary has no idea what width the source said.
  InstId Widen(InstId value, TypeId to) {
    const Function& current = module_->functions[current_function_];
    if (!current.Contains(value) || current.Get(value).type == to) {
      return value;
    }
    return builder_->Add(Opcode::kCast, to, {value});
  }

  uint32_t DeclareRuntime(Symbol name, TypeId type) {
    for (uint32_t i = 0; i < module_->functions.size(); ++i) {
      if (module_->functions[i].name == name) {
        return i;
      }
    }
    Function declaration;
    declaration.name = name;
    declaration.type = Erase(type);
    // The symbol is its own name: the runtime table is consulted before
    // the host's loader, so these cannot be shadowed.
    declaration.foreign_symbol = name;
    module_->functions.push_back(std::move(declaration));
    return static_cast<uint32_t>(module_->functions.size() - 1);
  }

  // Two strings joined. The pieces are pairs, so the call takes four
  // arguments and gives back a string that owns its bytes.
  InstId ConcatText(InstId left, InstId right) {
    const TypeId bytes = types_->Pointer(
        types_->Int(IntWidth::k8, /*is_signed=*/false),
        /*is_mutable=*/false);
    const TypeId word = types_->Int(IntWidth::k64, /*is_signed=*/false);
    return TextFromRuntime(
        "wolang_text_concat",
        {builder_->Extract(bytes, left, 0),
         builder_->Extract(word, left, 1),
         builder_->Extract(bytes, right, 0),
         builder_->Extract(word, right, 1)});
  }

  // Whether two strings hold the same bytes.
  InstId TextEqual(InstId left, InstId right) {
    const TypeId bytes = types_->Pointer(
        types_->Int(IntWidth::k8, /*is_signed=*/false),
        /*is_mutable=*/false);
    const TypeId word = types_->Int(IntWidth::k64, /*is_signed=*/false);
    return RuntimeCall("wolang_text_equal", types_->bool_type(),
                       {builder_->Extract(bytes, left, 0),
                        builder_->Extract(word, left, 1),
                        builder_->Extract(bytes, right, 0),
                        builder_->Extract(word, right, 1)});
  }

  // A string the runtime built: one call, then the pair read out of what
  // it returned. The pointer is what crosses the boundary, because a
  // sixteen-byte value cannot be returned through the C ABI this tier
  // speaks.
  InstId TextFromRuntime(std::string_view symbol,
                         std::vector<InstId> arguments) {
    const TypeId bytes = types_->Pointer(
        types_->Int(IntWidth::k8, /*is_signed=*/false),
        /*is_mutable=*/false);
    const TypeId word = types_->Int(IntWidth::k64, /*is_signed=*/false);
    // A pointer to the pair itself, so that the field offsets are the
    // ones a string has: pointing at a word instead would put the length
    // at offset zero and read the bytes as a length.
    const TypeId handle =
        types_->Pointer(types_->str(), /*is_mutable=*/true);
    const InstId pair =
        RuntimeCall(symbol, handle, std::move(arguments));
    const InstId data_address =
        builder_->FieldAddress(types_->Pointer(bytes, true), pair, 0);
    const InstId length_address =
        builder_->FieldAddress(types_->Pointer(word, true), pair, 1);
    const InstId data = builder_->Add(Opcode::kLoad, bytes, {data_address});
    const InstId length =
        builder_->Add(Opcode::kLoad, word, {length_address});
    return builder_->Add(Opcode::kMakeStruct, types_->str(),
                         {data, length});
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
        return TextFromRuntime("wolang_text_from_int",
                               {Widen(value, types_->Int(IntWidth::k64,
                                                         true))});
      case TypeKind::kFloat:
        return TextFromRuntime("wolang_text_from_float",
                               {Widen(value,
                                      types_->Float(FloatWidth::k64))});
      case TypeKind::kBool:
        return TextFromRuntime("wolang_text_from_bool", {value});
      default:
        // Anything else needs a formatting trait, which arrives with the
        // rest of `core`.
        return builder_->ConstString(str, "<?>");
    }
  }

  InstId LowerPath(const Expr& expr, TypeId type) {
    const Local* local = Find(expr.name);
    if (local == nullptr) {
      // A field of the receiver, read from it.
      if (const Place place = SelfPlace(expr.name); place.readable()) {
        return Coerce(ReadPlace(place), TypeAt(place), type);
      }
      // A function used as a value: a callback, a handler, a comparator.
      if (const InstId function = FunctionValue(expr.name, type);
          function.is_valid()) {
        return function;
      }
      // A module-level constant, read through the function it became.
      if (const InstId constant = ConstantValue(expr.name, type);
          constant.is_valid()) {
        return constant;
      }
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
    // Two strings are equal when they hold the same bytes. A pair of
    // pointers is not an answer to that, so this is a call -- and
    // comparing the pairs, which is what it used to do, said that two
    // copies of "GET" were different strings.
    if ((opcode == Opcode::kEq || opcode == Opcode::kNe) &&
        IsView(TypeOf(expr.lhs)) && IsView(TypeOf(expr.rhs))) {
      const InstId equal = TextEqual(left, right);
      return opcode == Opcode::kEq
                 ? equal
                 : builder_->Add(Opcode::kLogicalNot, types_->bool_type(),
                                 {equal});
    }
    // `p + n` on a raw pointer: the address of the nth element from there.
    // An element address rather than an add, because only the tiers know
    // how big an element is -- and the length a bounds check would use is
    // exactly what a raw pointer does not have, so it is unbounded.
    if ((opcode == Opcode::kAdd || opcode == Opcode::kSub) &&
        types_->KindOf(Erase(TypeOf(expr.lhs))) == TypeKind::kPointer) {
      const TypeId word = types_->Int(IntWidth::k64, /*is_signed=*/false);
      InstId offset = right;
      if (opcode == Opcode::kSub) {
        offset = builder_->Add(Opcode::kNeg, TypeOf(expr.rhs), {right});
      }
      return builder_->IndexAddress(Erase(type), left, offset,
                                    builder_->ConstInt(word, ~uint64_t{0}));
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
      if (!HasStorage(member.kind)) {
        continue;
      }
      if (member.name == name) {
        return index;
      }
      ++index;
    }
    return 0;
  }

  // How many elements a sequence has. Static for an array, and the second
  // half of the pair for a view -- a slice or a borrowed string is a
  // pointer and a length, so the length is where it says it is.
  InstId LengthOf(ExprId sequence, TypeId sequence_type) {
    const TypeId counted = Erase(sequence_type);
    const TypeId length_type = types_->default_int();
    if (types_->KindOf(counted) == TypeKind::kArray) {
      return builder_->ConstInt(
          length_type,
          static_cast<uint64_t>(types_->Get(counted).length));
    }
    const TypeId raw = types_->Int(IntWidth::k64, /*is_signed=*/false);
    const InstId value = Value(sequence);
    const InstId length = builder_->Extract(raw, value, 1);
    return builder_->Add(Opcode::kCast, length_type, {length});
  }

  bool IsView(TypeId type) {
    const TypeKind kind = types_->KindOf(Erase(type));
    return kind == TypeKind::kSlice || kind == TypeKind::kStr ||
           kind == TypeKind::kString;
  }

  InstId LowerField(ExprId id, const Expr& expr, TypeId type) {
    // A derived field is computed rather than stored, and kept once it is.
    const TypeId base_type = Erase(TypeOf(expr.lhs));
    if (const Member* member =
            resolution_.FindMember(base_type, expr.name);
        member != nullptr && member->kind == MemberKind::kDerived) {
      Place object;
      if (types_->IsHandle(base_type)) {
        object.pointer = Value(expr.lhs);
        object.root_type = base_type;
      } else {
        object = PlaceOf(expr.lhs);
        if (!object.readable()) {
          object.value = Value(expr.lhs);
          object.root_type = base_type;
        }
      }
      return LowerDerived(object, base_type, *member, type);
    }

    // The two halves of a view, which are its two fields.
    if (interner_->Text(expr.name) == "data" &&
        IsView(TypeOf(expr.lhs))) {
      return builder_->Extract(Erase(type), Value(expr.lhs), 0);
    }
    if (interner_->Text(expr.name) == "len") {
      const TypeId owner = Erase(TypeOf(expr.lhs));
      if (types_->KindOf(owner) == TypeKind::kArray || IsView(owner)) {
        return Coerce(LengthOf(expr.lhs, owner), types_->default_int(),
                      type);
      }
    }
    // Through an address when there is one, so that the field is read
    // where it lives rather than out of a copy of the whole aggregate.
    // A place that is only readable reads out of the value instead, and
    // either way the base is lowered once.
    if (const Place place = PlaceOf(id); place.readable()) {
      return Coerce(ReadPlace(place), TypeAt(place), type);
    }
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

  // The address of a place, for `&x`. Whatever has an address gives its
  // own -- a local's slot, a field of one, an element of a buffer --
  // because `&buf[0]` has to point *into* the buffer and not at a copy of
  // it. Anything else is copied into a fresh slot, which is what makes
  // taking a reference to a temporary mean something. References are
  // second class (`E0120`), so the slot cannot outlive its frame.
  InstId Address(ExprId place, InstId value) {
    if (const Place at = PlaceOf(place); at.addressable()) {
      return AddressOfPlace(at);
    }
    if (!value.is_valid()) {
      return value;
    }
    const InstId slot = builder_->Add(Opcode::kAlloca, TypeOf(place));
    builder_->Add(Opcode::kStore, TypeId{}, {slot, value});
    return slot;
  }

  // Where a value lives.
  //
  // A root and a path of steps from it. A step is a field, or an element
  // whose index is a value -- which is the difference between what the
  // layout knows and what only the run knows.
  //
  // The root is a slot, a pointer, or a plain value. The first two are
  // addresses, so a place rooted in one is read and written where it is;
  // the third -- a parameter, a method's receiver on a value type -- can
  // only be read, and the checker reports an assignment to it (`E0125`).
  struct Step {
    uint32_t field = 0;
    // Valid for an element: the index, as a value, and how many there are.
    InstId index;
    InstId length;
    TypeId type;

    bool index_is_element() const { return index.is_valid(); }
  };

  struct Place {
    InstId slot;
    InstId pointer;
    InstId value;
    std::vector<Step> path;
    TypeId root_type;

    bool readable() const {
      return slot.is_valid() || pointer.is_valid() || value.is_valid();
    }
    bool addressable() const {
      return slot.is_valid() || pointer.is_valid();
    }
  };

  Place PlaceOf(ExprId id) {
    Place place;
    if (!ast_.Contains(id)) {
      return place;
    }
    const Expr& expr = ast_.Get(id);
    switch (expr.kind) {
      case ExprKind::kPath: {
        if (const Local* local = Find(expr.name); local != nullptr) {
          place.slot = local->slot;
          place.value = local->value;
          place.root_type = local->type;
          return place;
        }
        // Inside a method a bare name may be a field of the enclosing
        // type: `health` means `self.health`, which is what section 4.4 of
        // the spec promises and what gameplay code is written in.
        return SelfPlace(expr.name);
      }
      case ExprKind::kSelf:
        return SelfPlace(Symbol{});
      case ExprKind::kUnary: {
        if (expr.op != TokenKind::kStar) {
          return place;
        }
        place.pointer = Value(expr.lhs);
        place.root_type = Erase(TypeOf(id));
        return place;
      }
      case ExprKind::kField: {
        const TypeId owner = Erase(TypeOf(expr.lhs));
        if (types_->IsHandle(owner)) {
          // The base is a class, so it is already an address: the field is
          // read and written through it rather than through a copy.
          place.pointer = Value(expr.lhs);
          place.root_type = owner;
          place.path.push_back(Step{FieldIndex(owner, expr.name), InstId{},
                                    InstId{}, Erase(TypeOf(id))});
          return place;
        }
        Place base = PlaceOf(expr.lhs);
        if (!base.readable()) {
          return place;
        }
        base.path.push_back(Step{FieldIndex(TypeAt(base), expr.name),
                                 InstId{}, InstId{}, Erase(TypeOf(id))});
        return base;
      }
      case ExprKind::kIndex: {
        const TypeId sequence = Erase(TypeOf(expr.lhs));
        if (IsView(sequence)) {
          // A view is a pointer and a length, so indexing one starts from
          // the pointer rather than from where the view itself lives.
          const TypeId element =
              types_->KindOf(sequence) == TypeKind::kSlice
                  ? Erase(types_->ElementOf(sequence))
                  : types_->Int(IntWidth::k8, /*is_signed=*/false);
          place.pointer = builder_->Extract(
              types_->Pointer(element, /*is_mutable=*/true),
              Value(expr.lhs), 0);
          place.root_type = element;
          place.path.push_back(Step{0, Value(expr.rhs),
                                    LengthOf(expr.lhs, sequence), element});
          return place;
        }
        Place base = PlaceOf(expr.lhs);
        if (!base.readable()) {
          return place;
        }
        base.path.push_back(Step{0, Value(expr.rhs),
                                 LengthOf(expr.lhs, sequence),
                                 Erase(TypeOf(id))});
        return base;
      }
      default:
        return place;
    }
  }

  // The receiver, or the field of it that a bare name stands for. An
  // invalid name asks for the receiver itself.
  Place SelfPlace(Symbol name) {
    Place place;
    if (!self_.is_valid() || !self_type_.is_valid()) {
      return place;
    }
    place.root_type = self_type_;
    if (types_->IsHandle(self_type_)) {
      // A class receiver is the object's address, so a method of one
      // writes to the object.
      place.pointer = self_;
    } else {
      place.value = self_;
    }
    if (!name.is_valid()) {
      return place;
    }
    const Member* member = resolution_.FindMember(self_type_, name);
    if (member == nullptr || !HasStorage(member->kind)) {
      return Place{};
    }
    place.path.push_back(Step{FieldIndex(self_type_, name), InstId{},
                              InstId{}, Erase(member->type)});
    return place;
  }

  // The type at the end of a place's path.
  TypeId TypeAt(const Place& place) {
    return place.path.empty() ? Erase(place.root_type)
                              : Erase(place.path.back().type);
  }

  TypeId FieldTypeAt(TypeId owner, uint32_t field) {
    uint32_t index = 0;
    for (const Member& member : resolution_.MembersOf(owner)) {
      if (!HasStorage(member.kind)) {
        continue;
      }
      if (index == field) {
        return member.type;
      }
      ++index;
    }
    // A tuple, a string or an optional: built in rather than declared, so
    // its parts are the type's own arguments.
    const std::vector<TypeId> parts = types_->ArgumentsOf(owner);
    return field < parts.size() ? parts[field] : types_->error();
  }

  // The address of a place, walking the path one step at a time. Only for
  // a place with an address to begin with.
  InstId AddressOfPlace(const Place& place) {
    InstId address = place.slot.is_valid() ? place.slot : place.pointer;
    for (const Step& step : place.path) {
      const TypeId pointer = types_->Pointer(Erase(step.type), true);
      address = step.index.is_valid()
                    ? builder_->IndexAddress(pointer, address, step.index,
                                             step.length)
                    : builder_->FieldAddress(pointer, address, step.field);
    }
    return address;
  }

  // Reads what a place holds.
  InstId ReadPlace(const Place& place) {
    if (place.addressable()) {
      if (place.path.empty() && place.slot.is_valid()) {
        return builder_->Add(Opcode::kLoad, Erase(place.root_type),
                             {place.slot});
      }
      return builder_->Add(Opcode::kLoad, TypeAt(place),
                           {AddressOfPlace(place)});
    }
    // A value with no location: the fields are read out of it.
    InstId value = place.value;
    for (const Step& step : place.path) {
      value = step.index.is_valid()
                  ? builder_->Add(Opcode::kIndex, Erase(step.type),
                                  {value, step.index})
                  : builder_->Extract(Erase(step.type), value, step.field);
    }
    return value;
  }

  // Writes |value| into a place, where it lives. Nothing is copied and
  // nothing is rebuilt: one `store` through the address the path works out,
  // which is what makes filling a buffer a write per element rather than a
  // copy per element.
  void WritePlace(const Place& place, InstId value) {
    if (!value.is_valid() || !place.addressable()) {
      return;
    }
    builder_->Add(Opcode::kStore, TypeId{},
                  {AddressOfPlace(place), value});
    InvalidateDerived(place);
  }

  // A write to a field makes every derived field that reads it stale.
  //
  // Which ones those are comes from the member table rather than from this
  // file, because the type may be declared in another one -- a write here
  // has to invalidate a derived field whose expression it has never seen.
  void InvalidateDerived(const Place& place) {
    if (place.path.empty() || place.path.back().index_is_element()) {
      return;
    }
    const TypeId owner = OwnerOfLastStep(place);
    if (!owner.is_valid()) {
      return;
    }
    const std::span<const Member> members = resolution_.MembersOf(owner);
    if (members.empty()) {
      return;
    }
    const uint32_t written = place.path.back().field;

    InstId base;
    for (const Member& member : members) {
      if (member.kind != MemberKind::kDerived) {
        continue;
      }
      if (std::find(member.depends_on.begin(), member.depends_on.end(),
                    StorageIndexOf(owner, written)) ==
          member.depends_on.end()) {
        continue;
      }
      if (!base.is_valid()) {
        Place holder = place;
        holder.path.pop_back();
        base = holder.path.empty() && !holder.pointer.is_valid()
                   ? holder.slot
                   : AddressOfPlace(holder);
      }
      if (!base.is_valid()) {
        return;
      }
      const InstId bit = builder_->FieldAddress(
          types_->Pointer(types_->bool_type(), true), base,
          ReadyIndex(owner, member));
      builder_->Add(Opcode::kStore, TypeId{},
                    {bit, builder_->ConstBool(false)});
    }
  }

  // The type whose field the last step of |place| names.
  TypeId OwnerOfLastStep(const Place& place) {
    TypeId at = Erase(place.root_type);
    for (size_t i = 0; i + 1 < place.path.size(); ++i) {
      at = Erase(place.path[i].type);
    }
    return at;
  }

  // Which member a storage index refers to, as an index into the member
  // table: the dependency list is in table terms and a field address is in
  // storage terms, and only the fields in between differ.
  uint32_t StorageIndexOf(TypeId owner, uint32_t storage) {
    uint32_t seen = 0;
    const std::span<const Member> members = resolution_.MembersOf(owner);
    for (uint32_t i = 0; i < members.size(); ++i) {
      if (!HasStorage(members[i].kind)) {
        continue;
      }
      if (seen == storage) {
        return i;
      }
      ++seen;
    }
    return 0xFFFFFFFFu;
  }

  InstId LowerAssign(const Expr& expr) {
    const InstId value = Value(expr.rhs);
    WritePlace(PlaceOf(expr.lhs), value);
    // An assignment is a statement, so it produces nothing a caller can use.
    return InstId{};
  }

  InstId LowerWait(const Expr& expr, TypeId type, Opcode mark) {
    if (!ast_.Contains(expr.lhs) ||
        ast_.Get(expr.lhs).kind != ExprKind::kCall) {
      // The checker reports a `wait` on anything but a call (`E0303`).
      return builder_->Undef(type);
    }
    const InstId call = Value(expr.lhs);
    if (call.is_valid() &&
        module_->functions[current_function_].Get(call).opcode ==
            Opcode::kCall) {
      Inst& inst = module_->functions[current_function_].Get(call);
      inst.opcode = mark;
      if (mark == Opcode::kSpawnCall) {
        // A handle, not the task's result: the work has only started.
        inst.type = type;
      }
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
                              ? Coerce(value, TypeOf(written[i]), params[i],
                                       written[i])
                              : value);
    }

    // `s.Connect(handler)` and `s.Emit(args)`: a signal's two operations.
    if (const InstId done = LowerSignalCall(expr, arguments);
        done.is_valid()) {
      return done;
    }

    // `BytesFromRaw(data, length)` is not a call: a slice *is* a pointer
    // and a length, so saying so is the whole of it. The one unsafe way
    // to make a view of memory that came from outside the language --
    // `std.mem` declares it with no body and this is where it is kept.
    if (const InstId view = LowerBytesFromRaw(expr, type, arguments);
        view.is_valid()) {
      return view;
    }

    // `receiver.Method(args)`, and `Method(args)` inside a method -- which
    // is the same call with the receiver left implicit.
    if (const InstId called = LowerMethodCall(expr, type, &arguments);
        called.is_valid()) {
      return called;
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
        // is the object, and the wrap is what bridges the two.
        const TypeId built = Erase(binding->type);
        if (types_->IsHandle(built)) {
          return Coerce(NewObject(built, arguments), built, type);
        }
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
                            binding->item, binding->is_task,
                            binding->has_body),
            type, std::move(arguments));
      }
    }
    // Through a value: the callee is whatever the expression evaluated to.
    if (types_->KindOf(callee_type) == TypeKind::kFunction) {
      const InstId target = Value(expr.lhs);
      if (target.is_valid()) {
        std::vector<InstId> operands;
        operands.push_back(target);
        for (InstId argument : arguments) {
          operands.push_back(argument);
        }
        return builder_->Add(Opcode::kCallIndirect, type,
                             std::move(operands));
      }
    }
    return builder_->Undef(type);
  }

  // A function named where a value is wanted. Not a call: the value is the
  // function itself, which is what makes a router, a comparator and a
  // signal's subscriber possible.
  InstId FunctionValue(Symbol name, TypeId type) {
    if (types_->KindOf(type) != TypeKind::kFunction) {
      return InstId{};
    }
    uint32_t callee = IndexOfName(name);
    if (callee == 0xFFFFFFFFu) {
      const Binding* binding = resolution_.Find(name);
      if (binding == nullptr || binding->kind != BindingKind::kFunction) {
        return InstId{};
      }
      callee = DeclareExternal(name, binding->type, binding->file,
                               binding->item, binding->is_task,
                               binding->has_body);
    }
    const InstId value = builder_->Add(Opcode::kFuncAddress, type);
    module_->functions[current_function_].Get(value).payload.callee = callee;
    return value;
  }

  // Reading a derived field: compute it if the cache is not current, keep
  // the answer, and hand back the cache. "Recomputes when an input
  // changes rather than on every read" is a claim about *when*, and this
  // is the when (D63).
  //
  // A value with no address has nowhere to keep a cache, so there the
  // computation happens on every read -- the same answer, paid for more
  // often.
  InstId LowerDerived(const Place& object,
                      TypeId owner,
                      const Member& member,
                      TypeId type) {
    const uint32_t compute = DerivedCompute(owner, member);
    if (compute == 0xFFFFFFFFu) {
      return builder_->Undef(type);
    }
    const InstId receiver = object.addressable()
                                ? AddressOfPlace(object)
                                : object.value;
    if (!receiver.is_valid()) {
      return builder_->Undef(type);
    }
    if (!object.addressable()) {
      return builder_->Call(compute, Erase(member.type), {receiver});
    }

    const uint32_t cache_field = FieldIndex(owner, member.name);
    const uint32_t ready_field = ReadyIndex(owner, member);
    const TypeId cached = Erase(member.type);
    const InstId ready_address = builder_->FieldAddress(
        types_->Pointer(types_->bool_type(), true), receiver, ready_field);
    const InstId cache_address = builder_->FieldAddress(
        types_->Pointer(cached, true), receiver, cache_field);

    const BlockId stale = builder_->AddBlock("derived.stale");
    const BlockId join = builder_->AddBlock("derived.join");
    const InstId ready =
        builder_->Add(Opcode::kLoad, types_->bool_type(), {ready_address});
    builder_->Branch(ready, join, stale);

    builder_->SetCurrent(stale);
    const InstId value =
        builder_->Call(compute, cached, {receiver});
    builder_->Add(Opcode::kStore, TypeId{}, {cache_address, value});
    builder_->Add(Opcode::kStore, TypeId{},
                  {ready_address, builder_->ConstBool(true)});
    builder_->Jump(join);

    builder_->SetCurrent(join);
    return Coerce(builder_->Add(Opcode::kLoad, cached, {cache_address}),
                  cached, type);
  }

  // Which function computes |member|, declaring it when it is in another
  // file.
  uint32_t DerivedCompute(TypeId owner, const Member& member) {
    const uint32_t local = IndexOfSource(member.file, member.item);
    if (local != 0xFFFFFFFFu) {
      return local;
    }
    if (member.file == file_index_) {
      return 0xFFFFFFFFu;
    }
    return DeclareExternal(DerivedName(owner, member.name),
                           types_->Function({owner}, Erase(member.type)),
                           member.file, member.item);
  }

  // Where the bit for |member| sits among the type's storage.
  uint32_t ReadyIndex(TypeId owner, const Member& member) {
    uint32_t index = 0;
    for (const Member& candidate : resolution_.MembersOf(owner)) {
      if (!HasStorage(candidate.kind)) {
        continue;
      }
      if (candidate.kind == MemberKind::kReadyBit &&
          candidate.item == member.item && candidate.file == member.file) {
        return index;
      }
      ++index;
    }
    return 0;
  }

  // A module-level constant: a call to the function it became, which the
  // inliner turns back into the value.
  InstId ConstantValue(Symbol name, TypeId type) {
    for (const auto& [constant, index] : constants_) {
      if (constant == name) {
        return Coerce(builder_->Call(static_cast<uint32_t>(index),
                                     module_->functions[index].type.is_valid()
                                         ? types_->ElementOf(
                                               module_->functions[index].type)
                                         : type,
                                     {}),
                      types_->ElementOf(module_->functions[index].type),
                      type);
      }
    }
    const Binding* binding = resolution_.Find(name);
    if (binding == nullptr || binding->kind != BindingKind::kValue ||
        !binding->type.is_valid()) {
      return InstId{};
    }
    // Declared in another file: the same call, resolved by the linker.
    const TypeId signature = types_->Function({}, Erase(binding->type));
    const uint32_t callee =
        DeclareExternal(ConstantName(name), signature, binding->file,
                        binding->item);
    return Coerce(builder_->Call(callee, Erase(binding->type), {}),
                  Erase(binding->type), type);
  }

  // The view `std.mem.BytesFromRaw` stands for, when that is what is
  // being called: a declaration with no body, of that name, taking an
  // address and a count and giving back a slice.
  InstId LowerBytesFromRaw(const Expr& expr,
                           TypeId type,
                           const std::vector<InstId>& arguments) {
    if (arguments.size() != 2 || !ast_.Contains(expr.lhs) ||
        ast_.Get(expr.lhs).kind != ExprKind::kPath) {
      return InstId{};
    }
    if (interner_->Text(ast_.Get(expr.lhs).name) != "BytesFromRaw") {
      return InstId{};
    }
    const TypeId slice = Erase(type);
    if (types_->KindOf(slice) != TypeKind::kSlice) {
      return InstId{};
    }
    const Binding* binding = resolution_.Find(ast_.Get(expr.lhs).name);
    if (binding != nullptr && binding->has_body) {
      // Somebody wrote their own function of that name, and theirs wins.
      return InstId{};
    }
    const TypeId word = types_->Int(IntWidth::k64, /*is_signed=*/false);
    return builder_->Add(
        Opcode::kMakeStruct, slice,
        {arguments[0], Widen(arguments[1], word)});
  }

  // What a signal's `Connect`, `Disconnect` or `Emit` becomes.
  //
  // Connecting is a call into the runtime, because growing a list is the
  // one part that allocates. Emitting is *not*: calling back into the
  // language is something only the language can do, so the loop over the
  // handlers is emitted here.
  InstId LowerSignalCall(const Expr& expr,
                         const std::vector<InstId>& arguments) {
    if (!ast_.Contains(expr.lhs)) {
      return InstId{};
    }
    const Expr& callee = ast_.Get(expr.lhs);
    if (callee.kind != ExprKind::kField) {
      return InstId{};
    }
    const std::string_view operation = interner_->Text(callee.name);
    const bool connect = operation == "Connect";
    const bool disconnect = operation == "Disconnect";
    const bool emit = operation == "Emit";
    if (!connect && !disconnect && !emit) {
      return InstId{};
    }

    // The signal itself: `p.Died` in `p.Died.Emit(...)`, or a bare `Died`
    // inside a method.
    const Place list = SignalList(callee.lhs);
    if (!list.pointer.is_valid()) {
      return InstId{};
    }
    const InstId address = AddressOfPlace(list);
    const TypeId word = types_->Int(IntWidth::k64, /*is_signed=*/false);
    const TypeId handler_pointer = types_->Pointer(
        types_->Int(IntWidth::k8, /*is_signed=*/false),
        /*is_mutable=*/true);

    if (connect || disconnect) {
      if (arguments.empty()) {
        return InstId{};
      }
      return RuntimeCall(connect ? "wolang_signal_connect"
                                 : "wolang_signal_disconnect",
                         types_->void_type(), {address, arguments[0]});
    }

    // Emit: every handler, in the order they were connected.
    const InstId count_address = builder_->FieldAddress(
        types_->Pointer(word, true), address, 0);
    const InstId handlers_address = builder_->FieldAddress(
        types_->Pointer(handler_pointer, true), address, 2);
    const InstId count =
        builder_->Add(Opcode::kLoad, word, {count_address});
    const InstId handlers =
        builder_->Add(Opcode::kLoad, handler_pointer, {handlers_address});

    const TypeId handler = Erase(TypeOf(callee.lhs));
    const InstId index = builder_->Add(Opcode::kAlloca, word);
    builder_->Add(Opcode::kStore, TypeId{},
                  {index, builder_->ConstInt(word, 0)});

    const BlockId header = builder_->AddBlock("emit.header");
    const BlockId body = builder_->AddBlock("emit.body");
    const BlockId exit = builder_->AddBlock("emit.exit");
    builder_->Jump(header);

    builder_->SetCurrent(header);
    const InstId current = builder_->Add(Opcode::kLoad, word, {index});
    const InstId more =
        builder_->Add(Opcode::kLt, types_->bool_type(), {current, count});
    builder_->Branch(more, body, exit);

    builder_->SetCurrent(body);
    const InstId slot = builder_->IndexAddress(
        types_->Pointer(handler, true), handlers, current, count);
    const InstId target = builder_->Add(Opcode::kLoad, handler, {slot});
    std::vector<InstId> operands;
    operands.push_back(target);
    for (InstId argument : arguments) {
      operands.push_back(argument);
    }
    builder_->Add(Opcode::kCallIndirect, types_->void_type(),
                  std::move(operands));
    const InstId next = builder_->Add(
        Opcode::kAdd, word, {current, builder_->ConstInt(word, 1)});
    builder_->Add(Opcode::kStore, TypeId{}, {index, next});
    builder_->Jump(header);

    builder_->SetCurrent(exit);
    // An emit is a statement; what it produces is nothing.
    return builder_->Add(Opcode::kLoad, word, {count_address});
  }

  // The storage of the signal |id| names, as a place pointing at it.
  Place SignalList(ExprId id) {
    Place place;
    if (!ast_.Contains(id)) {
      return place;
    }
    const Expr& expr = ast_.Get(id);
    TypeId owner;
    Symbol name;
    InstId object;
    if (expr.kind == ExprKind::kField) {
      owner = Erase(TypeOf(expr.lhs));
      name = expr.name;
      if (types_->IsHandle(owner)) {
        object = Value(expr.lhs);
      } else if (const Place base = PlaceOf(expr.lhs);
                 base.addressable()) {
        object = AddressOfPlace(base);
      }
    } else if (expr.kind == ExprKind::kPath && self_.is_valid()) {
      owner = self_type_;
      name = expr.name;
      object = types_->IsHandle(self_type_) ? self_ : InstId{};
    }
    if (!owner.is_valid() || !object.is_valid()) {
      return place;
    }
    const Member* signal = resolution_.FindMember(owner, name);
    if (signal == nullptr || signal->kind != MemberKind::kSignal) {
      return place;
    }
    place.pointer = object;
    place.root_type = owner;
    place.path.push_back(Step{SignalListIndex(owner, *signal), InstId{},
                              InstId{}, SignalListType(owner, *signal)});
    return place;
  }

  uint32_t SignalListIndex(TypeId owner, const Member& signal) {
    uint32_t index = 0;
    for (const Member& candidate : resolution_.MembersOf(owner)) {
      if (!HasStorage(candidate.kind)) {
        continue;
      }
      if (candidate.kind == MemberKind::kSignalList &&
          candidate.item == signal.item &&
          candidate.file == signal.file) {
        return index;
      }
      ++index;
    }
    return 0;
  }

  TypeId SignalListType(TypeId owner, const Member& signal) {
    for (const Member& candidate : resolution_.MembersOf(owner)) {
      if (candidate.kind == MemberKind::kSignalList &&
          candidate.item == signal.item &&
          candidate.file == signal.file) {
        return Erase(candidate.type);
      }
    }
    return types_->error();
  }

  // A call to a method, when that is what this is. |arguments| holds the
  // written arguments; the receiver goes in front of them, as an address.
  InstId LowerMethodCall(const Expr& expr,
                         TypeId type,
                         std::vector<InstId>* arguments) {
    if (!ast_.Contains(expr.lhs)) {
      return InstId{};
    }
    const Expr& callee = ast_.Get(expr.lhs);

    TypeId owner;
    Symbol name;
    InstId receiver;
    if (callee.kind == ExprKind::kField) {
      owner = Erase(TypeOf(callee.lhs));
      name = callee.name;
      receiver = Value(callee.lhs);
    } else if (callee.kind == ExprKind::kPath && self_.is_valid()) {
      // A bare name inside a method: its own type's method, if it has one.
      owner = self_type_;
      name = callee.name;
      receiver = self_;
    } else {
      return InstId{};
    }
    if (!owner.is_valid() || !receiver.is_valid()) {
      return InstId{};
    }

    const Member* member = resolution_.FindMember(owner, name);
    if (member == nullptr || member->kind != MemberKind::kMethod) {
      return InstId{};
    }

    uint32_t index =
        IndexOfSource(member->file, member->item, member->slot);
    if (index == 0xFFFFFFFFu) {
      if (member->file == file_index_) {
        // Declared here and not lowered: a generic, or a method of a
        // generic type. The interpreter keeps those for now.
        return InstId{};
      }
      // Declared in another file. Lowering is per file, so it gets a
      // declaration here with the receiver in its signature, and the
      // linker resolves it by the declaration it came from.
      std::vector<TypeId> params;
      params.push_back(owner);
      for (TypeId param : types_->ArgumentsOf(member->type)) {
        params.push_back(Erase(param));
      }
      const TypeId signature = types_->Function(
          params, Erase(types_->ElementOf(member->type)));
      index = DeclareExternal(MethodName(owner, name), signature,
                              member->file, member->item,
                              /*is_task=*/false, /*has_body=*/false,
                              member->slot);
      Remember(member->file, member->item, index, member->slot);
    }

    std::vector<InstId> with_receiver;
    with_receiver.push_back(receiver);
    for (InstId argument : *arguments) {
      with_receiver.push_back(argument);
    }
    return builder_->Call(index, type, std::move(with_receiver));
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

  // `for i in a..b` and `for x in xs`.
  //
  // Both are a counting loop: a range counts between its bounds, and a
  // sequence counts its elements and reads one per turn. Written out here
  // rather than desugared in the parser, because the index is a slot the
  // loop owns and the binding is not -- a body cannot assign to it, and
  // the IR says so by never giving it an address.
  void LowerForLoop(const Expr& expr) {
    const TypeId sequence_type = Erase(TypeOf(expr.lhs));
    const bool over_range = ast_.Contains(expr.lhs) &&
                            ast_.Get(expr.lhs).kind == ExprKind::kRange;

    // What the index counts in, and what it counts to.
    InstId start;
    InstId limit;
    bool inclusive = false;
    Place sequence;
    TypeId element;
    if (over_range) {
      const Expr& range = ast_.Get(expr.lhs);
      inclusive = range.op == TokenKind::kDotDotEq;
      element = Erase(TypeOf(expr.lhs));
      if (!element.is_valid() ||
          types_->KindOf(element) != TypeKind::kInt) {
        element = types_->default_int();
      }
      start = range.lhs.is_valid() ? Coerce(Value(range.lhs),
                                            TypeOf(range.lhs), element)
                                   : builder_->ConstInt(element, 0);
      limit = range.rhs.is_valid() ? Coerce(Value(range.rhs),
                                            TypeOf(range.rhs), element)
                                   : builder_->ConstInt(element, 0);
    } else {
      element = Erase(types_->ElementOf(sequence_type));
      sequence = PlaceOf(expr.lhs);
      if (!sequence.readable()) {
        // A sequence that is not a place -- a temporary -- still has to
        // live somewhere to be indexed.
        sequence.slot = Address(expr.lhs, Value(expr.lhs));
        sequence.root_type = sequence_type;
      }
      start = builder_->ConstInt(types_->default_int(), 0);
      limit = LengthOf(expr.lhs, sequence_type);
    }

    const TypeId counter_type =
        over_range ? element : types_->default_int();
    const InstId index = builder_->Add(Opcode::kAlloca, counter_type);
    builder_->Add(Opcode::kStore, TypeId{}, {index, start});

    const BlockId header = builder_->AddBlock("for.header");
    const BlockId body = builder_->AddBlock("for.body");
    const BlockId step = builder_->AddBlock("for.step");
    const BlockId exit = builder_->AddBlock("for.exit");

    builder_->Jump(header);
    builder_->SetCurrent(header);
    const InstId current =
        builder_->Add(Opcode::kLoad, counter_type, {index});
    const InstId keep_going = builder_->Add(
        inclusive ? Opcode::kLe : Opcode::kLt, types_->bool_type(),
        {current, limit});
    builder_->Branch(keep_going, body, exit);

    // `continue` goes to the step rather than to the header, or the index
    // would never advance and the loop would not end.
    loops_.push_back(LoopTarget{expr.name, exit, step, scope_depth_ + 1});
    builder_->SetCurrent(body);
    const size_t depth = locals_.size();
    ++scope_depth_;

    InstId bound = current;
    TypeId bound_type = counter_type;
    if (!over_range) {
      Place at = sequence;
      if (IsView(sequence_type)) {
        // A view is a pointer and a length, so the elements are where the
        // pointer says and not where the view itself is.
        at = Place{};
        at.pointer = builder_->Extract(
            types_->Pointer(element, /*is_mutable=*/true),
            ReadPlace(sequence), 0);
        at.root_type = element;
      }
      at.path.push_back(Step{0, current, limit, element});
      bound = ReadPlace(at);
      bound_type = element;
    }
    if (ast_.Contains(expr.pattern)) {
      const Pattern& pattern = ast_.Get(expr.pattern);
      if (pattern.kind == PatternKind::kBinding) {
        locals_.push_back(
            Local{pattern.name, InstId{}, bound, bound_type});
      }
    }

    Lower(expr.rhs);
    RunDeferred(scope_depth_);
    ForgetDeferred(scope_depth_);
    --scope_depth_;
    locals_.resize(depth);
    if (!builder_->Terminated()) {
      builder_->Jump(step);
    }
    loops_.pop_back();

    builder_->SetCurrent(step);
    const InstId before =
        builder_->Add(Opcode::kLoad, counter_type, {index});
    const InstId one = builder_->ConstInt(counter_type, 1);
    const InstId next =
        builder_->Add(Opcode::kAdd, counter_type, {before, one});
    builder_->Add(Opcode::kStore, TypeId{}, {index, next});
    builder_->Jump(header);

    builder_->SetCurrent(exit);
  }

  // `arena { ... }`: everything allocated inside comes from one arena, and
  // the whole arena goes at the end. Which is the point of the block --
  // a handler allocates as it likes and pays once.
  //
  // The release has to happen on every way out, including a `return` from
  // inside the block, so it is a deferred statement in all but name. What
  // keeps a value from outliving the arena is `E0121`, which the checker
  // already enforces.
  InstId LowerArena(const Expr& expr, TypeId type) {
    RuntimeCall("wolang_arena_push", types_->void_type(), {});
    arenas_.push_back(scope_depth_ + 1);
    ++scope_depth_;
    const InstId value = Value(expr.lhs);
    --scope_depth_;
    arenas_.pop_back();
    if (!builder_->Terminated()) {
      RuntimeCall("wolang_arena_pop", types_->void_type(), {});
    }
    (void)type;
    return value;
  }

  // The arenas an early exit from |scope| leaves behind, innermost first.
  void ReleaseArenas(size_t scope) {
    for (size_t i = arenas_.size(); i > 0; --i) {
      if (arenas_[i - 1] < scope) {
        break;
      }
      if (builder_->Terminated()) {
        return;
      }
      RuntimeCall("wolang_arena_pop", types_->void_type(), {});
    }
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
  const uint32_t file_index_ = 0;
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
  // Module-level constants, by the name they were written with.
  std::vector<std::pair<Symbol, size_t>> constants_;
  std::vector<MachineBlock> machine_blocks_;
  std::vector<MachineMethod> machine_methods_;
  std::vector<Pending> pending_;
  std::vector<Deferred> deferred_;
  // Whether the function being lowered is a `task`.
  bool in_task_ = false;
  // How many blocks deep lowering is. A `defer` belongs to the block it was
  // written in, and the count of locals cannot say which block that is -- a
  // block that declares nothing does not change it.
  size_t scope_depth_ = 0;
  TypeStore::Substitution substitution_;
  // The scope depth of each open `arena` block, so that an exit from
  // inside one releases it.
  std::vector<size_t> arenas_;

  Builder* builder_ = nullptr;
  uint32_t current_function_ = 0;
  TypeId return_type_;
  // The type whose method is being lowered, and where its receiver is.
  TypeId self_type_;
  InstId self_;
  const Symbol self_name_;
  std::vector<Local> locals_;
  std::vector<LoopTarget> loops_;
  std::vector<std::pair<ItemId, size_t>> indices_;
  // Every function of this module by the declaration it came from, which is
  // how a method call finds the function: a method has no file-scope name
  // to look up.
  struct Source {
    uint32_t file = 0;
    uint32_t item = 0;
    uint32_t slot = 0;
    uint32_t index = 0;
  };
  std::vector<Source> by_source_;
};

}  // namespace

void LowerFile(StringInterner* interner,
               TypeStore* types,
               uint32_t file_index,
               const FileResolution& resolution,
               const Ast& ast,
               const TypedBody& typed,
               std::span<const std::pair<ItemId, EffectRow>> effects,
               Module* module) {
  Lowerer lowerer(interner, types, file_index, resolution, ast, typed,
                  effects, module);
  lowerer.Run();
}

}  // namespace ir
}  // namespace wolang
