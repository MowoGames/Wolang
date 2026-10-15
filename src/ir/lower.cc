// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/ir/lower.h"

#include <algorithm>
#include <cstdio>
#include <utility>

#include "wolang/base/diagnostic.h"
#include "wolang/compiler/item_tree.h"
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
  // Which file's declarations are being lowered *right now*.
  //
  // Usually this file's, and not while a generic from another module is
  // being instantiated: its body resolves its names in the file it was
  // written in, so lowering a copy of it means borrowing that file's
  // syntax, member tables and recorded types for the duration.
  struct Context {
    uint32_t file_index = 0;
    const FileResolution* resolution = nullptr;
    const Ast* ast = nullptr;
    const TypedBody* typed = nullptr;
    std::span<const std::pair<ItemId, EffectRow>> effects;
  };

  // A closure, waiting for its own function to be written.
  //
  // A closure is a function with no declaration: it is written where it
  // is used, and what it becomes is a function of the module with the
  // body the expression wrote (D106). Queued rather than lowered on the
  // spot, because lowering it in the middle of another function would
  // need two builders at once.
  struct PendingClosure {
    ExprId expr;
    uint32_t index = 0;
    TypeStore::Substitution substitution;
    TypeId owner;
    Context where;
    // What the closure took with it: the names, and the type of the
    // environment holding them. Empty for one that captured nothing
    // (D138).
    std::vector<Symbol> captures;
    TypeId environment;
  };


  Lowerer(StringInterner* interner,
          TypeStore* types,
          uint32_t file_index,
          const FileResolution& resolution,
          const Ast& ast,
          const TypedBody& typed,
          std::span<const std::pair<ItemId, EffectRow>> effects,
          ForeignLookup foreign,
          const AccessGraph* systems,
          Module* module)
      : interner_(interner),
        types_(types),
        here_(Context{file_index, &resolution, &ast, &typed, effects}),
        foreign_(std::move(foreign)),
        systems_(systems),
        module_(module),
        resolver_(types, interner, &resolution, &discarded_),
        self_name_(interner->Intern("self")) {}

  const Ast& ast() const { return *here_.ast; }
  const FileResolution& resolution() const { return *here_.resolution; }
  const TypedBody& typed() const { return *here_.typed; }
  uint32_t file_index() const { return here_.file_index; }

  void Run() {
    // Two passes: every function gets a declaration first, so a call can
    // name one that is defined later in the file.
    for (ItemId id : ast().Get(ast().root())) {
      Declare(id, TypeId{});
    }
    for (ItemId id : ast().Get(ast().root())) {
      Define(id, TypeId{});
    }

    // Everything a module-level `var` needs doing before anything runs.
    EmitFileInit();

    // A generic function has no code of its own; it gets one copy per
    // instantiation. Lowering a specialization can discover more of them --
    // a generic body calling another generic -- so this is a worklist
    // rather than a second pass.
    while (!closures_.empty() || !pending_.empty()) {
      while (!closures_.empty()) {
        const PendingClosure work = closures_.back();
        closures_.pop_back();
        LowerClosureBody(work);
      }
      if (pending_.empty()) {
        break;
      }
      const Pending work = pending_.back();
      pending_.pop_back();
      substitution_ = work.substitution;
      self_type_ = work.owner;
      // A generic from another module is lowered with that module's view:
      // its own names, its own member tables, its own recorded types.
      const Context outer = here_;
      if (work.where.ast != nullptr) {
        here_ = Context{work.where.file_index, work.where.resolution,
                        work.where.ast, work.where.typed,
                        work.where.effects};
        resolver_ = TypeResolver(types_, interner_, here_.resolution,
                                 &discarded_);
      }
      if (ast().Contains(work.item)) {
        LowerFunction(ast().Get(work.item), work.index);
      }
      if (work.where.ast != nullptr) {
        here_ = Context{outer.file_index, outer.resolution, outer.ast,
                        outer.typed, outer.effects};
        resolver_ = TypeResolver(types_, interner_, here_.resolution,
                                 &discarded_);
      }
      self_type_ = TypeId{};
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
    if (!ast().Contains(id)) {
      return;
    }
    const Item& item = ast().Get(id);
    switch (item.kind) {
      case ItemKind::kDeinit:
        // What the type does when a value of it goes away: a method in
        // everything but how it is reached (D87).
        if (owner.is_valid()) {
          DeclareMethod(id, item, owner);
        }
        break;

      case ItemKind::kFunction: {
        if (owner.is_valid()) {
          DeclareMethod(id, item, owner);
          break;
        }
        const Binding* binding = resolution().Find(item.name);
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
        function.is_comptime = HasFlag(item.flags, ItemFlags::kComptime);
        function.is_primitive_task =
            function.is_task && !item.body.is_valid();
        if (HasFlag(item.flags, ItemFlags::kExtern)) {
          // `extern "C" fun puts(...) = "puts"` names the symbol; without
          // the clause the symbol is the declaration's own name.
          function.foreign_symbol =
              item.link_name.is_valid() ? item.link_name : item.name;
          function.type = ForeignSignature(function.type);
        }
        Remember(function.source_file, function.source_item,
                 module_->functions.size());
        indices_.push_back(Declared{id, module_->functions.size(),
                                    ast().Get(id).name, file_index()});
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
        // A module-level `var` has storage: it can be written, so a
        // function that recomputes the value is the wrong shape for it
        // (D88). The initializer still becomes a function, which the
        // file's init calls once -- and a `var` with *no* initializer is
        // still storage, starting as its type's zero. Checking the
        // initializer first meant such a one became no global at all,
        // and reading it called a function with no body (D136).
        if (item.name.is_valid() && HasFlag(item.flags, ItemFlags::kMutable)) {
          DeclareGlobal(id, item);
          break;
        }
        if (!item.name.is_valid() || !item.init.is_valid()) {
          break;
        }
        const Binding* binding = resolution().Find(item.name);
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
        constants_.push_back(
            Named{item.name, module_->functions.size(), file_index()});
        indices_.push_back(Declared{id, module_->functions.size(),
                                    ast().Get(id).name, file_index()});
        module_->functions.push_back(std::move(function));
        break;
      }

      case ItemKind::kMachine:
        DeclareMachine(id, item);
        break;

      case ItemKind::kSystem: {
        // A system is a function of the world and the time step.
        //
        // Its queries are not parameters at run time: a `Query` says what
        // the system touches, and what it is *given* is the world to ask.
        // So the shape every system compiles to is the same one, which is
        // what lets a stage be an array of addresses (D101).
        Function function;
        function.name = item.name;
        function.type = types_->Function(
            {types_->u64(), types_->Float(FloatWidth::k32)},
            types_->void_type());
        function.effects = EffectsOf(id, &function.effects_known);
        function.source_file = file_index();
        function.source_item = IndexOf(id) == 0xFFFFFFFFu
                                   ? Function::kNoSource
                                   : function.source_item;
        if (const Binding* binding = resolution().Find(item.name);
            binding != nullptr) {
          function.source_file = binding->file;
          function.source_item = binding->item;
        }
        Remember(function.source_file, function.source_item,
                 module_->functions.size());
        indices_.push_back(Declared{id, module_->functions.size(),
                                    item.name, file_index()});
        module_->functions.push_back(std::move(function));
        break;
      }

      case ItemKind::kClass:
      case ItemKind::kStruct:
      case ItemKind::kComponent:
      case ItemKind::kEnum:
      case ItemKind::kExtend:
        // Methods are functions too; their bodies are lowered the same way.
        for (ItemId member : ast().Get(item.members)) {
          Declare(member, OwnerOf(item));
        }
        break;
      default:
        break;
    }
  }

  // What a declaration does, as the module-wide inference found it. An
  // absence is not an empty row: see `Function::here_.effectsknown`.
  EffectRow EffectsOf(ItemId id, bool* known) const {
    for (const auto& [item, row] : here_.effects) {
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
    // The list above is this file's. A body borrowed from another one --
    // a generic being instantiated here -- calls *its* file's generics,
    // which are not in it: `Remove<T>` calling `Get<T>` found a
    // declaration with no body behind it instead (D100).
    for (ItemId id : ast().Get(ast().root())) {
      if (!ast().Contains(id)) {
        continue;
      }
      const Item& item = ast().Get(id);
      if (item.kind != ItemKind::kFunction || item.name != name ||
          ast().Get(item.generics).empty()) {
        continue;
      }
      const Binding* binding = resolution().Find(name);
      if (binding != nullptr && binding->type.is_valid() &&
          types_->MentionsParameter(binding->type)) {
        return id;
      }
    }
    return ItemId{};
  }

  // Where a generic this file *imported* was written: the file it came
  // from, and the declaration in it.
  //
  // A copy of it is made here rather than there, because the module it was
  // written in cannot know what every importer will ask for. The copy
  // borrows the defining file's syntax and member tables for as long as it
  // takes to lower -- `Store<T>` resolves `SoaNew` in the file that wrote
  // it, not in the one that called it.
  struct Imported {
    uint32_t file = 0;
    ItemId item;
    const FileResolution* resolution = nullptr;
    const Ast* ast = nullptr;
    const TypedBody* typed = nullptr;
  };

  Imported FindImportedGeneric(Symbol name) const {
    const Binding* binding = resolution().Find(name);
    if (binding == nullptr || binding->kind != BindingKind::kFunction ||
        binding->file == file_index() || !foreign_ ||
        !types_->MentionsParameter(binding->type)) {
      return Imported{};
    }
    const ForeignFile other = foreign_(binding->file);
    if (other.ast == nullptr || other.resolution == nullptr ||
        other.typed == nullptr) {
      return Imported{};
    }
    const ItemId item = ResolveItem(*other.ast, binding->item);
    if (!other.ast->Contains(item)) {
      return Imported{};
    }
    return Imported{binding->file, item, other.resolution, other.ast,
                    other.typed};
  }

  const std::vector<TypeId>* FindInstantiation(ExprId call) const {
    for (const auto& [id, arguments] : typed().instantiations) {
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
    // The current file's view, for the same reason as above: a copy
    // queued now is lowered later, when the current file may differ.
    return Instantiate(item, call, here_);
  }

  uint32_t Instantiate(ItemId item, ExprId call, const Context& where) {
    // The arguments come from *this* file -- inference happened at the
    // call -- and the declaration from wherever it was written.
    const std::vector<TypeId>* arguments = FindInstantiation(call);
    const Ast& home = where.ast != nullptr ? *where.ast : ast();
    const FileResolution& names =
        where.resolution != nullptr ? *where.resolution : resolution();
    const Binding* binding = names.Find(home.Get(item).name);
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

    std::string mangled(interner_->Text(home.Get(item).name));
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
    // No source identity: a specialization is reached by the name it was
    // mangled with and never through the declaration it came from. Two
    // instantiations of one generic would otherwise claim the same (file,
    // item) pair, and a linker that resolves by that pair binds a call to
    // whichever copy it finds first -- which is a call into another type's
    // layout.
    function.source_file = Function::kNoSource;
    function.source_item = Function::kNoSource;
    function.is_task = HasFlag(home.Get(item).flags, ItemFlags::kAsync);
    function.is_comptime =
        HasFlag(home.Get(item).flags, ItemFlags::kComptime);
    const auto index = static_cast<uint32_t>(module_->functions.size());
    module_->functions.push_back(std::move(function));
    specializations_.emplace_back(name, index);
    pending_.push_back(Pending{item, substitution, index, TypeId{}, where});
    return index;
  }

  // One copy of a generic type's method per instantiation.
  //
  // `Box<int>.Get` and `Box<float>.Get` are two functions over one body.
  // The substitution comes from the receiver rather than from inference,
  // which is what makes this simpler than a generic function: by the time
  // a method is called, what its type stands for is already decided.
  // The type parameters a method declares for itself: the ones its
  // signature mentions which the owning type does not provide (D119).
  std::vector<TypeId> MethodParameters(TypeId owner, const Member& member) {
    std::vector<TypeId> mine;
    if (!member.type.is_valid()) {
      return mine;
    }
    const TypeId base = types_->KindOf(owner) == TypeKind::kInstance
                            ? types_->Get(owner).base
                            : owner;
    const std::span<const TypeId> owned = resolution().ParametersOf(base);
    for (const TypeId parameter : ParametersIn(member.type)) {
      bool from_owner = false;
      for (const TypeId held : owned) {
        from_owner |= held == parameter;
      }
      if (!from_owner) {
        mine.push_back(parameter);
      }
    }
    return mine;
  }

  uint32_t InstantiateMethod(TypeId owner,
                             Symbol name,
                             const Member& member,
                             ExprId call = ExprId{}) {
    const bool generic_owner = types_->KindOf(owner) == TypeKind::kInstance;
    const TypeId base = generic_owner ? types_->Get(owner).base : owner;
    const std::span<const TypeId> parameters = resolution().ParametersOf(base);
    const std::vector<TypeId> arguments =
        generic_owner ? types_->ArgumentsOf(owner) : std::vector<TypeId>();
    const std::vector<TypeId> mine = MethodParameters(owner, member);
    if (generic_owner &&
        (parameters.empty() || parameters.size() != arguments.size())) {
      return 0xFFFFFFFFu;
    }
    if (!generic_owner && mine.empty()) {
      return 0xFFFFFFFFu;
    }

    TypeStore::Substitution substitution;
    for (size_t i = 0; i < parameters.size() && i < arguments.size(); ++i) {
      const TypeId concrete = Erase(Apply(arguments[i]));
      if (!concrete.is_valid() || types_->MentionsParameter(concrete)) {
        return 0xFFFFFFFFu;
      }
      substitution.emplace_back(parameters[i], concrete);
    }
    // What the call said its own parameters are. Inference happened at
    // the call, so this is where the answer is -- and a method whose `T`
    // nothing bound is a call with nothing to copy for.
    if (!mine.empty()) {
      const std::vector<TypeId>* said = FindInstantiation(call);
      if (said == nullptr) {
        return 0xFFFFFFFFu;
      }
      const std::vector<TypeId> order = ParametersIn(member.type);
      size_t at = 0;
      for (const TypeId parameter : order) {
        if (at >= said->size()) {
          break;
        }
        const TypeId given = Erase(Apply((*said)[at]));
        ++at;
        bool wanted = false;
        for (const TypeId held : mine) {
          wanted |= held == parameter;
        }
        if (!wanted) {
          continue;
        }
        if (!given.is_valid() || types_->MentionsParameter(given)) {
          return 0xFFFFFFFFu;
        }
        substitution.emplace_back(parameter, given);
      }
      if (substitution.size() != parameters.size() + mine.size()) {
        return 0xFFFFFFFFu;
      }
    }

    // Where the body is: the member table says which file and which
    // declaration, which is all that is needed whether the type was
    // written here or imported from another module.
    // The context the body belongs to, always -- not only when it comes
    // from another file. A copy asked for *while* another file's body was
    // being lowered is queued, and by the time the queue reaches it the
    // current file is somebody else's again: without this the body was
    // looked for in the wrong syntax tree and the function came out
    // empty (D79).
    Context where = here_;
    if (member.file != file_index()) {
      if (!foreign_) {
        return 0xFFFFFFFFu;
      }
      const ForeignFile other = foreign_(member.file);
      if (other.ast == nullptr || other.resolution == nullptr ||
          other.typed == nullptr) {
        return 0xFFFFFFFFu;
      }
      where = Context{member.file, other.resolution, other.ast,
                      other.typed, {}};
    }
    const Ast& home = where.ast != nullptr ? *where.ast : ast();
    const ItemId body = ResolveItem(home, member.item);
    if (!home.Contains(body)) {
      return 0xFFFFFFFFu;
    }

    // The name says which copy this is: a method with type parameters of
    // its own has one per set of arguments, and `Box.Map` alone would
    // make `Map<int>` and `Map<text>` the same function (D119).
    std::string spelling(interner_->Text(MethodName(owner, name)));
    if (!mine.empty()) {
      spelling.push_back('<');
      bool first = true;
      for (const TypeId parameter : mine) {
        for (const auto& [written, value] : substitution) {
          if (written != parameter) {
            continue;
          }
          if (!first) {
            spelling.append(", ");
          }
          first = false;
          spelling.append(types_->Describe(*interner_, value));
        }
      }
      spelling.push_back('>');
    }
    const Symbol mangled = interner_->Intern(spelling);
    for (const auto& [existing, index] : specializations_) {
      if (existing == mangled) {
        return index;
      }
    }

    // The receiver first, the way every method takes it (D60).
    std::vector<TypeId> params;
    params.push_back(owner);
    for (TypeId param : types_->ArgumentsOf(member.type)) {
      params.push_back(Erase(types_->Substitute(param, substitution)));
    }

    Function function;
    function.name = mangled;
    function.type = types_->Function(
        params,
        Erase(types_->Substitute(types_->ElementOf(member.type),
                                 substitution)));
    // Reached by its mangled name alone, for the reason above.
    function.source_file = Function::kNoSource;
    function.source_item = Function::kNoSource;
    function.self_type = owner;
    const auto index = static_cast<uint32_t>(module_->functions.size());
    module_->functions.push_back(std::move(function));
    specializations_.emplace_back(mangled, index);
    pending_.push_back(Pending{body, substitution, index, owner, where});
    return index;
  }

  uint32_t IndexOf(ItemId id) const {
    for (const Declared& declared : indices_) {
      if (declared.item == id && declared.file == file_index()) {
        return static_cast<uint32_t>(declared.index);
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
                 ? Erase(resolver_.Resolve(ast(), item.target_type))
                 : TypeId{};
    }
    const Binding* binding = resolution().Find(item.name);
    return binding != nullptr ? Erase(binding->type) : TypeId{};
  }

  // A class is a handle, so constructing one allocates and then writes the
  // fields through the handle. The size is the tier's answer -- lowering
  // has no layout -- which is why `new` is an operation.
  InstId NewObject(TypeId type, const std::vector<InstId>& fields) {
    const InstId object = builder_->Add(Opcode::kNew, type);
    uint32_t index = 0;
    // The descriptor first, when the class has one: the token that says
    // which class this object really is, which is what every virtual call
    // on it reads (D85).
    if (HasClassHeader(type)) {
      const InstId token = ClassToken(type);
      const InstId address = builder_->FieldAddress(
          types_->Pointer(types_->u64(), /*is_mutable=*/true), object, 0);
      builder_->Add(Opcode::kStore, TypeId{}, {address, token});
      index = 1;
    }
    for (InstId value : fields) {
      const TypeId field_type = Erase(FieldTypeAt(type, index));
      const InstId address = builder_->FieldAddress(
          types_->Pointer(field_type, /*is_mutable=*/true), object, index);
      builder_->Add(Opcode::kStore, TypeId{}, {address, value});
      ++index;
    }
    return object;
  }

  // The token for |type|, with the module's table for it built the first
  // time it is asked for.
  InstId ClassToken(TypeId type) {
    uint32_t index = 0;
    bool found = false;
    for (uint32_t i = 0; i < module_->classes.size(); ++i) {
      if (module_->classes[i].type == type) {
        index = i;
        found = true;
        break;
      }
    }
    if (!found) {
      ClassInfo info;
      info.type = type;
      info.token = ClassTokenOf(*types_, type);
      for (const VirtualSlot& slot : SlotsOf(type)) {
        info.methods.push_back(
            ClassInfo::Method{slot.selector, slot.function});
      }
      index = static_cast<uint32_t>(module_->classes.size());
      module_->classes.push_back(std::move(info));
    }
    const InstId token =
        builder_->Add(Opcode::kClassTable, types_->u64(), {});
    module_->functions[current_function_].Get(token).payload.klass = index;
    return token;
  }

  // What |type| implements, and under which declaration.
  //
  // One entry per virtual method visible on the class, named by the
  // declaration that introduced it -- so an `override` implements the
  // base's selector and a module that only sees the base can still call
  // it (D85).
  struct VirtualSlot {
    Symbol name;
    uint64_t selector = 0;
    uint32_t function = 0xFFFFFFFFu;
  };

  std::vector<VirtualSlot> SlotsOf(TypeId type) {
    std::vector<TypeId> chain;
    for (TypeId current = type; current.is_valid();
         current = resolution().BaseOf(current)) {
      chain.push_back(current);
    }
    std::reverse(chain.begin(), chain.end());

    std::vector<VirtualSlot> slots;

    // What the abilities this type implements asked for. The selector is the
    // *ability's* declaration, so a call through an ability value finds the
    // body without knowing what implements it (D86).
    for (TypeId ability : resolution().AbilitiesOf(type)) {
      for (const Member& wanted : resolution().MembersOf(ability)) {
        if (wanted.kind != MemberKind::kMethod) {
          continue;
        }
        const Member* body = FindAnyMember(type, wanted.name);
        if (body == nullptr || body->kind != MemberKind::kMethod) {
          continue;
        }
        slots.push_back(VirtualSlot{wanted.name, MemberSelector(wanted),
                                    ThunkFor(type, *body)});
      }
    }

    for (TypeId current : chain) {
      for (const Member& member : resolution().MembersOf(current)) {
        if (member.kind != MemberKind::kMethod ||
            !(member.is_virtual || member.is_override)) {
          continue;
        }
        const uint32_t body = MethodIndex(current, member);
        bool replaced = false;
        for (VirtualSlot& slot : slots) {
          if (slot.name == member.name) {
            // The selector stays the base's: this is the body for it.
            slot.function = body;
            replaced = true;
            break;
          }
        }
        if (!replaced) {
          slots.push_back(VirtualSlot{
              member.name, MemberSelector(member), body});
        }
      }
    }
    return slots;
  }

  // The declaration a call on |type| should name, or zero when the method
  // is not virtual at all.
  uint64_t SelectorOf(TypeId type, Symbol name) {
    for (const VirtualSlot& slot : SlotsOf(type)) {
      if (slot.name == name) {
        return slot.selector;
      }
    }
    return 0;
  }

  // Which declaration a member is: the same packing an object's class
  // token uses, so one scheme names both.
  static uint64_t MemberSelector(const Member& member) {
    return (static_cast<uint64_t>(member.file) << 32) | member.item;
  }

  // What an ability call can actually call.
  //
  // A method of a value type takes `self` as a value; a call through a
  // ability has only an address, because it does not know what is there.
  // So the table gets a one-line function that loads the value and calls
  // the method -- the thunk every vtable over value types needs.
  //
  // A class needs none: its handle *is* the address.
  uint32_t ThunkFor(TypeId owner, const Member& member) {
    const uint32_t body = MethodIndex(owner, member);
    if (types_->IsHandle(owner)) {
      return body;
    }

    std::string mangled(interner_->Text(MethodName(owner, member.name)));
    mangled.append("$thunk");
    const Symbol name = interner_->Intern(mangled);
    for (const auto& [existing, index] : specializations_) {
      if (existing == name) {
        return index;
      }
    }

    const TypeId pointer =
        types_->Pointer(Erase(owner), /*is_mutable=*/true);
    std::vector<TypeId> params;
    params.push_back(pointer);
    for (TypeId param : types_->ArgumentsOf(member.type)) {
      params.push_back(Erase(param));
    }
    const TypeId result = Erase(types_->ElementOf(member.type));

    Function thunk;
    thunk.name = name;
    thunk.type = types_->Function(params, result);
    const auto index = static_cast<uint32_t>(module_->functions.size());
    module_->functions.push_back(std::move(thunk));
    specializations_.emplace_back(name, index);

    // Written here rather than queued: it has no source to lower, and
    // the builder is the same one every other function is built with.
    Builder builder(module_, FunctionId{index});
    const BlockId entry = builder.AddBlock("entry");
    builder.SetCurrent(entry);
    const InstId self = builder.AddParam(entry, pointer);
    std::vector<InstId> arguments;
    arguments.push_back(
        builder.Add(Opcode::kLoad, Erase(owner), {self}));
    for (TypeId param : types_->ArgumentsOf(member.type)) {
      arguments.push_back(builder.AddParam(entry, Erase(param)));
    }
    const InstId call = builder.Call(body, result, std::move(arguments));
    if (result.is_valid() && types_->KindOf(result) != TypeKind::kVoid) {
      builder.Return(call);
    } else {
      builder.ReturnVoid();
    }
    return index;
  }

  // The function implementing |member| of |owner|, declaring it if this
  // file only knows the signature.
  uint32_t MethodIndex(TypeId owner, const Member& member,
                       ExprId call = ExprId{}) {
    // A method of a generic type is a recipe: ask for the copy for *this*
    // instantiation rather than for a declaration with no body.
    //
    // So is a method with type parameters of its own, whatever its type
    // is: `fun Map<T>(f: fun(int) -> T)` on a plain `class Box` needs a
    // copy per `T`, and asking for the declaration instead left the call
    // naming a function with no body (D119).
    if (types_->KindOf(owner) == TypeKind::kInstance ||
        MethodParameters(owner, member).size() != 0) {
      const uint32_t copy =
          InstantiateMethod(owner, member.name, member, call);
      if (copy != 0xFFFFFFFFu) {
        return copy;
      }
    }
    uint32_t index =
        IndexOfSource(member.file, member.item, member.slot);
    if (index != 0xFFFFFFFFu) {
      return index;
    }
    std::vector<TypeId> params;
    params.push_back(owner);
    for (TypeId param : types_->ArgumentsOf(member.type)) {
      params.push_back(Erase(param));
    }
    const TypeId signature =
        types_->Function(params, Erase(types_->ElementOf(member.type)));
    index = DeclareExternal(MethodName(owner, member.name), signature,
                            member.file, member.item, /*is_task=*/false,
                            /*has_body=*/false, member.slot);
    Remember(member.file, member.item, index, member.slot);
    return index;
  }

  // A method: `self` first, then what was written. The receiver is passed
  // by address, so a method that assigns to a field changes the object it
  // was called on rather than a copy of it -- which is the difference
  // between a method and a function that happens to be written inside a
  // type.
  void DeclareMethod(ItemId id, const Item& item, TypeId owner) {
    const Member* member = resolution().FindMember(owner, item.name);
    const TypeId declared = member != nullptr ? member->type : TypeId{};
    if (!declared.is_valid() ||
        types_->KindOf(declared) != TypeKind::kFunction) {
      return;
    }
    // A parameter in the signature, or a type that *has* parameters: a
    // method of `Storage<T>` is a recipe even when it takes and returns
    // nothing but integers, because its `self` is not one type.
    if (types_->MentionsParameter(declared) ||
        types_->MentionsParameter(owner) ||
        !resolution().ParametersOf(owner).empty()) {
      // A generic type's method is a recipe: there is nothing to declare
      // until something asks for `Box<int>.Get`, and where its body is
      // comes from the member table rather than from anything kept here.
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
    indices_.push_back(Declared{id, module_->functions.size(),
                                ast().Get(id).name, file_index(),
                                /*is_member=*/true});
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
    for (const Declared& declared : indices_) {
      if (declared.file == file_index() && declared.name == name &&
          !declared.is_member) {
        return static_cast<uint32_t>(declared.index);
      }
    }
    return 0xFFFFFFFFu;
  }

  void Define(ItemId id, TypeId owner) {
    if (!ast().Contains(id)) {
      return;
    }
    const Item& item = ast().Get(id);
    switch (item.kind) {
      case ItemKind::kDeinit:
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

      case ItemKind::kSystem:
        LowerSystem(item, IndexOf(id));
        break;

      case ItemKind::kClass:
      case ItemKind::kStruct:
      case ItemKind::kComponent:
      case ItemKind::kEnum:
      case ItemKind::kExtend:
        for (ItemId member : ast().Get(item.members)) {
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
    const Binding* binding = resolution().Find(item.name);
    if (binding == nullptr) {
      return;
    }
    const TypeId machine = Erase(binding->type);
    const TypeId answer = types_->bool_type();

    // The blocks first, so an event can call them.
    for (StateId state_id : ast().Get(item.states)) {
      if (!ast().Contains(state_id)) {
        continue;
      }
      const State& state = ast().Get(state_id);
      for (int which = 0; which < 2; ++which) {
        const ExprId block = which == 0 ? state.enter : state.exit;
        if (!ast().Contains(block)) {
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

    for (const Member& member : resolution().MembersOf(machine)) {
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
    const Binding* binding = resolution().Find(item.name);
    if (binding == nullptr) {
      return;
    }
    const TypeId machine = Erase(binding->type);

    // The states, in the order they were declared: that order is the
    // numbering, and the first one is where a machine starts.
    std::vector<StateId> states;
    for (StateId state_id : ast().Get(item.states)) {
      if (ast().Contains(state_id)) {
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
    // The drop flags belong to the body being lowered. Left behind, a
    // flag of the *previous* function was found by name here, and its
    // instruction id meant something else entirely in this one -- which
    // the dev tier turned into a store through whatever was at that
    // index (D111).
    owners_.clear();
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
    owners_.clear();
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
      const State& state = ast().Get(states[i]);
      for (TransitionId transition_id : ast().Get(state.transitions)) {
        if (!ast().Contains(transition_id)) {
          continue;
        }
        const Transition& transition = ast().Get(transition_id);
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
      CallMachineBlock(machine, ast().Get(states[froms[i]]).name,
                       /*is_enter=*/false);
      builder.Add(Opcode::kStore, TypeId{},
                  {state_address,
                   builder.ConstInt(state_type,
                                    static_cast<uint64_t>(targets[i]))});
      for (uint32_t step : PathTo(states, froms[i], method.name)) {
        CallMachineBlock(machine, ast().Get(states[step]).name,
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
    const State& state = ast().Get(states[from]);
    for (TransitionId transition_id : ast().Get(state.transitions)) {
      if (!ast().Contains(transition_id)) {
        continue;
      }
      const Transition& transition = ast().Get(transition_id);
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
    const State& state = ast().Get(states[from]);
    for (TransitionId transition_id : ast().Get(state.transitions)) {
      if (!ast().Contains(transition_id)) {
        continue;
      }
      const Transition& transition = ast().Get(transition_id);
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
      if (ast().Get(states[i]).name == name) {
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
    for (const Member& member : resolution().MembersOf(machine)) {
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
    const Member* member = resolution().FindMember(owner, item.name);
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
    indices_.push_back(Declared{id, module_->functions.size(),
                                ast().Get(id).name, file_index()});
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
    owners_.clear();
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
      // A declaration with no body -- an ability method, an `extern` -- has
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
    owners_.clear();
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

    for (ParamId param_id : ast().Get(item.params)) {
      const Param& param = ast().Get(param_id);
      const TypeId type =
          ParamTypeOf(module_->functions[index], locals_.size());
      // What the parameter was *declared* as, which is what says whether
      // the callee owns it: a `Text` parameter is handed a string the
      // caller gave up, and a `text` one is a borrow (D133).
      const TypeId declared = param.type.is_valid()
                                  ? Apply(resolver_.Resolve(ast(),
                                                            param.type))
                                  : type;
      const InstId value = builder.AddParam(entry, type);
      if (in_task_) {
        // A parameter outlives every suspension, so in a task it lives in
        // the frame like everything else.
        const InstId slot = builder.Add(Opcode::kAlloca, type);
        builder.Add(Opcode::kStore, TypeId{}, {slot, value});
        locals_.push_back(Local{param.name, slot, InstId{}, type});
        RememberOwner(param.name, slot, declared, /*live=*/true);
        continue;
      }
      locals_.push_back(Local{param.name, InstId{}, value, type});
      RememberOwner(param.name, value, declared, /*live=*/true,
                    /*in_slot=*/false);
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
    const TypeId source_type = Erase(from);

    // A `T!`: which it is, and the value when it is a value (D90). Zero
    // says it is a value; anything else says which error, named by the
    // tag's own symbol so that one number means one error everywhere.
    if (types_->KindOf(target) == TypeKind::kFallible &&
        types_->KindOf(source_type) != TypeKind::kFallible) {
      const TypeId held = Erase(types_->ElementOf(target));
      if (types_->KindOf(source_type) == TypeKind::kErrorSet) {
        // The value *is* the tag, so it travels as it is and the payload
        // is whatever was there -- a `T!` that failed has no value, and
        // nothing may read one out of it without matching first.
        return builder_->Add(Opcode::kMakeStruct, target,
                             {value, builder_->ConstInt(held, 0)});
      }
      return builder_->Add(
          Opcode::kMakeStruct, target,
          {builder_->ConstInt(types_->u32(), 0),
           Coerce(value, from, held, source)});
    }

    // A value where an ability is wanted: the pair that says what it is and
    // which class that is (D86). A class hands over its handle; a value
    // type has to be somewhere first, so it is put in a slot and the
    // address of that is what travels -- which is safe for the reason a
    // `str` is: an ability value cannot be stored in a field, so it cannot
    // outlive the frame it points into.
    if (source_type.is_valid() && source_type != target &&
        resolution().IsAbility(target) && !resolution().IsAbility(source_type)) {
      InstId address = value;
      if (!types_->IsHandle(source_type)) {
        // Copied onto the heap rather than pointed at where it is. A
        // ability value is the one thing in this language that outlives the
        // expression it was made in -- a list of shapes is the point of
        // having it -- and the reference tier's locals have no addresses
        // that survive a call, so a borrow could not work in every tier.
        //
        // Nothing frees it yet. That waits for destructors, which is the
        // next thing this needs.
        const TypeId boxed =
            types_->Pointer(source_type, /*is_mutable=*/true);
        const InstId object = builder_->Add(Opcode::kNew, boxed);
        builder_->Add(Opcode::kStore, TypeId{}, {object, value});
        address = object;
      }
      const InstId token = ClassToken(source_type);
      return builder_->Add(Opcode::kMakeStruct, target, {address, token});
    }

    // A reference where a raw pointer is wanted. The same address, and
    // the checker already decided it was allowed (D8) -- the cast is here
    // so that the IR's types stay exact.
    if (source_type.is_valid() && source_type != target &&
        types_->KindOf(source_type) == TypeKind::kReference &&
        types_->KindOf(target) == TypeKind::kPointer) {
      return builder_->Add(Opcode::kCast, target, {value});
    }

    // A pointer that may be written where one that may not is wanted.
    // The same address; what differs is a promise the checker already
    // held us to. Written down because the IR's types are compared
    // exactly, and a branch that produced one where a slot held the
    // other was a type mismatch the verifier was right to report
    // (D153).
    if (source_type.is_valid() && source_type != target &&
        types_->KindOf(source_type) == TypeKind::kPointer &&
        types_->KindOf(target) == TypeKind::kPointer &&
        Erase(types_->ElementOf(source_type)) ==
            Erase(types_->ElementOf(target))) {
      return builder_->Add(Opcode::kCast, target, {value});
    }

    // A derived handle where its base is wanted. Nothing happens at run
    // time -- a base's fields are the first fields of what derives from it
    // (D84), so the address is the same address -- but the IR says so,
    // because every type in it is exact and a verifier that guessed at
    // subtyping would be guessing at layout too.
    if (source_type.is_valid() && source_type != target &&
        types_->IsHandle(source_type) && types_->IsHandle(target) &&
        resolution().Satisfies(source_type, target)) {
      return builder_->Add(Opcode::kCast, target, {value});
    }
    if (types_->KindOf(target) != TypeKind::kOptional) {
      return value;
    }
    // Asked of the value rather than of what the checker recorded.
    //
    // The checker records the type the *context* wanted, so a `value`
    // being returned where `T?` is declared is recorded as `T?` while the
    // value in hand is a `T`. Reading the recorded type here meant no
    // wrap was emitted and a bare `T` was returned as an optional: the
    // compiled tier read its first field as the tag and the reference
    // tier saw a value of the wrong shape (D100).
    const TypeId held = ValueType(value);
    if (types_->KindOf(held) == TypeKind::kOptional) {
      return value;
    }
    return builder_->Add(Opcode::kOptionalWrap, target, {value});
  }

  // The type an instruction says it produces, which is what a value
  // actually is -- as against what the checker recorded at the expression
  // it came from, which is what its context wanted.
  TypeId ValueType(InstId value) {
    const Function& current = module_->functions[current_function_];
    return current.Contains(value) ? current.Get(value).type : TypeId{};
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
    const TypeId recorded = typed().TypeOf(id);
    return Erase(Apply(recorded.is_valid() ? recorded : types_->error()));
  }

  // The same, as the checker recorded it: with the dimensions and the
  // ownership still on it. What decides who frees a string, which
  // erasing is exactly what loses (D133).
  TypeId DeclaredTypeOf(ExprId id) {
    const TypeId recorded = typed().TypeOf(id);
    return Apply(recorded.is_valid() ? recorded : types_->error());
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
      case TypeKind::kString:
        // Owned or borrowed, a string is a pointer and a length: the IR
        // has one string type, and who frees it is tracked beside the
        // value rather than in it. Keeping two made a branch that
        // yielded an interpolated string into a variable the checker
        // typed as owned a type mismatch the verifier was right to
        // report (D127).
        return types_->str();
      case TypeKind::kReference:
        // A reference is an address at run time, and the IR has one kind
        // of address: erasing it here is what keeps a `&mut Builder`
        // parameter and the `&mut builder` passed to it the same type
        // (D109).
        return types_->Pointer(Erase(types_->ElementOf(type)),
                               types_->Get(type).is_mutable);
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
    if (!ast().Contains(id) || builder_->Terminated()) {
      return;
    }
    const Expr& expr = ast().Get(id);
    if (expr.kind == ExprKind::kBlock) {
      const size_t depth = locals_.size();
      ++scope_depth_;
      for (StmtId stmt : ast().Get(expr.stmts)) {
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
    if (!ast().Contains(id) || builder_->Terminated()) {
      return;
    }
    const Stmt& stmt = ast().Get(id);
    // Everything this statement lowers to belongs to the statement: one
    // line per step is what a debugger steps through, and what a trap
    // names (D118).
    if (stmt.span.is_valid()) {
      builder_->SetWhere(stmt.span);
    }
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
          deferred_.push_back(Deferred{scope_depth_, stmt.expr, InstId{},
                                       TypeId{}, InstId{}, true});
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
    const Pattern* pattern = ast().Contains(stmt.pattern)
                                 ? &ast().Get(stmt.pattern)
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
    TypeId type = stmt.expr.is_valid() ? DeclaredTypeOf(stmt.expr)
                                       : TypeId{};
    // An annotation wins over what the initializer happened to produce:
    // `const message: Text = "..."` says the binding owns the string.
    if (stmt.type.is_valid()) {
      const TypeId said = Apply(resolver_.Resolve(ast(), stmt.type));
      if (!type.is_valid() || types_->IsError(type) ||
          types_->KindOf(said) == TypeKind::kString) {
        type = said;
      }
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
    // A binding that owns something goes in a slot even when it is a
    // `const`: the flag that says whether it still owns it is written
    // beside the value, and a value with no address has nowhere for the
    // scope exit to read it from (D133).
    if (!mutable_binding && !in_task_ &&
        !HasFlag(pattern->flags, PatternFlags::kMutable) &&
        !Owns(type)) {
      locals_.push_back(Local{pattern->name, InstId{}, value, type});
      return;
    }

    // A `var` goes in a slot. Lowering every one of them and letting SROA
    // promote the ones that can be is simpler than deciding here.
    const InstId slot = builder_->Add(Opcode::kAlloca, Erase(type));
    if (value.is_valid()) {
      // Of the slot's type, for the same reason an assignment is
      // (D153): `var p: *u8 = &xs[0]` makes a pointer that may be
      // written, and the slot holds one that may not.
      builder_->Add(Opcode::kStore, TypeId{},
                    {slot, Coerce(value, ValueType(value), Erase(type))});
    }
    locals_.push_back(Local{pattern->name, slot, InstId{}, Erase(type)});
    // The *unerased* type decides who frees: erasing is what loses the
    // difference between a string that owns its bytes and a view.
    RememberOwner(pattern->name, slot, type, value.is_valid());
  }

  // Everything a scope has to run on the way out, innermost last. A `defer`
  // is the one statement whose position in the source is not where it runs,
  // so lowering is where it becomes ordinary code -- emitted at every exit
  // from the scope that deferred it, which includes the ones a `return` or
  // a `break` take.
  struct Deferred {
    size_t scope = 0;
    // A `defer` statement: the expression to run on the way out.
    ExprId expr;
    // Or a value to destroy on the way out, with the flag that says
    // whether it is still there to destroy (D87).
    InstId value;
    TypeId type;
    InstId flag;
    // Whether |value| is the slot holding it or the value itself.
    bool in_slot = true;
    // Storage to give back with no destructor to run first: a closure's
    // environment, which is a few copied values and nothing more (D138).
    bool free_only = false;
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
      const Deferred& entry = deferred_[i - 1];
      if (entry.free_only) {
        FreeObject(entry.value);
        continue;
      }
      if (entry.value.is_valid()) {
        EmitDestroy(entry);
        continue;
      }
      Value(entry.expr);
    }
  }

  // Says that |name| holds something that has to be destroyed when the
  // scope ends, and gives it the flag that says whether it still does.
  void RememberOwner(Symbol name,
                     InstId held,
                     TypeId type,
                     bool live,
                     bool in_slot = true) {
    if (!Owns(type)) {
      return;
    }
    const InstId flag =
        builder_->Add(Opcode::kAlloca, types_->bool_type());
    builder_->Add(Opcode::kStore, TypeId{},
                  {flag, builder_->ConstBool(live)});
    deferred_.push_back(
        Deferred{scope_depth_, ExprId{}, held, type, flag, in_slot});
    owners_.push_back(Owner{name, flag});
  }

  // Where the flag for |name| is, or an invalid id when the name owns
  // nothing.
  InstId FlagOf(Symbol name) const {
    for (auto it = owners_.rbegin(); it != owners_.rend(); ++it) {
      if (it->name == name) {
        return it->flag;
      }
    }
    return InstId{};
  }

  // What a scope exit does to a value that owns something.
  //
  // Guarded by the flag, because a value may have been handed away since
  // it was declared: the flag is set where the binding is initialized and
  // cleared where it is moved, so what runs here is what is still there.
  void EmitDestroy(const Deferred& entry) {
    const InstId live =
        builder_->Add(Opcode::kLoad, types_->bool_type(), {entry.flag});
    const BlockId destroy = builder_->AddBlock("drop.run");
    const BlockId after = builder_->AddBlock("drop.skip");
    builder_->Branch(live, destroy, after);

    builder_->SetCurrent(destroy);
    // A binding in a slot is read from it; a parameter is the value it
    // was handed.
    const InstId value =
        entry.in_slot
            ? builder_->Add(Opcode::kLoad, Erase(entry.type), {entry.value})
            : entry.value;
    Destroy(value, entry.type);
    builder_->Jump(after, {});
    builder_->SetCurrent(after);
  }

  // Runs the destructor of |type| on |value|, then frees what it was in.
  void Destroy(InstId value, TypeId raw) {
    // An owned string: the bytes it was built with, given back. The
    // runtime leaves a string an `arena` block handed out to the block
    // (D113, D133).
    if (types_->KindOf(raw) == TypeKind::kString) {
      const TypeId bytes = types_->Pointer(
          types_->Int(IntWidth::k8, /*is_signed=*/false),
          /*is_mutable=*/false);
      const Symbol name = interner_->Intern("wolang_text_free");
      const uint32_t free = DeclareExternal(
          name, types_->Function({bytes}, types_->void_type()));
      module_->functions[free].foreign_symbol = name;
      builder_->Call(free, types_->void_type(),
                     {builder_->Extract(bytes, value, 0)});
      return;
    }
    const TypeId type = Erase(raw);
    if (const Member* deinit = FindDestructor(type)) {
      const uint32_t index = MethodIndex(type, *deinit);
      if (index != 0xFFFFFFFFu) {
        builder_->Call(index, types_->void_type(), {value});
      }
    }
    if (types_->IsHandle(type)) {
      // The object the handle was to. A class is a handle to a heap
      // object (D61), and this is the other half of that sentence.
      //
      // The handle is cast to a word first: one declaration of the free
      // serves every class, and a verifier that compares types exactly
      // would otherwise want one per class.
      const Symbol name = interner_->Intern("wolang_object_free");
      const uint32_t free = DeclareExternal(
          name, types_->Function({types_->u64()}, types_->void_type()));
      module_->functions[free].foreign_symbol = name;
      builder_->Call(free, types_->void_type(),
                     {AsDeclared(free, 0, value)});
    }
  }

  // The destructor of |type| or of something it derives from.
  const Member* FindDestructor(TypeId type) const {
    for (TypeId current = type; current.is_valid();
         current = resolution().BaseOf(current)) {
      for (const Member& member : resolution().MembersOf(current)) {
        if (member.kind == MemberKind::kDeinit) {
          return &member;
        }
      }
    }
    return nullptr;
  }

  // Whether a value of |type| has to be destroyed when it goes away.
  // Whether a value of |type| has to be given back when its scope ends.
  //
  // A class with a destructor, and an owned string -- which is a
  // declared type of its own (`Text`), allocated when it is built and
  // nobody's until this says whose. The type is the *unerased* one,
  // because erasing is exactly what loses the difference between a
  // string that owns its bytes and a view of someone else's (D133).
  bool Owns(TypeId type) {
    if (types_->KindOf(type) == TypeKind::kString) {
      return true;
    }
    return FindDestructor(Erase(type)) != nullptr;
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
    if (!ast().Contains(id) || builder_->Terminated()) {
      return InstId{};
    }
    const Expr& expr = ast().Get(id);
    // Narrowed to this expression: a trap inside a statement should name
    // the operation that trapped rather than the line's first column
    // (D118). Put back on the way out, so that what follows belongs to
    // the statement again.
    const SourceSpan outer = builder_->where();
    if (expr.span.is_valid()) {
      builder_->SetWhere(expr.span);
    }
    const Restore restore(builder_, outer);
    const TypeId type = TypeOf(id);

    switch (expr.kind) {
      case ExprKind::kIntLiteral: {
        const NumberLiteral& number = ast().number(expr.aux);
        const TypeId built = Unwrapped(type);
        return Coerce(builder_->ConstInt(built, number.integer_value),
                      built, type);
      }
      case ExprKind::kFloatLiteral: {
        const NumberLiteral& number = ast().number(expr.aux);
        const TypeId built = Unwrapped(type);
        return Coerce(builder_->ConstFloat(built, number.float_value),
                      built, type);
      }
      case ExprKind::kBoolLiteral:
        return Coerce(builder_->ConstBool(expr.aux != 0),
                      types_->bool_type(), type);
      case ExprKind::kNullLiteral:
        return builder_->ConstNull(type);
      case ExprKind::kPointerNull: {
        // Zero, as an address. Written as a cast rather than as a
        // pointer-typed constant so that every tier sees the same shape
        // it already sees for `0 as *mut u8` (D156).
        const InstId zero =
            builder_->ConstInt(types_->Int(IntWidth::k64, false), 0);
        return builder_->Add(Opcode::kCast, Erase(type), {zero});
      }
      case ExprKind::kStringLiteral:
        return LowerString(expr, type);

      case ExprKind::kPath:
        return LowerPath(id, expr, type);

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
          const TypeId want = Erase(type);
          if (!want.is_valid() || ValueType(operand) == want) {
            return operand;
          }
          // The same pair under another name: rebuilt rather than passed
          // through, so that the instruction says what it is. Letting the
          // operand's type stand made a `[]u8 as text` a `[]u8` as far as
          // everything downstream could tell (D111).
          const TypeId bytes = types_->Pointer(
              types_->Int(IntWidth::k8, /*is_signed=*/false),
              /*is_mutable=*/false);
          const TypeId word = types_->u64();
          return builder_->Add(Opcode::kMakeStruct, want,
                               {builder_->Extract(bytes, operand, 0),
                                builder_->Extract(word, operand, 1)});
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
        for (ExprId element : ast().Get(expr.args)) {
          elements.push_back(Value(element));
        }
        return builder_->Add(Opcode::kMakeArray, type, std::move(elements));
      }

      case ExprKind::kIndex: {
        // `xs[a..b]` is a view of part of something, not an element of it.
        if (ast().Contains(expr.rhs) &&
            ast().Get(expr.rhs).kind == ExprKind::kRange) {
          return LowerSliceRange(expr, type);
        }
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

      case ExprKind::kTry:
        // `expr?`: hand the error on, or carry on with the value.
        return LowerTry(expr, type);

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

      case ExprKind::kClosure:
        return LowerClosure(id, expr);

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
    const std::span<const ExprId> parts = ast().Get(expr.args);
    if (parts.size() == 1 && ast().Contains(parts[0]) &&
        ast().Get(parts[0]).kind == ExprKind::kStringText) {
      const Expr& text = ast().Get(parts[0]);
      return builder_->ConstString(type,
                                   ast().string(text.aux));
    }
    // Interpolation builds a string at runtime, by turning each piece into
    // text and concatenating. The conversions are calls into the runtime
    // -- the floor the language stands on -- and both tiers make the same
    // ones.
    const TypeId str = types_->str();
    InstId result;
    // Whether what `result` holds was allocated by this expression. A
    // concatenation copies both sides into a new allocation, so the
    // pieces it consumed can go back immediately -- which is what keeps
    // `"a {b} c {d}"` to one live string instead of one per piece. The
    // leak this closes was the whole of why a program that builds strings
    // in a loop grew (D133).
    bool result_is_fresh = false;
    for (ExprId part : parts) {
      if (!ast().Contains(part)) {
        continue;
      }
      const Expr& piece = ast().Get(part);
      InstId text;
      bool text_is_fresh = false;
      if (piece.kind == ExprKind::kStringText) {
        text = builder_->ConstString(str, ast().string(piece.aux));
      } else {
        const TypeId held = TypeOf(part);
        text = Stringify(Value(part), held);
        // A number, a bool or a float became a string just now; a string
        // that was already one belongs to whoever made it.
        text_is_fresh = types_->KindOf(held) != TypeKind::kStr &&
                        types_->KindOf(held) != TypeKind::kString;
      }
      if (!text.is_valid()) {
        continue;
      }
      if (!result.is_valid()) {
        result = text;
        result_is_fresh = text_is_fresh;
        continue;
      }
      const InstId joined = ConcatText(result, text);
      if (result_is_fresh) {
        FreeText(result);
      }
      if (text_is_fresh) {
        FreeText(text);
      }
      result = joined;
      result_is_fresh = true;
      continue;
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

  // |value| in the type parameter |which| of |callee| is declared with.
  //
  // The same runtime function may be declared twice: once by a library
  // that wrote `extern "C" fun wolang_object_free(at: *mut u8)` and once
  // here, where a handle is a word. Only one declaration survives --
  // they are deduplicated by name -- so a call made here has to say what
  // that one says (D102).
  InstId AsDeclared(uint32_t callee, size_t which, InstId value) {
    const std::vector<TypeId> params =
        types_->ArgumentsOf(module_->functions[callee].type);
    if (which >= params.size() || !params[which].is_valid()) {
      return value;
    }
    const TypeId wanted = Erase(params[which]);
    if (!wanted.is_valid() || wanted == ValueType(value)) {
      return value;
    }
    return builder_->Add(Opcode::kCast, wanted, {value});
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

  // Frees |object| when the scope being lowered ends.
  //
  // What a closure's environment needs: it lives as long as the closure
  // may be called, which is the scope that made it (D138).
  void FreeAtScopeExit(InstId object) {
    deferred_.push_back(Deferred{scope_depth_, ExprId{}, object,
                                 types_->u64(), InstId{},
                                 /*in_slot=*/false,
                                 /*free_only=*/true});
  }

  // The object free every heap allocation goes back through. The runtime
  // leaves one an `arena` block handed out to the block (D113).
  void FreeObject(InstId object) {
    if (!object.is_valid()) {
      return;
    }
    const Symbol name = interner_->Intern("wolang_object_free");
    const uint32_t free = DeclareExternal(
        name, types_->Function({types_->u64()}, types_->void_type()));
    module_->functions[free].foreign_symbol = name;
    builder_->Call(free, types_->void_type(),
                   {AsDeclared(free, 0, object)});
  }

  // Gives a string's bytes back. The runtime leaves one an `arena` block
  // handed out to the block (D113, D133).
  void FreeText(InstId value) {
    const TypeId bytes = types_->Pointer(
        types_->Int(IntWidth::k8, /*is_signed=*/false),
        /*is_mutable=*/false);
    const Symbol name = interner_->Intern("wolang_text_free");
    const uint32_t free = DeclareExternal(
        name, types_->Function({bytes}, types_->void_type()));
    module_->functions[free].foreign_symbol = name;
    builder_->Call(free, types_->void_type(),
                   {builder_->Extract(bytes, value, 0)});
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
        // Anything else needs a formatting ability, which arrives with the
        // rest of `core`.
        return builder_->ConstString(str, "<?>");
    }
  }

  InstId LowerPath(ExprId id, const Expr& expr, TypeId type) {
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
      // An error tag is its number: `Corrupt` as a value is the tag a
      // `T!` carries when it failed that way (D90). One number per
      // declaration, so the test and the construction agree without
      // either side knowing about the other.
      if (const Binding* tag = resolution().Find(expr.name);
          tag != nullptr && tag->kind == BindingKind::kErrorTag) {
        return builder_->ConstInt(types_->u32(), expr.name.index + 1);
      }
      // A module-level `var`, read from its storage.
      if (const TypeId held = GlobalType(expr.name); held.is_valid()) {
        const InstId address = GlobalAddress(expr.name);
        return Coerce(builder_->Add(Opcode::kLoad, held, {address}), held,
                      type);
      }
      // A module-level constant, read through the function it became.
      if (const InstId constant = ConstantValue(expr.name, type);
          constant.is_valid()) {
        return constant;
      }
      return builder_->Undef(type);
    }
    // Handing an owning value away is a move: the name no longer holds
    // it, so the scope exit must not destroy it. The checker already
    // decided which uses are borrows and which are not (D87), so this
    // reads the same list rather than guessing again.
    if (const InstId flag = FlagOf(expr.name);
        flag.is_valid() && borrow_depth_ == 0 && !IsBorrow(id)) {
      builder_->Add(Opcode::kStore, TypeId{},
                    {flag, builder_->ConstBool(false)});
    }
    if (local->slot.is_valid()) {
      return builder_->Add(Opcode::kLoad, local->type, {local->slot});
    }
    return local->value;
  }

  // Puts the builder's span back when a nested expression is done.
  class Restore {
   public:
    Restore(Builder* builder, SourceSpan span)
        : builder_(builder), span_(span) {}
    ~Restore() {
      if (builder_ != nullptr && span_.is_valid()) {
        builder_->SetWhere(span_);
      }
    }
    Restore(const Restore&) = delete;
    Restore& operator=(const Restore&) = delete;

   private:
    Builder* builder_;
    SourceSpan span_;
  };

  // While this is set, reading a name does not take what it holds: the
  // base of a field access, an index or a method call is *read through*
  // rather than handed away, which is the same rule the checker's own walk
  // follows (D87).
  class Borrowing {
   public:
    explicit Borrowing(Lowerer* lowerer) : lowerer_(lowerer) {
      if (lowerer_ != nullptr) {
        ++lowerer_->borrow_depth_;
      }
    }
    ~Borrowing() {
      if (lowerer_ != nullptr) {
        --lowerer_->borrow_depth_;
      }
    }

    Borrowing(const Borrowing&) = delete;
    Borrowing& operator=(const Borrowing&) = delete;

   private:
    Lowerer* lowerer_;
  };

  // Whether the checker marked this use as a borrow rather than a move.
  bool IsBorrow(ExprId id) const {
    for (ExprId borrow : typed().borrows) {
      if (borrow == id) {
        return true;
      }
    }
    return false;
  }

  InstId LowerUnary(const Expr& expr, TypeId type) {
    const InstId operand = Value(expr.lhs);
    // What arithmetic produces is what it was given, not what the context
    // wanted: `-total` where an `int?` is expected is a negation of an
    // `int`, and the wrap to the optional is a `Coerce` like every other
    // one (D111).
    const TypeId held =
        operand.is_valid() && ValueType(operand).is_valid()
            ? ValueType(operand)
            : Erase(type);
    switch (expr.op) {
      case TokenKind::kMinus:
        return Coerce(builder_->Add(Opcode::kNeg, held, {operand}), held,
                      type);
      case TokenKind::kBang:
        return builder_->Add(Opcode::kLogicalNot, types_->bool_type(),
                             {operand});
      case TokenKind::kTilde:
        return Coerce(builder_->Add(Opcode::kBitNot, held, {operand}),
                      held, type);
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
    // `a + b` on strings is the two of them joined, which is the same
    // call an interpolated string makes (D110).
    if (opcode == Opcode::kAdd && IsView(TypeOf(expr.lhs)) &&
        IsView(TypeOf(expr.rhs))) {
      return ConcatText(left, right);
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
    if (compare) {
      return builder_->Add(opcode, types_->bool_type(), {left, right});
    }
    // Typed by what it operates on rather than by what the context
    // wanted: `sign * whole` where a `float?` is expected multiplies two
    // floats, and the wrap is a `Coerce` like every other one (D111).
    const TypeId held = ValueType(left).is_valid() ? ValueType(left)
                                                   : Erase(type);
    return Coerce(builder_->Add(opcode, held, {left, right}), held, type);
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
  // declaration order, so counting the fields before it is the offset --
  // and for a class, the fields it inherits are counted first, because
  // that is where the layout puts them (D84).
  uint32_t FieldIndex(TypeId owner, Symbol name) const {
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

    // Root first, so a base's field keeps its index in everything that
    // derives from it.
    std::vector<TypeId> chain;
    for (TypeId current = named; current.is_valid();
         current = resolution().BaseOf(current)) {
      chain.push_back(current);
    }
    std::reverse(chain.begin(), chain.end());

    // The descriptor, when the class has one, is the field before all of
    // them.
    uint32_t index = HasClassHeader(named) ? 1u : 0u;
    for (TypeId current : chain) {
      for (const Member& member : resolution().MembersOf(current)) {
        if (!HasStorage(member.kind)) {
          continue;
        }
        if (member.name == name) {
          return index;
        }
        ++index;
      }
    }
    return 0;
  }

  // The function that gives every module-level `var` of this file its
  // first value.
  //
  // A global starts as its type's zero -- that is what the runtime hands
  // back -- and this is where what the source wrote is stored into it.
  // Called before anything else runs, by whoever is running it: a global
  // initialized lazily would need a flag on every read.
  void EmitFileInit() {
    if (globals_.empty()) {
      return;
    }
    std::string label("file$init$");
    label.append(std::to_string(file_index()));
    Function init;
    init.name = interner_->Intern(label);
    init.type = types_->Function({}, types_->void_type());
    const auto index = static_cast<uint32_t>(module_->functions.size());
    module_->functions.push_back(std::move(init));
    module_->initializer = index;

    const uint32_t previous = current_function_;
    current_function_ = index;
    Builder builder(module_, FunctionId{index});
    Builder* outer = builder_;
    builder_ = &builder;
    const BlockId entry = builder.AddBlock("entry");
    builder.SetCurrent(entry);
    for (const Global& global : globals_) {
      if (!global.has_initializer) {
        continue;
      }
      const InstId address = builder.Add(
          Opcode::kGlobalAddress,
          types_->Pointer(global.type, /*is_mutable=*/true), {});
      builder.Mutable(address).payload.integer = global.token;
      const InstId value =
          builder.Call(global.initializer, global.type, {});
      builder.Add(Opcode::kStore, TypeId{}, {address, value});
    }
    builder.ReturnVoid();
    builder_ = outer;
    current_function_ = previous;
  }

  // A module-level `var`: storage the runtime owns, plus the function
  // that works out its first value.
  void DeclareGlobal(ItemId id, const Item& item) {
    const Binding* binding = resolution().Find(item.name);
    if (binding == nullptr) {
      return;
    }
    // A `var` with no annotation takes the type of what it was given, and
    // that is a checking-time answer rather than a signature one -- so the
    // binding may have no type here and the initializer does.
    TypeId type = Erase(binding->type);
    if ((!type.is_valid() || types_->IsError(type)) && item.init.is_valid()) {
      type = TypeOf(item.init);
    }
    if (!type.is_valid() || types_->IsError(type)) {
      return;
    }

    Function initializer;
    initializer.name = ConstantName(item.name);
    initializer.type = types_->Function({}, type);
    initializer.source_file = binding->file;
    initializer.source_item = binding->item;
    const auto index = static_cast<uint32_t>(module_->functions.size());
    Remember(initializer.source_file, initializer.source_item, index);
    indices_.push_back(
        Declared{id, index, ast().Get(id).name, file_index()});
    module_->functions.push_back(std::move(initializer));

    globals_.push_back(Global{
        item.name,
        (static_cast<uint64_t>(binding->file) << 32) | binding->item, type,
        index,
        // A `var` with no initializer starts as its type's zero, which is
        // what the runtime's storage already holds -- so there is nothing
        // to call, and calling the initializer that was never given a
        // body trapped with "no body to run" (D136).
        item.init.is_valid()});
  }

  // What |name| holds, or an invalid id when it is not a global.
  TypeId GlobalType(Symbol name) {
    for (const Global& global : globals_) {
      if (global.name == name) {
        return global.type;
      }
    }
    // A module-level `var` of *another* file of the same module. Its
    // storage belongs to the runtime and is found by the declaration it
    // came from, so reading one from here is the same instruction with
    // the same token -- and reading it as a *constant* instead, which is
    // what happened before, gave back the value it started with and
    // threw every write away (D140).
    if (const Binding* binding = resolution().Find(name);
        binding != nullptr && binding->kind == BindingKind::kValue &&
        binding->is_mutable && binding->type.is_valid()) {
      return Erase(binding->type);
    }
    return TypeId{};
  }

  // Where |name|'s storage is, as a pointer, or an invalid id when the
  // name is not a global.
  InstId GlobalAddress(Symbol name) {
    for (const Global& global : globals_) {
      if (global.name != name) {
        continue;
      }
      const InstId address = builder_->Add(
          Opcode::kGlobalAddress,
          types_->Pointer(global.type, /*is_mutable=*/true), {});
      builder_->Mutable(address).payload.integer = global.token;
      return address;
    }
    // Another file's: the same token, built from the declaration the
    // binding points at (D140).
    if (const Binding* binding = resolution().Find(name);
        binding != nullptr && binding->kind == BindingKind::kValue &&
        binding->is_mutable && binding->type.is_valid()) {
      const InstId address = builder_->Add(
          Opcode::kGlobalAddress,
          types_->Pointer(Erase(binding->type), /*is_mutable=*/true), {});
      builder_->Mutable(address).payload.integer =
          (static_cast<uint64_t>(binding->file) << 32) | binding->item;
      return address;
    }
    return InstId{};
  }

  // A member of |owner|, its own or one it inherits.
  const Member* FindAnyMember(TypeId owner, Symbol name) const {
    if (const Member* own = resolution().FindMember(owner, name)) {
      return own;
    }
    return resolution().FindInheritedMember(owner, name);
  }

  // Whether an object of |type| begins with a class descriptor. The same
  // question the layout answers; asked here because lowering decides the
  // field indices and both have to agree.
  bool HasClassHeader(TypeId type) const {
    if (!types_->IsHandle(type)) {
      return false;
    }
    for (TypeId current = type; current.is_valid();
         current = resolution().BaseOf(current)) {
      for (const Member& member : resolution().MembersOf(current)) {
        if (member.kind == MemberKind::kMethod &&
            (member.is_virtual || member.is_override)) {
          return true;
        }
      }
    }
    return false;
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
    const Borrowing borrowing(this);

    // `Shape.Circle`: a variant of an enum is the tag, and nothing else.
    // The base is the type's name rather than a value, so there is no
    // object to read a field out of (D89).
    if (const InstId variant = VariantValue(expr, type);
        variant.is_valid()) {
      return variant;
    }
    // A derived field is computed rather than stored, and kept once it is.
    const TypeId base_type = Erase(TypeOf(expr.lhs));
    if (const Member* member =
            FindAnyMember(base_type, expr.name);
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
    // The field's own type, not the one the context wanted: a field read
    // where a `T?` is expected is recorded as `T?` by the checker, and
    // taking that as the *extract's* type left the value unwrapped --
    // with the tag of the enum read as the tag of the optional (D111).
    const Member* member = FindAnyMember(owner, expr.name);
    const TypeId held =
        member != nullptr ? Erase(Apply(member->type)) : Erase(type);
    return Coerce(
        builder_->Extract(held, object, FieldIndex(owner, expr.name)),
        held, type);
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
      // `&x` on a whole local: a slot is not an address in a tier with
      // real frames, and this is the one place that needs it to be one
      // (D94). Reading and writing a slot goes through the slot itself,
      // which is what lets a slot be promoted out of memory at all.
      if (at.slot.is_valid() && at.path.empty() &&
          !at.soa_record.is_valid()) {
        return builder_->Add(
            Opcode::kLocalAddress,
            types_->Pointer(Erase(at.root_type), /*is_mutable=*/true),
            {at.slot});
      }
      return AddressOfPlace(at);
    }
    if (!value.is_valid()) {
      return value;
    }
    // A value with nowhere to point at: it is put somewhere first, and
    // what travels is the address of that -- `local.addr` rather than the
    // slot itself, because a slot is an index into the frame that made it
    // and `&x` is for handing to somebody else (D111).
    const TypeId held = Erase(TypeOf(place));
    const InstId slot = builder_->Add(Opcode::kAlloca, held);
    builder_->Add(Opcode::kStore, TypeId{}, {slot, value});
    return builder_->Add(Opcode::kLocalAddress,
                         types_->Pointer(held, /*is_mutable=*/true),
                         {slot});
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
    // For storage laid out one array per field: which record it holds.
    // The last step of such a place carries both the element index and
    // the field, because where a field of an element sits depends on how
    // many elements there are.
    TypeId soa_record;

    bool readable() const {
      return slot.is_valid() || pointer.is_valid() || value.is_valid();
    }
    bool addressable() const {
      return slot.is_valid() || pointer.is_valid();
    }
  };

  Place PlaceOf(ExprId id) {
    Place place;
    if (!ast().Contains(id)) {
      return place;
    }
    const Expr& expr = ast().Get(id);
    // Everything but a bare name reads *through* something rather than
    // taking it: assigning to `b.size` does not hand `b` away.
    const bool through = expr.kind != ExprKind::kPath &&
                         expr.kind != ExprKind::kSelf;
    const Borrowing borrowing(through ? this : nullptr);
    switch (expr.kind) {
      case ExprKind::kPath: {
        if (const Local* local = Find(expr.name); local != nullptr) {
          place.slot = local->slot;
          place.value = local->value;
          place.root_type = local->type;
          return place;
        }
        // A module-level `var`: a place at a known address, which is what
        // lets it be assigned to.
        if (const InstId address = GlobalAddress(expr.name);
            address.is_valid()) {
          place.pointer = address;
          place.root_type = Erase(TypeOf(id));
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
        // What this step reads is the *field's* type. The checker records
        // the type the context wanted at the expression, so a field read
        // where a `T?` is expected is recorded as `T?` -- and a step
        // typed that way read the enum's bytes as an optional's (D111).
        const TypeId through_address =
            types_->KindOf(owner) == TypeKind::kPointer
                ? Erase(types_->ElementOf(owner))
                : owner;
        const Member* read = FindAnyMember(through_address, expr.name);
        const TypeId field_type = read != nullptr
                                      ? Erase(Apply(read->type))
                                      : Erase(TypeOf(id));
        // A field of an element of `soa` storage: the arrays are per
        // field, so where it is depends on how many records there are.
        if (const Place element = SoaElement(expr.lhs);
            element.pointer.is_valid()) {
          Place field = element;
          // The step already says which element; this adds which field.
          // `soa_record` stays what `SoaElement` set it to -- the record
          // -- because that is what the offsets are computed from.
          field.path.back().field =
              FieldIndex(element.soa_record, expr.name);
          field.path.back().type = field_type;
          return field;
        }
        if (types_->IsHandle(owner)) {
          // The base is a class, so it is already an address: the field is
          // read and written through it rather than through a copy.
          place.pointer = Value(expr.lhs);
          place.root_type = owner;
          place.path.push_back(Step{FieldIndex(owner, expr.name), InstId{},
                                    InstId{}, field_type});
          return place;
        }
        // A field through an address: a reference is a pointer here
        // (D109), and reading a field of what it refers to is the same
        // walk a class gets -- the address is the value, and the field is
        // an offset into it.
        if (types_->KindOf(owner) == TypeKind::kPointer) {
          const TypeId referenced = Erase(types_->ElementOf(owner));
          if (FindAnyMember(referenced, expr.name) != nullptr) {
            // A reference to a class refers to the *handle*, so the
            // handle is read out of it first; a reference to a value type
            // already is the address its fields are at.
            place.pointer = types_->IsHandle(referenced)
                                ? builder_->Add(Opcode::kLoad, referenced,
                                                {Value(expr.lhs)})
                                : Value(expr.lhs);
            place.root_type = referenced;
            place.path.push_back(Step{FieldIndex(referenced, expr.name),
                                      InstId{}, InstId{}, field_type});
            return place;
          }
        }
        Place base = PlaceOf(expr.lhs);
        if (!base.readable()) {
          return place;
        }
        base.path.push_back(Step{FieldIndex(TypeAt(base), expr.name),
                                 InstId{}, InstId{}, field_type});
        return base;
      }
      case ExprKind::kIndex: {
        // A range makes a view rather than naming a place.
        if (ast().Contains(expr.rhs) &&
            ast().Get(expr.rhs).kind == ExprKind::kRange) {
          return place;
        }
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

  // `xs[a..b]`: a view of part of a sequence (D93).
  //
  // The pointer is where element `a` is and the length is `b - a`, so a
  // slice of a slice is two words of arithmetic and no copy. A slice may
  // start at the end -- an empty tail is a useful thing to ask for -- so
  // the bound for both ends is one past the length, which is what
  // `index.check` compares against.
  InstId LowerSliceRange(const Expr& expr, TypeId type) {
    const TypeId sequence = Erase(TypeOf(expr.lhs));
    const TypeId element = Erase(types_->ElementOf(sequence));
    const TypeId slice = Erase(type);
    const TypeId pointer = types_->Pointer(element, /*is_mutable=*/true);

    // Where the elements are, and how many there are.
    InstId data;
    InstId length;
    if (IsView(sequence)) {
      const InstId view = Value(expr.lhs);
      data = builder_->Extract(pointer, view, 0);
      length = builder_->Extract(types_->default_int(), view, 1);
    } else {
      Place base = PlaceOf(expr.lhs);
      InstId storage;
      if (base.addressable()) {
        storage = AddressOfPlace(base);
      } else {
        storage = builder_->Add(Opcode::kAlloca, sequence);
        builder_->Add(Opcode::kStore, TypeId{}, {storage, Value(expr.lhs)});
      }
      length = builder_->ConstInt(
          types_->default_int(),
          static_cast<uint64_t>(types_->Get(sequence).length));
      const InstId zero = builder_->ConstInt(types_->default_int(), 0);
      data = builder_->IndexAddress(pointer, storage, zero, length);
    }

    const Expr& range = ast().Get(expr.rhs);
    const InstId begin = ast().Contains(range.lhs)
                             ? Value(range.lhs)
                             : builder_->ConstInt(types_->default_int(), 0);
    const InstId end =
        ast().Contains(range.rhs) ? Value(range.rhs) : length;

    // One past the length: a view may end at the end, and may start
    // there too.
    const InstId one = builder_->ConstInt(types_->default_int(), 1);
    const InstId limit = builder_->Add(Opcode::kAdd, types_->default_int(),
                                       {length, one});
    builder_->Mutable(limit).is_unchecked = true;
    builder_->Add(Opcode::kIndexCheck, TypeId{}, {end, limit});
    const InstId after_begin = builder_->Add(
        Opcode::kAdd, types_->default_int(), {end, one});
    builder_->Mutable(after_begin).is_unchecked = true;
    builder_->Add(Opcode::kIndexCheck, TypeId{}, {begin, after_begin});

    // The first element of the view, reached without a check of its own:
    // the two above are what says it is in range.
    const InstId first =
        builder_->IndexAddress(pointer, data, begin, limit);
    builder_->Mutable(first).is_unchecked = true;
    const InstId count = builder_->Add(Opcode::kSub,
                                       types_->default_int(), {end, begin});
    builder_->Mutable(count).is_unchecked = true;
    // A view's length is a word, the way every other view's is: the two
    // halves of a slice are a pointer and a count, and the count is what
    // the runtime and the other tier expect to find there.
    const InstId word =
        builder_->Add(Opcode::kCast, types_->u64(), {count});
    return builder_->Add(Opcode::kMakeStruct, slice, {first, word});
  }

  // An element of storage laid out one array per field, as a place with
  // room for the field the caller is about to ask for.
  Place SoaElement(ExprId id) {
    Place place;
    if (!ast().Contains(id)) {
      return place;
    }
    const Expr& expr = ast().Get(id);
    if (expr.kind != ExprKind::kIndex) {
      return place;
    }
    const TypeId storage = Erase(TypeOf(expr.lhs));
    if (types_->KindOf(storage) != TypeKind::kSlice ||
        !types_->Get(storage).is_soa) {
      return place;
    }
    const TypeId record = Erase(types_->ElementOf(storage));
    // Typed as a pointer to the record rather than to bytes: that is how
    // the tiers know *which* record's fields they are spacing out, and
    // there is nothing else in the instruction that could tell them.
    const InstId view = Value(expr.lhs);
    place.pointer = builder_->Extract(
        types_->Pointer(record, /*is_mutable=*/true), view, 0);
    place.root_type = record;
    place.soa_record = record;
    place.path.push_back(Step{0, Value(expr.rhs),
                              LengthOf(expr.lhs, storage), record});
    return place;
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
    const Member* member = resolution().FindMember(self_type_, name);
    if (member == nullptr) {
      // A field the class inherits: declared by a base, stored in this
      // object, and named without any qualification inside a method.
      member = resolution().FindInheritedMember(self_type_, name);
    }
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
    for (const Member& member : resolution().MembersOf(owner)) {
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
    if (place.soa_record.is_valid() && !place.path.empty()) {
      // One step, two coordinates: which element and which field. The
      // tiers work out where that is, because only they know how big a
      // field is.
      const Step& step = place.path.back();
      const InstId result = builder_->Add(
          Opcode::kSoaAddress, types_->Pointer(Erase(step.type), true),
          {address, step.index, step.length});
      module_->functions[current_function_].Get(result).field = step.field;
      return result;
    }
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
    const std::span<const Member> members = resolution().MembersOf(owner);
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
    const std::span<const Member> members = resolution().MembersOf(owner);
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
    const Place place = PlaceOf(expr.lhs);
    // What goes into a place is of the place's type: `p = &xs[0]` makes
    // a pointer that may be written and the place may hold one that may
    // not, and a slot whose stores disagree with it is one the passes
    // then promote into a block parameter that disagrees too (D153).
    const TypeId held = TypeAt(place);
    WritePlace(place,
               held.is_valid()
                   ? Coerce(value, ValueType(value), held, expr.rhs)
                   : value);
    // An assignment is a statement, so it produces nothing a caller can use.
    return InstId{};
  }

  InstId LowerWait(const Expr& expr, TypeId type, Opcode mark) {
    if (!ast().Contains(expr.lhs) ||
        ast().Get(expr.lhs).kind != ExprKind::kCall) {
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
        ast().Contains(expr.lhs) ? TypeOf(expr.lhs) : TypeId{};
    const std::vector<TypeId> params =
        types_->KindOf(callee_type) == TypeKind::kFunction
            ? types_->ArgumentsOf(callee_type)
            : std::vector<TypeId>();

    std::vector<InstId> arguments;
    const std::span<const ExprId> written = ast().Get(expr.args);
    for (size_t i = 0; i < written.size(); ++i) {
      const InstId value = Value(written[i]);
      arguments.push_back(i < params.size()
                              ? Coerce(value, TypeOf(written[i]), params[i],
                                       written[i])
                              : value);
    }

    // `AiState.Chase(target)`: a variant that carries values. The tag
    // says which one and the payload sits beside it (D89).
    if (const InstId built = LowerVariantCall(expr, type, arguments);
        built.is_valid()) {
      return built;
    }

    // `s.Connect(handler)` and `s.Emit(args)`: a signal's two operations.
    if (const InstId done = LowerSignalCall(expr, arguments);
        done.is_valid()) {
      return done;
    }

    // `Free(items)` gives the storage back. Not a call either: what a
    // view's storage is depends on how the tier laid it out.
    if (ast().Contains(expr.lhs) &&
        ast().Get(expr.lhs).kind == ExprKind::kPath &&
        interner_->Text(ast().Get(expr.lhs).name) == "Free" &&
        arguments.size() == 1 &&
        types_->KindOf(Erase(TypeOf(ast().Get(expr.args).empty()
                                        ? ExprId{}
                                        : ast().Get(expr.args)[0]))) ==
            TypeKind::kSlice) {
      const TypeId word = types_->u64();
      const InstId data = builder_->Extract(word, arguments[0], 0);
      const Symbol name = interner_->Intern("wolang_object_free");
      const uint32_t free = DeclareExternal(
          name, types_->Function({word}, types_->void_type()));
      module_->functions[free].foreign_symbol = name;
      return builder_->Call(free, types_->void_type(),
                            {AsDeclared(free, 0, data)});
    }

    // `RunSystems(world, delta)` is not a call either: what it stands for
    // is this file's schedule, which only the compiler knows. Every
    // stage's systems are handed to the runtime together, because a stage
    // is exactly a set that may run at the same time (D101).
    if (ast().Contains(expr.lhs) &&
        ast().Get(expr.lhs).kind == ExprKind::kPath &&
        interner_->Text(ast().Get(expr.lhs).name) == "RunSystems" &&
        arguments.size() == 2) {
      const Binding* binding = resolution().Find(ast().Get(expr.lhs).name);
      if (binding == nullptr || !binding->has_body) {
        return RunSchedule(arguments[0], arguments[1]);
      }
    }

    // `WrappingAdd`, `WrappingSub` and `WrappingMul` are the arithmetic
    // that does not trap.
    //
    // Everything else in this language traps on overflow, which is right
    // for a program counting damage and wrong for one hashing a key or
    // stepping a generator: those *want* the low bits of a wrapped
    // result. Not a call, because what they stand for is the instruction
    // the compiler already writes for its own address arithmetic (D107).
    if (ast().Contains(expr.lhs) &&
        ast().Get(expr.lhs).kind == ExprKind::kPath &&
        arguments.size() == 2) {
      const std::string_view called =
          interner_->Text(ast().Get(expr.lhs).name);
      Opcode wrapping = Opcode::kUndef;
      if (called == "WrappingAdd") {
        wrapping = Opcode::kAdd;
      } else if (called == "WrappingSub") {
        wrapping = Opcode::kSub;
      } else if (called == "WrappingMul") {
        wrapping = Opcode::kMul;
      }
      const Binding* binding = resolution().Find(ast().Get(expr.lhs).name);
      if (wrapping != Opcode::kUndef &&
          (binding == nullptr || !binding->has_body)) {
        const TypeId word = Erase(type);
        const InstId answer = builder_->Add(
            wrapping, word, {Widen(arguments[0], word),
                             Widen(arguments[1], word)});
        builder_->Mutable(answer).is_unchecked = true;
        return answer;
      }
    }

    // `SizeOf(value)` is not a call: how many bytes a value of its type
    // takes is a question only a tier can answer, so the instruction
    // carries the type and each tier answers it. The value itself is not
    // read -- what is wanted is its type, which is why this is spelled
    // with an argument at all (D100).
    if (ast().Contains(expr.lhs) &&
        ast().Get(expr.lhs).kind == ExprKind::kPath &&
        interner_->Text(ast().Get(expr.lhs).name) == "SizeOf" &&
        arguments.size() == 1) {
      const Binding* binding = resolution().Find(ast().Get(expr.lhs).name);
      if ((binding == nullptr || !binding->has_body) &&
          !ast().Get(expr.args).empty()) {
        // Through the substitution in force: inside `Add<T>` the
        // argument's recorded type is `T`, and what is wanted is the size
        // of what `T` stands for in this copy (D100).
        const TypeId measured =
            Erase(Apply(TypeOf(ast().Get(expr.args)[0])));
        if (measured.is_valid()) {
          const InstId size =
              builder_->Add(Opcode::kTypeSize,
                            types_->Int(IntWidth::k64, /*is_signed=*/true));
          builder_->Mutable(size).payload.integer = measured.index;
          return size;
        }
      }
    }

    // `TokenOf<T>()` is not a call: the number that says which type `T`
    // is, is the one every tier computes the same way -- the declaration
    // it was written as, packed (D85). The result type is what carries
    // `T`, which is why it is a `Token<T>` rather than a bare number.
    if (ast().Contains(expr.lhs) &&
        ast().Get(expr.lhs).kind == ExprKind::kPath &&
        interner_->Text(ast().Get(expr.lhs).name) == "TokenOf" &&
        arguments.empty()) {
      const Binding* binding = resolution().Find(ast().Get(expr.lhs).name);
      const TypeId wrapper = Erase(Apply(type));
      const std::vector<TypeId> named = types_->ArgumentsOf(wrapper);
      if ((binding == nullptr || !binding->has_body) && !named.empty()) {
        const TypeId word = types_->Int(IntWidth::k64, /*is_signed=*/false);
        const InstId number = builder_->ConstInt(
            word, ir::ClassTokenOf(*types_, Erase(Apply(named[0]))));
        return builder_->Add(Opcode::kMakeStruct, wrapper, {number});
      }
    }

    // `SoaNew<T>(count)` is not a call either: how big that storage is, is
    // a question only a tier can answer.
    if (ast().Contains(expr.lhs) &&
        ast().Get(expr.lhs).kind == ExprKind::kPath &&
        (interner_->Text(ast().Get(expr.lhs).name) == "SoaNew" ||
         interner_->Text(ast().Get(expr.lhs).name) == "ArrayNew") &&
        arguments.size() == 1 &&
        types_->KindOf(Unwrapped(type)) == TypeKind::kSlice) {
      const TypeId word = types_->Int(IntWidth::k64, /*is_signed=*/false);
      const InstId count = Widen(arguments[0], word);
      const TypeId made = Unwrapped(type);
      return Coerce(builder_->Add(Opcode::kSoaNew, made, {count}), made,
                    type);
    }

    // `ParallelFor(count, threads, body)` hands the runtime the two
    // halves of the body's function value: the code, and what it closed
    // over. Which is a thing only lowering can do -- the language has no
    // way to take a function value apart, and should not (D147).
    if (ast().Contains(expr.lhs) &&
        ast().Get(expr.lhs).kind == ExprKind::kPath &&
        interner_->Text(ast().Get(expr.lhs).name) == "ParallelFor" &&
        arguments.size() == 3) {
      const TypeId word = types_->u64();
      const TypeId count = types_->Int(IntWidth::k64, /*is_signed=*/true);
      const TypeId threads = types_->Int(IntWidth::k32, /*is_signed=*/true);
      const InstId code = builder_->Extract(word, arguments[2], 0);
      const InstId environment =
          builder_->Extract(word, arguments[2], 1);
      const Symbol name = interner_->Intern("wolang_parallel_for");
      const uint32_t runtime = DeclareExternal(
          name,
          types_->Function({word, word, count, threads},
                           types_->void_type()));
      module_->functions[runtime].foreign_symbol = name;
      return builder_->Call(
          runtime, types_->void_type(),
          {code, environment, Widen(arguments[0], count),
           Coerce(arguments[1], ValueType(arguments[1]), threads)});
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
    if (const InstId called = LowerMethodCall(expr, type, &arguments, id);
        called.is_valid()) {
      return called;
    }

    if (ast().Contains(expr.lhs) &&
        ast().Get(expr.lhs).kind == ExprKind::kPath) {
      const Symbol name = ast().Get(expr.lhs).name;
      const ItemId generic = FindGeneric(name);
      if (generic.is_valid()) {
        const uint32_t specialization = Instantiate(generic, id);
        if (specialization != 0xFFFFFFFFu) {
          return CallAs(specialization, type, std::move(arguments));
        }
        // Inference did not reach a concrete type for every parameter,
        // which only happens in a body that is itself generic and not
        // instantiated. There is nothing to call.
        return builder_->Undef(type);
      }
      // A generic from another module: the copy is made here, because the
      // module it was written in cannot know what its importers will ask
      // for.
      if (const Imported imported = FindImportedGeneric(name);
          imported.ast != nullptr) {
        const uint32_t specialization = Instantiate(
            imported.item, id,
            Context{imported.file, imported.resolution, imported.ast,
                    imported.typed, {}});
        if (specialization != 0xFFFFFFFFu) {
          return CallAs(specialization, type, std::move(arguments));
        }
      }
      const uint32_t callee = IndexOfName(name);
      if (callee != 0xFFFFFFFFu) {
        return CallAs(callee, type, std::move(arguments));
      }

      const Binding* binding = resolution().Find(name);
      // A constructor: the callee is a type, and the arguments are its
      // fields in order.
      if (binding != nullptr && binding->kind == BindingKind::kType) {
        // The checker records the type the context wanted, so `type` may
        // already be `Pawn?` for a `Pawn(...)`. What the instruction builds
        // is the object, and the wrap is what bridges the two.
        //
        // Read from what the checker recorded for the callee rather than
        // from the declaration: `Box<int>(7)` builds a `Box<s32>`, and the
        // declaration is still `Box` with a parameter in its field.
        const TypeId recorded = TypeOf(expr.lhs);
        const TypeId built = types_->IsError(recorded)
                                 ? Erase(binding->type)
                                 : recorded;
        if (types_->IsHandle(built)) {
          return Coerce(NewObject(built, arguments), built, type);
        }
        const InstId value = builder_->Add(
            Opcode::kMakeStruct, built.is_valid() ? built : type,
            std::move(arguments));
        return Coerce(value, built, type);
      }
      // A function from another file. Lowering is per file, so it gets a
      // declaration here -- the same shape an ability method or an `extern`
      // has, and what the linker will resolve.
      if (binding != nullptr && binding->kind == BindingKind::kFunction) {
        const uint32_t target =
            DeclareExternal(name, binding->type, binding->file,
                            binding->item, binding->is_task,
                            binding->has_body);
        // An `extern` from another file is reached through the C ABI
        // here too. Without this the declaration linked to nothing --
        // the file it came from has no body for it either -- and a
        // generic instantiated into this file that called one trapped
        // with "no body to run" (D100).
        if (binding->is_foreign &&
            !module_->functions[target].foreign_symbol.is_valid()) {
          module_->functions[target].foreign_symbol = name;
          module_->functions[target].type =
              ForeignSignature(module_->functions[target].type);
        }
        return CallAs(target, type, std::move(arguments));
      }
    }
    // Through a value: the callee is whatever the expression evaluated to.
    if (types_->KindOf(callee_type) == TypeKind::kFunction) {
      const InstId target = Value(expr.lhs);
      if (target.is_valid()) {
        return CallThroughValue(target, std::move(arguments), type);
      }
    }
    return builder_->Undef(type);
  }

  // A call through a function value.
  //
  // The value is a pair: the code, and the environment it closed over.
  // A closure's body takes that environment as its first argument and a
  // plain function does not, so which call to make is a question about
  // the value -- asked here, in the IR, so that every tier gets the
  // answer from the one place that knows it (D138).
  InstId CallThroughValue(InstId target,
                          std::vector<InstId> arguments,
                          TypeId type) {
    const TypeId word = types_->u64();
    const InstId code = builder_->Extract(word, target, 0);
    const InstId environment = builder_->Extract(word, target, 1);
    const InstId none = builder_->ConstInt(word, 0);
    const InstId plain = builder_->Add(Opcode::kEq, types_->bool_type(),
                                       {environment, none});

    const BlockId bare = builder_->AddBlock("call.bare");
    const BlockId closed = builder_->AddBlock("call.closed");
    const BlockId join = builder_->AddBlock("call.join");
    const bool produces = type.is_valid() &&
                          types_->KindOf(Erase(type)) != TypeKind::kVoid;
    const InstId result =
        produces ? builder_->AddParam(join, Erase(type)) : InstId{};
    builder_->Branch(plain, bare, closed);

    builder_->SetCurrent(bare);
    std::vector<InstId> without;
    without.push_back(code);
    for (InstId argument : arguments) {
      without.push_back(argument);
    }
    const InstId answered =
        builder_->Add(Opcode::kCallIndirect, Erase(type), without);
    builder_->Jump(join, produces ? std::vector<InstId>{answered}
                                  : std::vector<InstId>{});

    builder_->SetCurrent(closed);
    std::vector<InstId> with;
    with.push_back(code);
    with.push_back(environment);
    for (InstId argument : arguments) {
      with.push_back(argument);
    }
    const InstId closed_answer =
        builder_->Add(Opcode::kCallIndirect, Erase(type), with);
    builder_->Jump(join, produces ? std::vector<InstId>{closed_answer}
                                  : std::vector<InstId>{});

    builder_->SetCurrent(join);
    return produces ? Coerce(result, Erase(type), type) : InstId{};
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
      const Binding* binding = resolution().Find(name);
      if (binding == nullptr || binding->kind != BindingKind::kFunction) {
        return InstId{};
      }
      callee = DeclareExternal(name, binding->type, binding->file,
                               binding->item, binding->is_task,
                               binding->has_body);
    }
    const InstId code = builder_->Add(Opcode::kFuncAddress, types_->u64());
    module_->functions[current_function_].Get(code).payload.callee = callee;
    return builder_->Add(Opcode::kMakeStruct, Erase(type),
                         {code, builder_->ConstInt(types_->u64(), 0)});
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
    if (member.file == file_index()) {
      return 0xFFFFFFFFu;
    }
    return DeclareExternal(DerivedName(owner, member.name),
                           types_->Function({owner}, Erase(member.type)),
                           member.file, member.item);
  }

  // Where the bit for |member| sits among the type's storage.
  uint32_t ReadyIndex(TypeId owner, const Member& member) {
    uint32_t index = 0;
    for (const Member& candidate : resolution().MembersOf(owner)) {
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
    for (const auto& [constant, index, file] : constants_) {
      if (constant == name && file == file_index()) {
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
    const Binding* binding = resolution().Find(name);
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
    if (arguments.size() != 2 || !ast().Contains(expr.lhs) ||
        ast().Get(expr.lhs).kind != ExprKind::kPath) {
      return InstId{};
    }
    const std::string_view called = interner_->Text(ast().Get(expr.lhs).name);
    // `TextFromRaw` is the same instruction with a different name: a
    // `text` is a pointer and a length too (D67), so what both stand for
    // is the pair rather than a call.
    // `ViewFromRaw<T>` is the same instruction with the element type
    // coming from the call rather than being `u8`: what all three stand
    // for is the pair, not a call (D100).
    // `Prefix<T>(items, count)` is the same pair again, with the
    // pointer taken from a view the caller already has: a container with
    // room to spare hands out the part that is in use (D138).
    if (called == "Prefix") {
      const TypeId slice = Unwrapped(type);
      if (types_->KindOf(slice) != TypeKind::kSlice) {
        return InstId{};
      }
      const TypeId word = types_->Int(IntWidth::k64, /*is_signed=*/false);
      const TypeId pointer = types_->Pointer(
          types_->Int(IntWidth::k8, /*is_signed=*/false),
          /*is_mutable=*/true);
      const InstId data = builder_->Extract(pointer, arguments[0], 0);
      return Coerce(builder_->Add(Opcode::kMakeStruct, slice,
                                  {data, Widen(arguments[1], word)}),
                    slice, type);
    }
    if (called != "BytesFromRaw" && called != "TextFromRaw" &&
        called != "ViewFromRaw") {
      return InstId{};
    }
    // What the pair *is*, not what the context wanted: a call whose
    // result is used where a `text?` is expected is recorded as `text?`,
    // and reading that as the result type made the intrinsic
    // unrecognisable -- so it lowered to a call with no body (D111).
    const TypeId slice = Unwrapped(type);
    if (types_->KindOf(slice) != TypeKind::kSlice &&
        types_->KindOf(slice) != TypeKind::kStr) {
      return InstId{};
    }
    const Binding* binding = resolution().Find(ast().Get(expr.lhs).name);
    if (binding != nullptr && binding->has_body) {
      // Somebody wrote their own function of that name, and theirs wins.
      return InstId{};
    }
    const TypeId word = types_->Int(IntWidth::k64, /*is_signed=*/false);
    return Coerce(builder_->Add(Opcode::kMakeStruct, slice,
                                {arguments[0], Widen(arguments[1], word)}),
                  slice, type);
  }

  // What a signal's `Connect`, `Disconnect` or `Emit` becomes.
  //
  // Connecting is a call into the runtime, because growing a list is the
  // one part that allocates. Emitting is *not*: calling back into the
  // language is something only the language can do, so the loop over the
  // handlers is emitted here.
  InstId LowerSignalCall(const Expr& expr,
                         const std::vector<InstId>& arguments) {
    if (!ast().Contains(expr.lhs)) {
      return InstId{};
    }
    const Expr& callee = ast().Get(expr.lhs);
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
      // The code, not the pair: the runtime's list holds one word per
      // handler, and a closure that captured a frame cannot outlive it
      // -- which is what the checker refuses to let happen (D138).
      const InstId code = builder_->Extract(word, arguments[0], 0);
      return RuntimeCall(connect ? "wolang_signal_connect"
                                 : "wolang_signal_disconnect",
                         types_->void_type(), {address, code});
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

    // One word per entry, which is what was connected.
    const TypeId handler = word;
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
    if (!ast().Contains(id)) {
      return place;
    }
    const Expr& expr = ast().Get(id);
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
    const Member* signal = resolution().FindMember(owner, name);
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
    for (const Member& candidate : resolution().MembersOf(owner)) {
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
    for (const Member& candidate : resolution().MembersOf(owner)) {
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
  // A call whose value is what the *callee* returns, adapted to what the
  // context wanted.
  //
  // The checker records the type the context wanted at an expression, so
  // a call whose result is used where a `T?` is expected is recorded as
  // `T?` while the function returns a `T`. Taking the recorded type as
  // the call's own made the IR disagree with the function it was calling
  // (D111) -- and the wrap that bridges the two belongs where every other
  // conversion is.
  // A C function's signature as the IR has to state it.
  //
  // A function value is two words here and one in C, so a foreign
  // declaration that takes a callback takes an address: the pair's code
  // half, which is what `ForC` hands it (D138).
  TypeId ForeignSignature(TypeId type) {
    if (!type.is_valid() ||
        types_->KindOf(Erase(type)) != TypeKind::kFunction) {
      return type;
    }
    const TypeId word = types_->u64();
    std::vector<TypeId> params;
    bool changed = false;
    for (const TypeId held : types_->ArgumentsOf(Erase(type))) {
      if (types_->KindOf(Erase(held)) == TypeKind::kFunction) {
        params.push_back(word);
        changed = true;
      } else {
        params.push_back(Erase(held));
      }
    }
    TypeId result = Erase(types_->ElementOf(Erase(type)));
    if (result.is_valid() &&
        types_->KindOf(result) == TypeKind::kFunction) {
      result = word;
      changed = true;
    }
    return changed ? types_->Function(params, result) : type;
  }

  // What a foreign call sees of a function value: its code address.
  //
  // A C function takes a function pointer, which is one word -- so the
  // pair a function value is here crosses as the half C understands. A
  // closure that captured something cannot cross at all, and the
  // checker is what stops it (D138).
  std::vector<InstId> ForC(uint32_t callee, std::vector<InstId> args) {
    if (callee >= module_->functions.size()) {
      return args;
    }
    const Function& target = module_->functions[callee];
    if (!target.is_foreign()) {
      return args;
    }
    // Decided by what is being *passed*, not by what the declaration
    // says it takes: the declaration already says `u64`, because that is
    // what a C function takes (see `ForeignSignature`).
    const TypeId word = types_->u64();
    for (size_t i = 0; i < args.size(); ++i) {
      if (args[i].is_valid() &&
          types_->KindOf(Erase(ValueType(args[i]))) ==
              TypeKind::kFunction) {
        args[i] = builder_->Extract(word, args[i], 0);
      }
    }
    return args;
  }

  InstId CallAs(uint32_t callee, TypeId wanted, std::vector<InstId> args) {
    const TypeId declared =
        callee < module_->functions.size()
            ? Erase(types_->ElementOf(module_->functions[callee].type))
            : TypeId{};
    const TypeId result = declared.is_valid() ? declared : Erase(wanted);
    const InstId value =
        builder_->Call(callee, result, ForC(callee, std::move(args)));
    return Coerce(value, result, wanted);
  }

  InstId LowerMethodCall(const Expr& expr,
                         TypeId type,
                         std::vector<InstId>* arguments,
                         ExprId id = ExprId{}) {
    if (!ast().Contains(expr.lhs)) {
      return InstId{};
    }
    const Expr& callee = ast().Get(expr.lhs);

    TypeId owner;
    Symbol name;
    InstId receiver;
    if (callee.kind == ExprKind::kField) {
      owner = Erase(TypeOf(callee.lhs));
      name = callee.name;
      {
        // The receiver is read through, not handed away.
        const Borrowing borrowing(this);
        receiver = Value(callee.lhs);
      }
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

    // A method on a reference is the method of what it refers to (D109).
    //
    // A reference is a pointer by the time it gets here (see `Erase`).
    // What the receiver *is* depends on the type: a reference to a class
    // holds the handle, so the handle is loaded out of it; a reference to
    // a value type already is the address a method of one takes.
    if (types_->KindOf(owner) == TypeKind::kPointer) {
      const TypeId referenced = Erase(types_->ElementOf(owner));
      if (types_->IsHandle(referenced)) {
        receiver = builder_->Add(Opcode::kLoad, referenced, {receiver});
      }
      owner = referenced;
    }

    const Member* member = resolution().FindMember(owner, name);
    if (member == nullptr) {
      // A method the class inherits and does not override.
      member = resolution().FindInheritedMember(owner, name);
    }
    if (member == nullptr || member->kind != MemberKind::kMethod) {
      return InstId{};
    }

    // A call on an ability value: the pair says what to call it on and which
    // class that is, and the method's own declaration says which method
    // (D86). Nothing about the implementing type is known here, which is
    // the point.
    if (resolution().IsAbility(owner)) {
      const InstId pointer =
          builder_->Add(Opcode::kExtract, types_->u64(), {receiver});
      module_->functions[current_function_].Get(pointer).field = 0;
      const InstId token =
          builder_->Add(Opcode::kExtract, types_->u64(), {receiver});
      module_->functions[current_function_].Get(token).field = 1;

      std::vector<InstId> operands;
      operands.push_back(token);
      operands.push_back(pointer);
      for (InstId argument : *arguments) {
        operands.push_back(argument);
      }
      const InstId call =
          builder_->Add(Opcode::kVirtualCall, type, operands);
      module_->functions[current_function_].Get(call).payload.integer =
          MemberSelector(*member);
      return call;
    }

    // A method with type parameters of its own is a recipe wherever it
    // lives: one copy per set of arguments, asked for before the
    // declaration is, because the declaration has no body (D119).
    const std::vector<TypeId> own = MethodParameters(owner, *member);
    uint32_t index = 0xFFFFFFFFu;
    if (!own.empty()) {
      index = InstantiateMethod(owner, name, *member, id);
    }
    if (index == 0xFFFFFFFFu) {
      index = IndexOfSource(member->file, member->item, member->slot);
    }
    if (index == 0xFFFFFFFFu && types_->KindOf(owner) == TypeKind::kInstance) {
      // A method of a generic type gets one copy per instantiation, the
      // same way a generic function does -- and from the same place: the
      // receiver's own arguments say what its parameters stand for, so
      // there is nothing to infer.
      index = InstantiateMethod(owner, name, *member, id);
    }
    if (index == 0xFFFFFFFFu) {
      if (member->file == file_index()) {
        // Declared here and not lowered: a generic the interpreter keeps.
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

    // A virtual method is found through the object rather than through the
    // name: the receiver's static type says *which slot*, and the token
    // the object carries says which body is in it. Which is the whole
    // difference between `virtual` and not.
    if (member->is_virtual || member->is_override) {
      const uint64_t selector = SelectorOf(owner, name);
      if (selector != 0) {
        // The token is the first word of the object, read here rather
        // than inside the call so that every tier sees one shape: the
        // token, the receiver, then the arguments.
        const InstId address = builder_->FieldAddress(
            types_->Pointer(types_->u64(), /*is_mutable=*/false), receiver,
            0);
        const InstId token =
            builder_->Add(Opcode::kLoad, types_->u64(), {address});
        std::vector<InstId> operands;
        operands.push_back(token);
        for (InstId operand : with_receiver) {
          operands.push_back(operand);
        }
        const InstId call =
            builder_->Add(Opcode::kVirtualCall, type, operands);
        module_->functions[current_function_].Get(call).payload.integer =
            selector;
        return call;
      }
    }
    return CallAs(index, type, std::move(with_receiver));
  }

  InstId LowerBlockValue(ExprId id, const Expr& expr, TypeId type) {
    const size_t depth = locals_.size();
    ++scope_depth_;
    const std::span<const StmtId> stmts = ast().Get(expr.stmts);
    InstId result;
    for (size_t i = 0; i < stmts.size(); ++i) {
      const bool last = i + 1 == stmts.size();
      if (last && ast().Contains(stmts[i]) &&
          ast().Get(stmts[i]).kind == StmtKind::kExpr) {
        result = Value(ast().Get(stmts[i]).expr);
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
    // The join's parameter is erased like every other type in the IR: a
    // variable the checker typed as an owned string and an arm that
    // yields the two words one is are the same type here, and leaving
    // the parameter unerased made them differ (D127).
    const InstId result = produces
                              ? builder_->AddParam(join, Erase(type))
                              : InstId{};

    builder_->Branch(condition, yes, no);

    builder_->SetCurrent(yes);
    const size_t depth = locals_.size();
    if (subject.is_valid() && ast().Contains(expr.pattern)) {
      const Pattern& pattern = ast().Get(expr.pattern);
      if (pattern.kind == PatternKind::kBinding) {
        const TypeId inner = Erase(types_->ElementOf(TypeOf(expr.lhs)));
        const InstId value =
            builder_->Add(Opcode::kOptionalValue, inner, {subject});
        locals_.push_back(Local{pattern.name, InstId{}, value, inner});
      }
    }
    // `Value` lowers the block and yields its final expression; `Lower`
    // does the same without a result. Calling both would run the body twice.
    bool reached = false;
    InstId then_value = produces ? Value(expr.rhs) : InstId{};
    if (!produces) {
      Lower(expr.rhs);
    }
    // What an arm produces is whatever produced it, and the join's
    // parameter is what the expression's type says: a branch yielding a
    // borrowed string where the variable is an owned one needs the
    // conversion written down, not assumed (D127).
    if (produces && then_value.is_valid()) {
      then_value = Coerce(then_value, ValueType(then_value), type);
    }
    if (!builder_->Terminated()) {
      reached = true;
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
    if (produces && else_value.is_valid()) {
      else_value = Coerce(else_value, ValueType(else_value), type);
    }
    if (!builder_->Terminated()) {
      reached = true;
      builder_->Jump(join, produces ? std::vector<InstId>{else_value}
                                    : std::vector<InstId>{});
    }

    builder_->SetCurrent(join);
    if (!reached) {
      // No arm fell through, so nothing reaches the join and nothing
      // should be put in it (D152).
      builder_->Poison();
    }
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

    // Made when something needs it rather than up front: a `match` whose
    // every arm returns has nowhere to come back to, and a block nothing
    // reaches is a block the verifier is right to complain about.
    BlockId join;
    InstId result;
    const auto join_block = [&]() {
      if (!join.is_valid()) {
        join = builder_->AddBlock("match.join");
        if (produces) {
          result = builder_->AddParam(join, Erase(type));
        }
      }
      return join;
    };

    // Each arm gets a block; the arms are tested in order, so each test
    // needs somewhere to go when it fails.
    struct Arm {
      MatchArmId id;
      BlockId body;
      BlockId next;
      bool catch_all = false;
    };
    std::vector<Arm> arms;
    const std::span<const MatchArmId> arm_ids = ast().Get(expr.arms);
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
      arm.catch_all = IsCatchAll(arm_ids[i], subject_type);
      arms.push_back(arm);
    }

    for (size_t i = 0; i < arms.size(); ++i) {
      const MatchArm& arm = ast().Get(arms[i].id);
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
          builder_->Jump(join_block(), {value});
        }
      } else {
        Lower(arm.body);
        if (!builder_->Terminated()) {
          builder_->Jump(join_block());
        }
      }
      locals_.resize(depth);

      if (otherwise.is_valid()) {
        builder_->SetCurrent(otherwise);
      }
    }

    if (arms.empty()) {
      builder_->Jump(join_block());
    }

    if (join.is_valid()) {
      builder_->SetCurrent(join);
    }
    return result;
  }

  bool IsCatchAll(MatchArmId id, TypeId subject_type) const {
    const MatchArm& arm = ast().Get(id);
    if (arm.guard.is_valid()) {
      return false;
    }
    for (PatternId pattern_id : ast().Get(arm.patterns)) {
      if (!ast().Contains(pattern_id)) {
        continue;
      }
      const Pattern& pattern = ast().Get(pattern_id);
      if (pattern.kind == PatternKind::kWildcard ||
          pattern.kind == PatternKind::kRest) {
        return true;
      }
      // A bare name that is not a variant binds everything -- unless it
      // names one of the errors a `T!` carries, which is a test and not a
      // binding (D90).
      if (pattern.kind == PatternKind::kBinding) {
        if (types_->KindOf(subject_type) == TypeKind::kFallible &&
            IsErrorName(pattern.name)) {
          return false;
        }
        return true;
      }
    }
    return false;
  }

  // What has to be true for |arm| to run. An invalid id means "always".
  InstId ArmTest(const MatchArm& arm, InstId subject, TypeId subject_type) {
    InstId combined;
    for (PatternId pattern_id : ast().Get(arm.patterns)) {
      if (!ast().Contains(pattern_id)) {
        continue;
      }
      const InstId test =
          PatternTest(ast().Get(pattern_id), subject, subject_type);
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

  // Whether |name| is one of the program's error tags.
  bool IsErrorName(Symbol name) const {
    const Binding* binding = resolution().Find(name);
    return binding != nullptr && binding->kind == BindingKind::kErrorTag;
  }

  // The test for one arm of a `match` over a `T!`: zero for `Ok`, and the
  // error's own symbol for an error tag -- the same number the coercion
  // writes (D90).
  InstId FallibleTest(const Pattern& pattern, InstId subject) {
    Symbol name = pattern.name;
    if (const std::span<const Symbol> path = ast().Get(pattern.path);
        !path.empty()) {
      name = path.back();
    }
    const bool is_ok = interner_->Text(name) == "Ok";
    const InstId tag = builder_->Extract(types_->u32(), subject, 0);
    const InstId expected = builder_->ConstInt(
        types_->u32(), is_ok ? 0 : name.index + 1);
    return builder_->Add(Opcode::kEq, types_->bool_type(),
                         {tag, expected});
  }

  InstId PatternTest(const Pattern& pattern,
                     InstId subject,
                     TypeId subject_type) {
    switch (pattern.kind) {
      case PatternKind::kLiteral: {
        if (!ast().Contains(pattern.literal)) {
          return InstId{};
        }
        const Expr& literal = ast().Get(pattern.literal);
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
        // `Ok(x)` on a `T!`: the tag is zero when it is a value (D90).
        if (types_->KindOf(subject_type) == TypeKind::kFallible) {
          return FallibleTest(pattern, subject);
        }
        // A variant is identified by the tag the enum stores first. Reading
        // it as a field keeps the layout decision in one place.
        const InstId tag = builder_->Extract(types_->default_int(), subject,
                                             0);
        const InstId expected = builder_->ConstInt(
            types_->default_int(), VariantIndex(subject_type, pattern));
        return builder_->Add(Opcode::kEq, types_->bool_type(),
                             {tag, expected});
      }

      case PatternKind::kBinding:
        // A bare name that is an error of the set a `T!` carries: not a
        // binding at all, but a test for that error.
        if (types_->KindOf(subject_type) == TypeKind::kFallible &&
            IsErrorName(pattern.name)) {
          return FallibleTest(pattern, subject);
        }
        return InstId{};

      default:
        return InstId{};
    }
  }

  uint64_t VariantIndex(TypeId owner, const Pattern& pattern) const {
    Symbol name = pattern.name;
    const std::span<const Symbol> path = ast().Get(pattern.path);
    if (!path.empty()) {
      name = path.back();
    }
    uint64_t index = 0;
    for (const Member& member : resolution().MembersOf(owner)) {
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
    for (PatternId pattern_id : ast().Get(arm.patterns)) {
      if (!ast().Contains(pattern_id)) {
        continue;
      }
      const Pattern& pattern = ast().Get(pattern_id);
      if (pattern.kind == PatternKind::kBinding &&
          types_->KindOf(subject_type) == TypeKind::kFallible &&
          IsErrorName(pattern.name)) {
        continue;
      }
      if (pattern.kind == PatternKind::kBinding &&
          !IsVariantName(subject_type, pattern.name)) {
        locals_.push_back(
            Local{pattern.name, InstId{}, subject, subject_type});
        continue;
      }
      if (pattern.kind == PatternKind::kVariant &&
          types_->KindOf(subject_type) == TypeKind::kFallible) {
        // `Ok(value)`: the value is the second half of the pair.
        const TypeId held = Erase(types_->ElementOf(subject_type));
        const InstId value = builder_->Extract(held, subject, 1);
        for (PatternId element : ast().Get(pattern.elements)) {
          if (ast().Contains(element) &&
              ast().Get(element).kind == PatternKind::kBinding) {
            locals_.push_back(
                Local{ast().Get(element).name, InstId{}, value, held});
          }
        }
        continue;
      }
      if (pattern.kind == PatternKind::kVariant) {
        // What the variant carries, in the order it was declared: each
        // sub-pattern that is a name binds one of them.
        const uint32_t index = static_cast<uint32_t>(
            VariantIndex(subject_type, pattern));
        const std::span<const PatternId> elements =
            ast().Get(pattern.elements);
        uint32_t position = 0;
        for (PatternId element : elements) {
          if (!ast().Contains(element) ||
              ast().Get(element).kind != PatternKind::kBinding) {
            ++position;
            continue;
          }
          const TypeId carried =
              VariantFieldType(subject_type, index, position);
          const InstId payload = builder_->Add(
              Opcode::kVariantPayload, carried, {subject});
          builder_->Mutable(payload).field = index;
          builder_->Mutable(payload).payload.integer = position;
          locals_.push_back(Local{ast().Get(element).name, InstId{},
                                  payload, carried});
          ++position;
        }
        continue;
      }
      if (pattern.kind == PatternKind::kPresent) {
        const TypeId inner = Erase(types_->ElementOf(subject_type));
        const InstId value =
            builder_->Add(Opcode::kOptionalValue, inner, {subject});
        for (PatternId element : ast().Get(pattern.elements)) {
          if (ast().Contains(element) &&
              ast().Get(element).kind == PatternKind::kBinding) {
            locals_.push_back(
                Local{ast().Get(element).name, InstId{}, value, inner});
          }
        }
      }
    }
  }

  // `expr?` on a `T!`: if it failed, return that failure as this
  // function's; otherwise the value (D90).
  //
  // The early return is what makes `?` worth having -- the alternative is
  // a `match` at every call -- and it is why the error set of a signature
  // is the union of what its body propagates.
  InstId LowerTry(const Expr& expr, TypeId type) {
    const TypeId inner = TypeOf(expr.lhs);
    const InstId subject = Value(expr.lhs);
    if (types_->KindOf(inner) != TypeKind::kFallible) {
      // `?` on an optional, or on something already unwrapped: nothing to
      // do here yet, and the checker has already said what the type is.
      return subject;
    }

    const TypeId held = Erase(types_->ElementOf(inner));
    const InstId tag = builder_->Extract(types_->u32(), subject, 0);
    const InstId zero = builder_->ConstInt(types_->u32(), 0);
    const InstId ok =
        builder_->Add(Opcode::kEq, types_->bool_type(), {tag, zero});

    const BlockId carry = builder_->AddBlock("try.carry");
    const BlockId fail = builder_->AddBlock("try.fail");
    builder_->Branch(ok, carry, fail);

    // The failure, as this function's own: the same tag, and no value.
    builder_->SetCurrent(fail);
    const TypeId returned = Erase(return_type_);
    if (types_->KindOf(returned) == TypeKind::kFallible) {
      const TypeId mine = Erase(types_->ElementOf(returned));
      const InstId again = builder_->Add(
          Opcode::kMakeStruct, returned,
          {tag, builder_->ConstInt(mine, 0)});
      RunDeferred(0);
      builder_->Return(again);
    } else {
      builder_->Unreachable();
    }

    builder_->SetCurrent(carry);
    return Coerce(builder_->Extract(held, subject, 1), held, type);
  }

  // `Enum.Variant(values)`, or an invalid id when the call is not one.
  InstId LowerVariantCall(const Expr& expr,
                          TypeId type,
                          const std::vector<InstId>& arguments) {
    if (!ast().Contains(expr.lhs)) {
      return InstId{};
    }
    const Expr& callee = ast().Get(expr.lhs);
    if (callee.kind != ExprKind::kField || !ast().Contains(callee.lhs) ||
        ast().Get(callee.lhs).kind != ExprKind::kPath) {
      return InstId{};
    }
    const Binding* owner_binding =
        resolution().Find(ast().Get(callee.lhs).name);
    if (owner_binding == nullptr ||
        owner_binding->kind != BindingKind::kType) {
      return InstId{};
    }
    const TypeId owner = Erase(owner_binding->type);
    uint32_t index = 0;
    for (const Member& member : resolution().MembersOf(owner)) {
      if (member.kind != MemberKind::kVariant) {
        continue;
      }
      if (member.name == callee.name) {
        const InstId built =
            builder_->Add(Opcode::kMakeVariant, owner, arguments);
        builder_->Mutable(built).field = index;
        return Coerce(built, owner, type);
      }
      ++index;
    }
    return InstId{};
  }

  // The tag for `Enum.Variant`, or an invalid id when this is not one.
  InstId VariantValue(const Expr& expr, TypeId type) {
    if (!ast().Contains(expr.lhs) ||
        ast().Get(expr.lhs).kind != ExprKind::kPath) {
      return InstId{};
    }
    const Binding* binding = resolution().Find(ast().Get(expr.lhs).name);
    if (binding == nullptr || binding->kind != BindingKind::kType) {
      return InstId{};
    }
    const TypeId owner = Erase(binding->type);
    uint64_t index = 0;
    for (const Member& member : resolution().MembersOf(owner)) {
      if (member.kind != MemberKind::kVariant) {
        continue;
      }
      if (member.name == expr.name) {
        // An enum *is* its tag, and lowering reads that tag as field zero
        // (the layout says so), so the value is a one-field aggregate
        // rather than a bare number.
        const InstId tag = builder_->ConstInt(types_->u32(), index);
        return Coerce(
            builder_->Add(Opcode::kMakeStruct, owner, {tag}), owner, type);
      }
      ++index;
    }
    return InstId{};
  }

  // The type of the |position|-th value variant |index| carries.
  TypeId VariantFieldType(TypeId owner, uint32_t index, uint32_t position) {
    uint32_t seen = 0;
    for (const Member& member : resolution().MembersOf(owner)) {
      if (member.kind != MemberKind::kVariant) {
        continue;
      }
      if (seen++ != index) {
        continue;
      }
      const std::vector<TypeId> fields = types_->ArgumentsOf(member.type);
      return position < fields.size() ? Erase(fields[position])
                                      : types_->error();
    }
    return types_->error();
  }

  bool IsVariantName(TypeId owner, Symbol name) const {
    for (const Member& member : resolution().MembersOf(owner)) {
      if (member.kind == MemberKind::kVariant && member.name == name) {
        return true;
      }
    }
    return false;
  }

  // The type `Iterate` on |owner| hands back, or an invalid id when the
  // type does not say how to walk itself.
  TypeId IteratorOf(TypeId owner) {
    if (types_->KindOf(owner) != TypeKind::kNamed &&
        types_->KindOf(owner) != TypeKind::kInstance) {
      return TypeId{};
    }
    const Member* iterate =
        FindAnyMember(owner, interner_->Intern("Iterate"));
    if (iterate == nullptr || iterate->kind != MemberKind::kMethod ||
        types_->KindOf(iterate->type) != TypeKind::kFunction) {
      return TypeId{};
    }
    const TypeId iterator = Erase(Apply(types_->ElementOf(iterate->type)));
    // It has to answer the other two questions as well, or the loop has
    // nothing to ask.
    if (FindAnyMember(iterator, interner_->Intern("Next")) == nullptr ||
        FindAnyMember(iterator, interner_->Intern("Value")) == nullptr) {
      return TypeId{};
    }
    return iterator;
  }

  // `for x in thing` where the thing says how to walk itself.
  //
  // Three calls and a loop: `Iterate` once, then `Next` until it says no,
  // with `Value` inside. Written out here rather than desugared in the
  // parser, so that a diagnostic points at the loop the program wrote.
  void LowerIteratorLoop(const Expr& expr, TypeId sequence_type) {
    const TypeId iterator = IteratorOf(sequence_type);
    InstId sequence;
    {
      // Read through rather than taken: the loop borrows what it walks.
      const Borrowing borrowing(this);
      sequence = Value(expr.lhs);
    }
    const InstId walker =
        CallMethod(sequence_type, sequence, interner_->Intern("Iterate"),
                   {}, iterator);
    const InstId slot = builder_->Add(Opcode::kAlloca, iterator);
    builder_->Add(Opcode::kStore, TypeId{}, {slot, walker});

    const BlockId header = builder_->AddBlock("for.header");
    const BlockId body = builder_->AddBlock("for.body");
    const BlockId exit = builder_->AddBlock("for.exit");
    builder_->Jump(header);

    builder_->SetCurrent(header);
    const InstId current =
        builder_->Add(Opcode::kLoad, iterator, {slot});
    const InstId more =
        CallMethod(iterator, current, interner_->Intern("Next"), {},
                   types_->bool_type());
    builder_->Branch(more, body, exit);

    builder_->SetCurrent(body);
    const Member* value =
        FindAnyMember(iterator, interner_->Intern("Value"));
    const TypeId element =
        value != nullptr ? Erase(Apply(types_->ElementOf(value->type)))
                         : types_->error();
    const InstId held = builder_->Add(Opcode::kLoad, iterator, {slot});
    const InstId element_value =
        CallMethod(iterator, held, interner_->Intern("Value"), {}, element);

    const size_t depth = locals_.size();
    loops_.push_back(LoopTarget{expr.label, exit, header, scope_depth_});
    if (ast().Contains(expr.pattern) &&
        ast().Get(expr.pattern).kind == PatternKind::kBinding) {
      locals_.push_back(Local{ast().Get(expr.pattern).name, InstId{},
                              element_value, element});
    }
    Lower(expr.rhs);
    locals_.resize(depth);
    loops_.pop_back();
    if (!builder_->Terminated()) {
      builder_->Jump(header);
    }
    builder_->SetCurrent(exit);
  }

  // Calls |name| on |receiver|, which the caller has already worked out
  // the type of. For the loops and the destructors that build calls the
  // source did not write.
  InstId CallMethod(TypeId owner,
                    InstId receiver,
                    Symbol name,
                    std::vector<InstId> arguments,
                    TypeId result) {
    const Member* member = FindAnyMember(owner, name);
    if (member == nullptr) {
      return builder_->Undef(result);
    }
    const uint32_t index = MethodIndex(owner, *member);
    if (index == 0xFFFFFFFFu) {
      return builder_->Undef(result);
    }
    std::vector<InstId> operands;
    operands.push_back(receiver);
    for (InstId argument : arguments) {
      operands.push_back(argument);
    }
    return builder_->Call(index, result, std::move(operands));
  }

  // `|n| n + 1`: a function the program wrote where it is used.
  //
  // Declared here, with the closure's own type as its signature, and the
  // body queued -- the value of the expression is the function's address,
  // which is what a `fun(...)` value is in every tier (D106).
  InstId LowerClosure(ExprId id, const Expr& expr) {
    const TypeId signature = Erase(Apply(TypeOf(id)));
    if (types_->KindOf(signature) != TypeKind::kFunction) {
      return builder_->Undef(signature);
    }

    // What the closure takes with it, and where from: the locals of the
    // function being lowered, by the names the checker recorded (D138).
    std::vector<Symbol> captures;
    std::vector<TypeId> captured_types;
    std::vector<InstId> captured_values;
    for (const Symbol captured : typed().CapturesOf(id)) {
      const Local* local = Find(captured);
      if (local == nullptr) {
        continue;
      }
      const TypeId held = Erase(local->type);
      const InstId value = local->slot.is_valid()
                               ? builder_->Add(Opcode::kLoad, held,
                                               {local->slot})
                               : local->value;
      if (!value.is_valid()) {
        continue;
      }
      captures.push_back(captured);
      captured_types.push_back(held);
      captured_values.push_back(value);
    }

    std::string name = "closure$";
    name.append(std::to_string(closure_serial_++));
    Function function;
    function.name = interner_->Intern(name);
    function.type = signature;

    // The body of a capturing closure takes the environment first. Its
    // *value* keeps the closure's own type -- the call through it is
    // indirect, and which of the two shapes to call is a question about
    // the value (D138).
    TypeId environment;
    if (!captures.empty()) {
      environment = types_->Tuple(captured_types);
      std::vector<TypeId> params;
      params.push_back(types_->Pointer(environment, /*is_mutable=*/true));
      for (const TypeId held : types_->ArgumentsOf(signature)) {
        params.push_back(held);
      }
      function.type =
          types_->Function(params, types_->ElementOf(signature));
    }
    // Nothing observable: a closure that takes nothing with it can only
    // do what its arguments and the file's names allow, and saying the
    // row is unknown would stop a call to it being deleted as dead.
    function.effects_known = false;
    const uint32_t index = static_cast<uint32_t>(module_->functions.size());
    module_->functions.push_back(std::move(function));

    closures_.push_back(PendingClosure{id, index, substitution_, self_type_,
                                       here_, captures, environment});

    // The value is the pair every function value is: the code, and what
    // it closed over. A closure that captures nothing closed over
    // nothing; one that captures gets a copy of each name, in the frame
    // of the function that made it -- which is why it may not outlive it
    // (D138).
    const InstId code = builder_->Add(Opcode::kFuncAddress, types_->u64());
    builder_->Mutable(code).payload.callee = index;
    InstId closed = builder_->ConstInt(types_->u64(), 0);
    if (!captures.empty()) {
      // On the heap rather than in the frame: the environment's *address*
      // is what travels, and a frame slot has no address in the
      // reference tier -- it has an index. From the innermost `arena`
      // block when there is one, like every other allocation, and given
      // back when the scope that made the closure ends (D138).
      const TypeId pointer =
          types_->Pointer(environment, /*is_mutable=*/true);
      // At the top of the function rather than here: the environment is
      // freed when the scope ends, and a definition inside a branch --
      // `a && Each(xs, |x| ...)` puts one there -- would not dominate
      // that (D138). One allocation per closure site per call, reused by
      // every turn of a loop, which is safe because a capturing closure
      // cannot outlive the frame anyway.
      const InstId storage = builder_->AddToEntry(Opcode::kNew, pointer);
      for (size_t i = 0; i < captured_values.size(); ++i) {
        const InstId field = builder_->FieldAddress(
            types_->Pointer(captured_types[i], /*is_mutable=*/true),
            storage, static_cast<uint32_t>(i));
        builder_->Add(Opcode::kStore, TypeId{},
                      {field, captured_values[i]});
      }
      closed = builder_->Add(Opcode::kCast, types_->u64(), {storage});
      FreeAtScopeExit(storage);
    }
    return builder_->Add(Opcode::kMakeStruct, signature, {code, closed});
  }

  // The body of one, as an ordinary function of its parameters.
  void LowerClosureBody(const PendingClosure& work) {
    const Context outer = here_;
    const TypeStore::Substitution outer_substitution = substitution_;
    const TypeId outer_self = self_type_;
    if (work.where.ast != nullptr) {
      here_ = Context{work.where.file_index, work.where.resolution,
                      work.where.ast, work.where.typed, work.where.effects};
      resolver_ = TypeResolver(types_, interner_, here_.resolution,
                               &discarded_);
    }
    substitution_ = work.substitution;
    self_type_ = work.owner;

    if (ast().Contains(work.expr) &&
        work.index < module_->functions.size()) {
      const Expr& expr = ast().Get(work.expr);
      Builder builder(module_, FunctionId{work.index});
      builder.types = types_;
      current_function_ = work.index;
      builder_ = &builder;
      locals_.clear();
      loops_.clear();
      deferred_.clear();
      owners_.clear();
    owners_.clear();
      scope_depth_ = 0;
      const BlockId entry = builder.AddBlock("entry");
      builder.SetCurrent(entry);

      std::vector<TypeId> params =
          types_->ArgumentsOf(module_->functions[work.index].type);
      if (!work.captures.empty() && !params.empty()) {
        // The environment, and the names it holds: bound before the
        // parameters, because the body reads them like locals (D138).
        const InstId held = builder.AddParam(entry, params.front());
        const std::vector<TypeId> inside =
            types_->ArgumentsOf(work.environment);
        for (size_t i = 0; i < work.captures.size() && i < inside.size();
             ++i) {
          const InstId field = builder.FieldAddress(
              types_->Pointer(inside[i], /*is_mutable=*/true), held,
              static_cast<uint32_t>(i));
          const InstId value =
              builder.Add(Opcode::kLoad, inside[i], {field});
          locals_.push_back(
              Local{work.captures[i], InstId{}, value, inside[i]});
        }
        params.erase(params.begin());
      }
      size_t position = 0;
      for (ParamId param_id : ast().Get(expr.params)) {
        const Param& param = ast().Get(param_id);
        const TypeId type =
            position < params.size() ? params[position] : types_->error();
        const InstId value = builder.AddParam(entry, type);
        locals_.push_back(Local{param.name, InstId{}, value, type});
        ++position;
      }

      return_type_ = types_->ElementOf(module_->functions[work.index].type);
      // The body is an expression: its value is what the closure gives
      // back, the way an expression-bodied function's is.
      const InstId value = Value(expr.lhs);
      if (!builder.Terminated()) {
        if (types_->KindOf(return_type_) == TypeKind::kVoid) {
          builder.ReturnVoid();
        } else {
          builder.Return(Coerce(value, TypeOf(expr.lhs), return_type_));
        }
      }
      builder_ = nullptr;
    }

    substitution_ = outer_substitution;
    self_type_ = outer_self;
    if (work.where.ast != nullptr) {
      here_ = Context{outer.file_index, outer.resolution, outer.ast,
                      outer.typed, outer.effects};
      resolver_ = TypeResolver(types_, interner_, here_.resolution,
                               &discarded_);
    }
  }

  // What a `system` becomes: a function of the world and the time step,
  // with its queries opened from that world.
  //
  // The queries are built here rather than passed in because a `Query` is
  // a *declaration* of access, not a value the caller has: the system says
  // `Query<mut Position, Velocity>` and what it needs is a walk over the
  // entities of that world which have both (D101).
  void LowerSystem(const Item& item, uint32_t index) {
    if (index >= module_->functions.size() || !item.body.is_valid()) {
      return;
    }
    Builder builder(module_, FunctionId{index});
    builder.types = types_;
    current_function_ = index;
    builder_ = &builder;
    locals_.clear();
    loops_.clear();
    deferred_.clear();
    owners_.clear();
    scope_depth_ = 0;

    const BlockId entry = builder.AddBlock("entry");
    builder.SetCurrent(entry);
    const TypeId word = types_->u64();
    const TypeId step = types_->Float(FloatWidth::k32);
    const InstId world = builder.AddParam(entry, word);
    const InstId delta = builder.AddParam(entry, step);

    // Every cursor this system opened, closed again on the way out: a
    // query is a walk over the world as it was when the walk started, so
    // it holds something that has to be given back.
    std::vector<InstId> cursors;
    for (ParamId param_id : ast().Get(item.params)) {
      const Param& param = ast().Get(param_id);
      const TypeId declared =
          param.type.is_valid()
              ? Erase(resolver_.Resolve(ast(), param.type))
              : TypeId{};
      if (IsQuery(declared)) {
        const InstId cursor = OpenQuery(world, param.type);
        cursors.push_back(cursor);
        locals_.push_back(Local{param.name, InstId{}, cursor, declared});
        continue;
      }
      // The time step, which is the one other thing a system may ask for.
      if (declared.is_valid() &&
          types_->KindOf(declared) == TypeKind::kFloat) {
        locals_.push_back(Local{param.name, InstId{},
                                Coerce(delta, step, declared), declared});
        continue;
      }
      locals_.push_back(
          Local{param.name, InstId{}, builder.Undef(declared), declared});
    }

    return_type_ = types_->void_type();
    Lower(item.body);
    for (const InstId cursor : cursors) {
      CloseQuery(cursor);
    }
    if (!builder.Terminated()) {
      builder.ReturnVoid();
    }
    builder_ = nullptr;
  }

  // This file's systems, stage by stage, handed to the runtime.
  //
  // The schedule is a property of the declarations -- which is the whole
  // argument for an ECS being in the language rather than in a library --
  // so the call the program wrote becomes the stages the compiler worked
  // out (D82, D101).
  InstId RunSchedule(InstId world, InstId delta) {
    const TypeId word = types_->u64();
    const TypeId step = types_->Float(FloatWidth::k32);
    const TypeId nothing = types_->void_type();
    // What the runtime holds is a number, and a `World` is that number in
    // its first field -- so the handle is read out of the object the same
    // way any other field of it would be.
    InstId handle = world;
    if (types_->IsHandle(ValueType(world))) {
      const InstId address =
          builder_->Add(Opcode::kFieldAddress,
                        types_->Pointer(word, /*is_mutable=*/true), {world});
      builder_->Mutable(address).field = 0;
      handle = builder_->Add(Opcode::kLoad, word, {address});
    }
    const Symbol add = interner_->Intern("wolang_stage_add");
    const uint32_t collect =
        DeclareRuntime(add, types_->Function({word}, nothing));
    const Symbol run = interner_->Intern("wolang_stage_run");
    const uint32_t callee =
        DeclareRuntime(run, types_->Function({word, step}, nothing));
    const InstId step_value = Coerce(delta, ValueType(delta), step);
    InstId last;
    const std::vector<std::vector<Symbol>> none;
    for (const std::vector<Symbol>& stage :
         systems_ != nullptr ? systems_->stages : none) {
      bool any = false;
      for (const Symbol name : stage) {
        const uint32_t index = IndexOfName(name);
        if (index == 0xFFFFFFFFu) {
          // A system from another file of the module: that file's own
          // `RunSystems` runs it, because the address of a function is
          // something only the module holding it can give.
          continue;
        }
        const InstId address = builder_->Add(Opcode::kFuncAddress, word);
        builder_->Mutable(address).payload.callee = index;
        builder_->Call(collect, nothing, {address});
        any = true;
      }
      if (any) {
        last = builder_->Call(callee, nothing, {handle, step_value});
      }
    }
    // A file with no systems still ran: the call is what the program
    // wrote, and running nothing is what it means there.
    return last.is_valid()
               ? last
               : builder_->Call(callee, nothing, {handle, step_value});
  }

  // The component a query argument names. `mut T` resolves to a mutable
  // reference to it, and `Erase` turns a reference into the address it is
  // -- so what names the column is under one or the other (D101).
  TypeId ComponentOf(TypeId type) {
    TypeId named = Erase(type);
    while (types_->KindOf(named) == TypeKind::kReference ||
           types_->KindOf(named) == TypeKind::kPointer) {
      named = Erase(types_->ElementOf(named));
    }
    return named;
  }

  bool IsQuery(TypeId type) const {
    return type.is_valid() && types_->KindOf(type) == TypeKind::kInstance &&
           types_->Get(type).base == types_->query();
  }

  // The cursor for one `Query<...>`, built from the written type: which
  // components an entity must have and which it must not.
  //
  // Read from the syntax rather than from the resolved type because
  // `with` and `without` describe access rather than the type itself --
  // both resolve to the component, and only the written form says which
  // is which.
  InstId OpenQuery(InstId world, TypeRefId written) {
    std::vector<uint64_t> required;
    std::vector<uint64_t> excluded;
    if (ast().Contains(written)) {
      for (TypeRefId argument : ast().Get(ast().Get(written).args)) {
        if (!ast().Contains(argument)) {
          continue;
        }
        const TypeRef& filter = ast().Get(argument);
        const TypeId component =
            ComponentOf(resolver_.Resolve(ast(), argument));
        if (!component.is_valid()) {
          continue;
        }
        const uint64_t token = ClassTokenOf(*types_, component);
        if (filter.kind == TypeRefKind::kWithout) {
          excluded.push_back(token);
        } else {
          required.push_back(token);
        }
      }
    }

    const TypeId word = types_->u64();
    const TypeId words = types_->Pointer(word, /*is_mutable=*/true);
    const InstId wanted = TokenArray(required);
    const InstId refused = TokenArray(excluded);
    const Symbol open = interner_->Intern("wolang_query_open");
    const uint32_t callee = DeclareRuntime(
        open, types_->Function({word, words, word, words, word}, word));
    return builder_->Call(
        callee, word,
        {world, wanted, builder_->ConstInt(word, required.size()), refused,
         builder_->ConstInt(word, excluded.size())});
  }

  // A run of tokens in memory, and the address of the first.
  //
  // None of them is a null address, which is what the runtime reads as
  // "no filter" -- and the address is an element's rather than the slot's,
  // because that is the one thing a slot of any type can hand to C.
  InstId TokenArray(const std::vector<uint64_t>& tokens) {
    const TypeId word = types_->u64();
    const TypeId words = types_->Pointer(word, /*is_mutable=*/true);
    if (tokens.empty()) {
      return builder_->Add(Opcode::kCast, words,
                           {builder_->ConstInt(word, 0)});
    }
    // A local array, the way the language's own `var xs: [N]u64` is: a
    // slot of an array type with element addresses into it. A slot of
    // several words would be a thing only the tiers with real frames
    // understand.
    const TypeId array =
        types_->Array(word, tokens.size(), /*is_soa=*/false);
    const InstId room = builder_->Add(Opcode::kAlloca, array);
    for (size_t i = 0; i < tokens.size(); ++i) {
      const InstId at = builder_->Add(
          Opcode::kIndexAddress, types_->Pointer(word, /*is_mutable=*/true),
          {room, builder_->ConstInt(word, i),
           builder_->ConstInt(word, tokens.size())});
      builder_->Add(Opcode::kStore, TypeId{},
                    {at, builder_->ConstInt(word, tokens[i])});
    }
    return builder_->Add(
        Opcode::kIndexAddress, words,
        {room, builder_->ConstInt(word, 0),
         builder_->ConstInt(word, tokens.size())});
  }

  void CloseQuery(InstId cursor) {
    const TypeId word = types_->u64();
    const Symbol close = interner_->Intern("wolang_query_close");
    const uint32_t callee = DeclareRuntime(
        close, types_->Function({word}, types_->void_type()));
    builder_->Call(callee, types_->void_type(), {cursor});
  }

  // `foreach x in q`: the walk the query stands for.
  //
  // `x` is bound to where the component *is*, not to a copy of it: a
  // system that writes says so in its signature, and what it writes has
  // to be the component the world holds (D101).
  void LowerQueryLoop(const Expr& expr, TypeId query) {
    const TypeId word = types_->u64();
    const InstId cursor = Value(expr.lhs);
    // The first component the query names is what the body walks over:
    // `Query<mut Position, Velocity>` binds a `Position`, and the rest
    // are what an entity must also have (D101). The arguments rather than
    // the element, because an instance of a built-in has no element -- the
    // components *are* its arguments.
    const std::vector<TypeId> named = types_->ArgumentsOf(query);
    const TypeId component =
        named.empty() ? TypeId{} : ComponentOf(Apply(named.front()));

    // Where every matching entity's component is, asked for once, and
    // then a loop that is arithmetic: a call per entity was most of what
    // running a system cost (D101).
    const TypeId addresses = types_->Pointer(word, /*is_mutable=*/false);
    const Symbol walk = interner_->Intern("wolang_query_walk");
    const uint32_t ask =
        DeclareRuntime(walk, types_->Function({word}, addresses));
    const Symbol length = interner_->Intern("wolang_query_length");
    const uint32_t how_many =
        DeclareRuntime(length, types_->Function({word}, word));
    const InstId first = builder_->Call(ask, addresses, {cursor});
    const InstId count = builder_->Call(how_many, word, {cursor});

    const BlockId header = builder_->AddBlock("query.header");
    const BlockId body = builder_->AddBlock("query.body");
    const BlockId exit = builder_->AddBlock("query.exit");
    const InstId index = builder_->Add(Opcode::kAlloca, word);
    builder_->Add(Opcode::kStore, TypeId{},
                  {index, builder_->ConstInt(word, 0)});
    builder_->Jump(header);

    builder_->SetCurrent(header);
    const InstId at = builder_->Add(Opcode::kLoad, word, {index});
    const InstId more =
        builder_->Add(Opcode::kLt, types_->bool_type(), {at, count});
    builder_->Branch(more, body, exit);

    builder_->SetCurrent(body);
    const InstId slot = builder_->Add(
        Opcode::kIndexAddress, types_->Pointer(word, /*is_mutable=*/false),
        {first, at, count});
    builder_->Mutable(slot).is_unchecked = true;
    const InstId held = builder_->Add(Opcode::kLoad, word, {slot});
    // The address is a number as far as the boundary is concerned; what
    // the body reads is the component at it.
    const InstId component_at = builder_->Add(
        Opcode::kCast, types_->Pointer(component, /*is_mutable=*/true),
        {held});

    const size_t depth = locals_.size();
    loops_.push_back(LoopTarget{expr.label, exit, header, scope_depth_});
    if (ast().Contains(expr.pattern) &&
        ast().Get(expr.pattern).kind == PatternKind::kBinding) {
      // Bound as a slot: reading it loads through the address and writing
      // it stores through it, which is what makes `mut` in the query mean
      // what it says.
      locals_.push_back(Local{ast().Get(expr.pattern).name, component_at,
                              InstId{}, component});
    }
    Lower(expr.rhs);
    locals_.resize(depth);
    loops_.pop_back();
    if (!builder_->Terminated()) {
      const InstId next = builder_->Add(
          Opcode::kAdd, word, {at, builder_->ConstInt(word, 1)});
      builder_->Mutable(next).is_unchecked = true;
      builder_->Add(Opcode::kStore, TypeId{}, {index, next});
      builder_->Jump(header);
    }
    builder_->SetCurrent(exit);
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
    const bool over_range = ast().Contains(expr.lhs) &&
                            ast().Get(expr.lhs).kind == ExprKind::kRange;

    // A query: the walk over the world the system declared it wanted.
    if (!over_range && IsQuery(sequence_type)) {
      LowerQueryLoop(expr, sequence_type);
      return;
    }

    // A type that says how to walk itself: `Iterate` hands back something
    // with `Next` and `Value`, and the loop is those three calls (D96).
    if (!over_range && IteratorOf(sequence_type).is_valid()) {
      LowerIteratorLoop(expr, sequence_type);
      return;
    }

    // What the index counts in, and what it counts to.
    InstId start;
    InstId limit;
    bool inclusive = false;
    Place sequence;
    TypeId element;
    if (over_range) {
      const Expr& range = ast().Get(expr.lhs);
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
    if (ast().Contains(expr.pattern)) {
      const Pattern& pattern = ast().Get(expr.pattern);
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
    // The type the body's `self` is, for a method of a generic type.
    TypeId owner;
    // The file the body was written in, when it was not this one.
    Context where;
  };

  StringInterner* interner_;
  TypeStore* types_;
  Context here_;
  ForeignLookup foreign_;
  // The module's schedule: which systems may run together, and in what
  // order the stages go (D82).
  const AccessGraph* systems_ = nullptr;
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
  // Name, index, and the file the declaration is in. The file matters:
  // while a generic from another module is being lowered here, a name in
  // its body means what it means *there* -- and an entry from this file
  // that happens to spell the same thing is a different declaration.
  struct Named {
    Symbol name;
    size_t index = 0;
    uint32_t file = 0;
  };
  std::vector<Named> constants_;
  std::vector<MachineBlock> machine_blocks_;
  std::vector<MachineMethod> machine_methods_;
  std::vector<PendingClosure> closures_;
  uint32_t closure_serial_ = 0;
  std::vector<Pending> pending_;
  std::vector<Deferred> deferred_;
  // Which local owns something, and the flag that says whether it still
  // does. Parallel to the deferred list rather than part of it, because a
  // move looks the flag up by name and an exit walks the scopes.
  struct Owner {
    Symbol name;
    InstId flag;
  };
  std::vector<Owner> owners_;
  // How many enclosing reads are through a name rather than of it.
  int borrow_depth_ = 0;
  // The module-level `var`s this file declares.
  struct Global {
    Symbol name;
    uint64_t token = 0;
    TypeId type;
    // The function that works out its first value.
    uint32_t initializer = 0;
    // Whether the source gave it one. A `var` with none is its type's
    // zero, which the runtime's storage already is (D136).
    bool has_initializer = true;
  };
  std::vector<Global> globals_;
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
  struct Declared {
    ItemId item;
    size_t index = 0;
    Symbol name;
    uint32_t file = 0;
    // A method, which is reached through its owner and not by its bare
    // name: `Bytes(value)` is the imported function even when a class in
    // the same file has a `Bytes` method (D123).
    bool is_member = false;
  };
  std::vector<Declared> indices_;
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
               ForeignLookup foreign,
               const AccessGraph* systems,
               Module* module) {
  Lowerer lowerer(interner, types, file_index, resolution, ast, typed,
                  effects, std::move(foreign), systems, module);
  lowerer.Run();
}

}  // namespace ir
}  // namespace wolang
