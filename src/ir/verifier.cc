// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/ir/verifier.h"

#include <algorithm>

namespace wolang {
namespace ir {
namespace {

class Verifier {
 public:
  Verifier(const StringInterner& interner,
           const TypeStore& types,
           const Module& module,
           const Function& function)
      : interner_(interner),
        types_(types),
        module_(module),
        function_(function) {}

  VerificationResult Run() {
    // A declaration has a signature and no body. There is nothing to verify
    // and nothing wrong with it.
    if (function_.is_declaration()) {
      return std::move(result_);
    }

    CheckBlockShape();
    ComputeReachability();
    ComputeDominators();
    CheckOperands();
    CheckEdges();
    return std::move(result_);
  }

 private:
  void Fail(std::string message) {
    std::string prefixed = "@";
    prefixed.append(interner_.Text(function_.name));
    prefixed.append(": ");
    prefixed.append(std::move(message));
    result_.problems.push_back(std::move(prefixed));
  }

  std::string At(BlockId block) const {
    return "^" + std::to_string(block.index);
  }
  std::string At(InstId inst) const {
    return "%" + std::to_string(inst.index);
  }

  // Exactly one terminator, at the end. A block that falls off the end has
  // no defined successor, and one that terminates early leaves dead
  // instructions the back ends would still have to lower.
  void CheckBlockShape() {
    for (uint32_t i = 0; i < function_.blocks.size(); ++i) {
      const BlockId id{i};
      const Block& block = function_.Get(id);

      if (block.instructions.empty()) {
        // A pass removes a block by emptying it, because a BlockId is an
        // index and renumbering would invalidate every successor. An empty
        // block is therefore a deleted one, and only a jump to it is wrong.
        continue;
      }
      if (block.param_count > block.instructions.size()) {
        Fail(At(id) + " claims more parameters than it has instructions");
        continue;
      }

      for (size_t j = 0; j < block.instructions.size(); ++j) {
        const InstId inst_id = block.instructions[j];
        if (!function_.Contains(inst_id)) {
          Fail(At(id) + " holds the invalid instruction " + At(inst_id));
          continue;
        }
        const Inst& inst = function_.Get(inst_id);
        const bool last = j + 1 == block.instructions.size();

        if (IsTerminator(inst.opcode) && !last) {
          Fail(At(id) + " has a terminator at " + At(inst_id) +
               ", which is not its last instruction");
        }
        if (!IsTerminator(inst.opcode) && last) {
          Fail(At(id) + " ends with " + At(inst_id) +
               ", which is not a terminator");
        }
        if ((inst.opcode == Opcode::kParam) != (j < block.param_count)) {
          Fail(At(id) + " has " + At(inst_id) +
               " where a parameter was expected, or the other way round");
        }
        if (function_.owners.size() > inst_id.index &&
            function_.owners[inst_id.index] != id) {
          Fail(At(inst_id) + " is listed in " + At(id) +
               " but owned by another block");
        }
      }
    }
  }

  void ComputeReachability() {
    reachable_.assign(function_.blocks.size(), false);
    std::vector<BlockId> worklist{function_.entry()};
    reachable_[0] = true;
    while (!worklist.empty()) {
      const BlockId block = worklist.back();
      worklist.pop_back();
      for (BlockId next : SuccessorsOf(block)) {
        if (!function_.Contains(next)) {
          Fail(At(block) + " jumps to the invalid block " + At(next));
          continue;
        }
        if (function_.blocks[next.index].instructions.empty()) {
          Fail(At(block) + " jumps to " + At(next) + ", which was removed");
          continue;
        }
        if (reachable_[next.index]) {
          continue;
        }
        reachable_[next.index] = true;
        worklist.push_back(next);
      }
    }

    for (uint32_t i = 0; i < reachable_.size(); ++i) {
      if (!reachable_[i] && !function_.blocks[i].instructions.empty()) {
        Fail(At(BlockId{i}) + " is not reachable from the entry");
      }
    }
  }

  // The iterative dominator set algorithm. Slower than Lengauer-Tarjan and
  // far easier to be sure of, which is the right trade for a verifier: it
  // runs on every test input, so it has to be obviously correct rather than
  // fast.
  void ComputeDominators() {
    const size_t count = function_.blocks.size();
    dominators_.assign(count, std::vector<bool>(count, true));
    dominators_[0].assign(count, false);
    dominators_[0][0] = true;

    bool changed = true;
    while (changed) {
      changed = false;
      for (uint32_t i = 1; i < count; ++i) {
        if (!reachable_[i] || function_.blocks[i].instructions.empty()) {
          continue;
        }
        std::vector<bool> next(count, true);
        bool any_predecessor = false;
        for (uint32_t p = 0; p < count; ++p) {
          if (!reachable_[p]) {
            continue;
          }
          const std::span<const BlockId> successors = SuccessorsOf(BlockId{p});
          if (std::find(successors.begin(), successors.end(), BlockId{i}) ==
              successors.end()) {
            continue;
          }
          any_predecessor = true;
          for (uint32_t b = 0; b < count; ++b) {
            next[b] = next[b] && dominators_[p][b];
          }
        }
        if (!any_predecessor) {
          next.assign(count, false);
        }
        next[i] = true;
        if (next != dominators_[i]) {
          dominators_[i] = std::move(next);
          changed = true;
        }
      }
    }
  }

  bool Dominates(BlockId a, BlockId b) const {
    if (!function_.Contains(a) || !function_.Contains(b)) {
      return false;
    }
    return dominators_[b.index][a.index];
  }

  // A definition has to be in scope at every use. Within one block that is a
  // position comparison; across blocks it is dominance.
  void CheckOperands() {
    for (uint32_t i = 0; i < function_.blocks.size(); ++i) {
      const BlockId block_id{i};
      const Block& block = function_.Get(block_id);
      for (size_t position = 0; position < block.instructions.size();
           ++position) {
        const InstId use = block.instructions[position];
        if (!function_.Contains(use)) {
          continue;
        }
        const Inst& inst = function_.Get(use);
        for (InstId operand : function_.OperandsOf(inst)) {
          CheckOperand(block_id, position, use, operand);
        }
        CheckTypes(use, inst);
      }
    }
  }

  void CheckOperand(BlockId block_id,
                    size_t position,
                    InstId use,
                    InstId operand) {
    if (!function_.Contains(operand)) {
      Fail(At(use) + " uses the invalid value " + At(operand));
      return;
    }
    if (!ProducesValue(function_.Get(operand).opcode)) {
      Fail(At(use) + " uses " + At(operand) + ", which produces no value");
      return;
    }
    if (operand.index >= function_.owners.size()) {
      Fail(At(operand) + " belongs to no block");
      return;
    }

    const BlockId definition = function_.owners[operand.index];
    if (definition == block_id) {
      const Block& block = function_.Get(block_id);
      const auto defined_at =
          std::find(block.instructions.begin(), block.instructions.end(),
                    operand);
      const size_t defined_position =
          static_cast<size_t>(defined_at - block.instructions.begin());
      if (defined_position >= position) {
        Fail(At(use) + " uses " + At(operand) + " before it is defined");
      }
      return;
    }
    if (!Dominates(definition, block_id)) {
      Fail(At(use) + " uses " + At(operand) + ", defined in " +
           At(definition) + ", which does not dominate " + At(block_id));
    }
  }

  void CheckTypes(InstId id, const Inst& inst) {
    const std::span<const InstId> operands = function_.OperandsOf(inst);
    const auto arity = [&](size_t expected) {
      if (operands.size() != expected) {
        Fail(At(id) + " (" + std::string(OpcodeMnemonic(inst.opcode)) +
             ") takes " + std::to_string(expected) + " operands, has " +
             std::to_string(operands.size()));
        return false;
      }
      return true;
    };
    const auto type_of = [&](size_t index) {
      return function_.Contains(operands[index])
                 ? function_.Get(operands[index]).type
                 : TypeId{};
    };

    if (ProducesValue(inst.opcode) && !inst.type.is_valid()) {
      Fail(At(id) + " produces a value with no type");
    }
    if (!ProducesValue(inst.opcode) && inst.type.is_valid()) {
      Fail(At(id) + " produces no value but carries a type");
    }

    switch (inst.opcode) {
      case Opcode::kAdd:
      case Opcode::kSub:
      case Opcode::kMul:
      case Opcode::kDiv:
      case Opcode::kRem:
      case Opcode::kBitAnd:
      case Opcode::kBitOr:
      case Opcode::kBitXor:
      case Opcode::kShl:
      case Opcode::kShr:
        if (arity(2) && type_of(0) != type_of(1)) {
          Fail(At(id) + " combines two different types");
        }
        break;

      case Opcode::kEq:
      case Opcode::kNe:
      case Opcode::kLt:
      case Opcode::kLe:
      case Opcode::kGt:
      case Opcode::kGe:
        if (arity(2)) {
          if (type_of(0) != type_of(1)) {
            Fail(At(id) + " compares two different types");
          }
          if (types_.KindOf(inst.type) != TypeKind::kBool) {
            Fail(At(id) + " is a comparison but does not produce a bool");
          }
        }
        break;

      case Opcode::kNeg:
      case Opcode::kBitNot:
        if (arity(1) && type_of(0) != inst.type) {
          Fail(At(id) + " changes the type of its operand");
        }
        break;

      case Opcode::kLogicalNot:
        if (arity(1) && types_.KindOf(type_of(0)) != TypeKind::kBool) {
          Fail(At(id) + " negates something that is not a bool");
        }
        break;

      case Opcode::kBranch:
        if (arity(1) && types_.KindOf(type_of(0)) != TypeKind::kBool) {
          Fail(At(id) + " branches on something that is not a bool");
        }
        if (inst.successors_count != 2) {
          Fail(At(id) + " is a branch with " +
               std::to_string(inst.successors_count) + " successors");
        }
        break;

      case Opcode::kJump:
        if (inst.successors_count != 1) {
          Fail(At(id) + " is a jump with " +
               std::to_string(inst.successors_count) + " successors");
        }
        break;

      case Opcode::kSwitch:
        if (arity(1) && !types_.IsInteger(type_of(0))) {
          Fail(At(id) + " switches on something that is not an integer");
        }
        if (inst.successors_count != inst.cases_count + 1) {
          Fail(At(id) + " has " + std::to_string(inst.cases_count) +
               " cases but " + std::to_string(inst.successors_count) +
               " successors; it needs one more for the default");
        }
        break;

      case Opcode::kStore:
        arity(2);
        break;

      case Opcode::kFieldAddress:
        // The operand is an address and so is the result: a field address
        // is pointer arithmetic the layout decides, which is why it is an
        // instruction rather than an add.
        if (arity(1) &&
            types_.KindOf(type_of(0)) != TypeKind::kPointer) {
          Fail(At(id) + " takes a field address inside something that is "
                        "not a pointer");
        }
        if (types_.KindOf(inst.type) != TypeKind::kPointer) {
          Fail(At(id) + " produces something that is not a pointer");
        }
        break;

      case Opcode::kCall: {
        // A call is the one place the IR refers to something outside the
        // function, so it is the one place the module has to be consulted.
        const uint32_t callee = inst.payload.callee;
        if (callee >= module_.functions.size()) {
          Fail(At(id) + " calls function " + std::to_string(callee) +
               ", which the module does not have");
          break;
        }
        const Function& target = module_.functions[callee];
        const std::vector<TypeId> params = types_.ArgumentsOf(target.type);
        if (params.size() != operands.size()) {
          Fail(At(id) + " passes " + std::to_string(operands.size()) +
               " arguments to @" + std::string(interner_.Text(target.name)) +
               ", which takes " + std::to_string(params.size()));
          break;
        }
        for (size_t i = 0; i < params.size(); ++i) {
          if (type_of(i) != params[i]) {
            Fail(At(id) + " passes " + At(operands[i]) +
                 " as argument " + std::to_string(i) + " of @" +
                 std::string(interner_.Text(target.name)) +
                 ", whose type does not match");
          }
        }
        const TypeId result = types_.ElementOf(target.type);
        if (result.is_valid() && inst.type.is_valid() &&
            inst.type != result) {
          Fail(At(id) + " takes the result of @" +
               std::string(interner_.Text(target.name)) +
               " as the wrong type");
        }
        break;
      }

      case Opcode::kLoad:
        arity(1);
        break;

      case Opcode::kOptionalIsPresent:
        if (arity(1) && types_.KindOf(inst.type) != TypeKind::kBool) {
          Fail(At(id) + " does not produce a bool");
        }
        break;

      default:
        break;
    }
  }

  // A jump has to pass what the target's parameters expect. This is where
  // phi nodes would have gone wrong silently; as block parameters it is an
  // arity and type check.
  void CheckEdges() {
    for (uint32_t i = 0; i < function_.blocks.size(); ++i) {
      const BlockId block_id{i};
      const InstId terminator = function_.TerminatorOf(block_id);
      if (!function_.Contains(terminator)) {
        continue;
      }
      const Inst& inst = function_.Get(terminator);
      if (inst.opcode != Opcode::kJump) {
        // Only a jump carries arguments; a branch and a switch go to blocks
        // that take none, which is what keeps edge splitting unnecessary.
        for (BlockId target : function_.SuccessorsOf(inst)) {
          if (function_.Contains(target) &&
              function_.Get(target).param_count != 0) {
            Fail(At(terminator) + " goes to " + At(target) +
                 ", which takes parameters, but passes none");
          }
        }
        continue;
      }

      const std::span<const BlockId> targets = function_.SuccessorsOf(inst);
      if (targets.empty() || !function_.Contains(targets[0])) {
        continue;
      }
      const std::span<const InstId> params = function_.ParamsOf(targets[0]);
      const std::span<const InstId> arguments = function_.OperandsOf(inst);
      if (params.size() != arguments.size()) {
        Fail(At(terminator) + " passes " + std::to_string(arguments.size()) +
             " arguments to " + At(targets[0]) + ", which takes " +
             std::to_string(params.size()));
        continue;
      }
      for (size_t j = 0; j < params.size(); ++j) {
        if (!function_.Contains(arguments[j]) ||
            !function_.Contains(params[j])) {
          continue;
        }
        if (function_.Get(arguments[j]).type !=
            function_.Get(params[j]).type) {
          Fail(At(terminator) + " passes " + At(arguments[j]) +
               " where " + At(targets[0]) + " expects the type of " +
               At(params[j]));
        }
      }
    }
  }

  std::span<const BlockId> SuccessorsOf(BlockId block) const {
    const InstId terminator = function_.TerminatorOf(block);
    if (!function_.Contains(terminator)) {
      return {};
    }
    return function_.SuccessorsOf(function_.Get(terminator));
  }

  const StringInterner& interner_;
  const TypeStore& types_;
  const Module& module_;
  const Function& function_;
  VerificationResult result_;
  std::vector<bool> reachable_;
  // dominators_[b][a] is true when a dominates b.
  std::vector<std::vector<bool>> dominators_;
};

}  // namespace

VerificationResult Verify(const StringInterner& interner,
                          const TypeStore& types,
                          const Module& module,
                          const Function& function) {
  Verifier verifier(interner, types, module, function);
  return verifier.Run();
}

VerificationResult Verify(const StringInterner& interner,
                          const TypeStore& types,
                          const Module& module) {
  VerificationResult all;
  for (const Function& function : module.functions) {
    VerificationResult one = Verify(interner, types, module, function);
    all.problems.insert(all.problems.end(), one.problems.begin(),
                        one.problems.end());
  }
  return all;
}

}  // namespace ir
}  // namespace wolang
