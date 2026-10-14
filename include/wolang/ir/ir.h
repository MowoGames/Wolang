// Copyright 2026 The Wolang Authors. All rights reserved.

#ifndef WOLANG_IR_IR_H_
#define WOLANG_IR_IR_H_

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "wolang/base/string_interner.h"
#include "wolang/sema/effect.h"
#include "wolang/sema/type.h"

namespace wolang {
namespace ir {

// ---------------------------------------------------------------- opcodes ---

enum class Opcode : uint8_t {
#define WOLANG_IR_OP(Name, Mnemonic, Description) k##Name,
#define WOLANG_IR_TERMINATOR(Name, Mnemonic, Description) k##Name,
#include "wolang/ir/ir_opcode.inc"
  kCount,
};

std::string_view OpcodeMnemonic(Opcode opcode);

// True for the operations that must end a block, and may only appear there.
bool IsTerminator(Opcode opcode);

// True for the operations that produce no value: a store and every
// terminator.
bool ProducesValue(Opcode opcode);

// ---------------------------------------------------------------- handles ---

// An instruction, which is also the value it produces. One id space keeps
// "where did this value come from" a lookup rather than a search.
struct InstId {
  static constexpr uint32_t kInvalidIndex = 0xFFFFFFFFu;
  uint32_t index = kInvalidIndex;

  bool is_valid() const { return index != kInvalidIndex; }
  friend bool operator==(InstId lhs, InstId rhs) {
    return lhs.index == rhs.index;
  }
  friend bool operator!=(InstId lhs, InstId rhs) { return !(lhs == rhs); }
};

struct BlockId {
  static constexpr uint32_t kInvalidIndex = 0xFFFFFFFFu;
  uint32_t index = kInvalidIndex;

  bool is_valid() const { return index != kInvalidIndex; }
  friend bool operator==(BlockId lhs, BlockId rhs) {
    return lhs.index == rhs.index;
  }
  friend bool operator!=(BlockId lhs, BlockId rhs) { return !(lhs == rhs); }
};

struct FunctionId {
  static constexpr uint32_t kInvalidIndex = 0xFFFFFFFFu;
  uint32_t index = kInvalidIndex;

  bool is_valid() const { return index != kInvalidIndex; }
  friend bool operator==(FunctionId lhs, FunctionId rhs) {
    return lhs.index == rhs.index;
  }
  friend bool operator!=(FunctionId lhs, FunctionId rhs) {
    return !(lhs == rhs);
  }
};

// ----------------------------------------------------------- instructions ---

// One operation. Operands, and a payload whose meaning depends on the
// opcode. Kept to a fixed size with the variable parts in side pools, the
// same way the AST is laid out.
struct Inst {
  Opcode opcode = Opcode::kUndef;
  // The type of the value this produces, or an invalid id when it produces
  // none.
  TypeId type;
  // A range into the function's operand pool.
  uint32_t operands_begin = 0;
  uint32_t operands_count = 0;

  // Extract, Insert: which field. Index: unused, the index is an operand.
  uint32_t field = 0;

  union {
    // ConstInt: the value, already truncated to its type.
    uint64_t integer;
    // ConstFloat.
    double floating;
    // ConstBool.
    bool boolean;
    // ConstString: an index into the module's string pool.
    uint32_t string;
    // Call: which function.
    uint32_t callee;
    // Alloca: how many slots, for an array.
    uint32_t slots;
  } payload = {0};

  // Jump, Branch, Switch: a range into the function's successor pool. Jump
  // has one, Branch two, Switch one per case plus the default last.
  uint32_t successors_begin = 0;
  uint32_t successors_count = 0;
  // Switch: a range into the function's case pool, parallel to the first
  // |successors_count - 1| successors.
  uint32_t cases_begin = 0;
  uint32_t cases_count = 0;
};

// ----------------------------------------------------------------- blocks ---

// A basic block: parameters, then straight-line instructions, then exactly
// one terminator. There are no phi nodes -- a value that depends on which
// edge was taken is a block parameter, passed by the jump.
struct Block {
  // Instruction ids, in order. The parameters come first and the terminator
  // last.
  std::vector<InstId> instructions;
  // How many leading instructions are parameters.
  uint32_t param_count = 0;
  // For the dump and for diagnostics.
  std::string label;
};

// --------------------------------------------------------------- function ---

struct Function {
  Symbol name;
  TypeId type;
  // What the function does. Carried into the IR rather than erased, because
  // "this call has no effects" is what licenses deleting it as dead.
  EffectRow effects;
  // Whether |effects| is an answer or an absence. An empty row is an
  // assertion -- "nothing observable" (D38) -- so a function whose row was
  // never inferred must not read as one that does nothing: a declaration
  // from another file, or a specialization lowered before its row existed,
  // is unknown rather than pure.
  bool effects_known = false;
  // Implemented outside the language and reached through the C ABI: the
  // symbol to look up. Invalid for a function written in Wolang. A foreign
  // call is the `unsafe` effect, so the checker has already decided whether
  // it is allowed to be here at all.
  Symbol foreign_symbol;
  // Which declaration this function came from: a file index and an item
  // index inside it. Carried so that a call into another file resolves to
  // one definition rather than to a name two files might both define --
  // the IR knows only unqualified names, and linking by name would be a
  // guess.
  static constexpr uint32_t kNoSource = 0xFFFFFFFFu;
  // Whether the declaration was a `task`: it may suspend, so it is not a
  // function the way the machine means it until the transform in
  // `ir/tasks.h` has split it into a state machine.
  bool is_task = false;
  // A `task` with no body anywhere: the one suspension primitive. The
  // transform writes its machine -- suspend once, then finish -- because
  // there is no body to split. A declaration whose body is in another file
  // is *not* this, and saying so is the difference between a `wait` that
  // works and one that quietly does nothing.
  bool is_primitive_task = false;
  uint32_t source_file = kNoSource;
  uint32_t source_item = kNoSource;
  // Block 0 is the entry, and its parameters are the function's.
  std::vector<Block> blocks;
  std::vector<Inst> instructions;
  // Operands, successors and switch cases of every instruction.
  std::vector<InstId> operands;
  std::vector<BlockId> successors;
  std::vector<uint64_t> cases;
  // Which block each instruction belongs to, indexed by instruction. What
  // makes "is this definition before that use" answerable.
  std::vector<BlockId> owners;

  const Inst& Get(InstId id) const { return instructions[id.index]; }
  Inst& Get(InstId id) { return instructions[id.index]; }
  const Block& Get(BlockId id) const { return blocks[id.index]; }
  Block& Get(BlockId id) { return blocks[id.index]; }

  bool Contains(InstId id) const {
    return id.is_valid() && id.index < instructions.size();
  }
  bool Contains(BlockId id) const {
    return id.is_valid() && id.index < blocks.size();
  }

  std::span<const InstId> OperandsOf(const Inst& inst) const {
    return std::span<const InstId>(operands.data() + inst.operands_begin,
                                   inst.operands_count);
  }
  std::span<const BlockId> SuccessorsOf(const Inst& inst) const {
    return std::span<const BlockId>(successors.data() + inst.successors_begin,
                                    inst.successors_count);
  }
  std::span<const uint64_t> CasesOf(const Inst& inst) const {
    return std::span<const uint64_t>(cases.data() + inst.cases_begin,
                                     inst.cases_count);
  }

  bool is_foreign() const { return foreign_symbol.is_valid(); }

  bool has_source() const { return source_item != kNoSource; }

  // A declaration with no body -- a trait method, an `extern`, a host
  // function -- has a signature and no blocks. Everything that walks blocks
  // has to say so, rather than assuming an entry exists.
  bool is_declaration() const { return blocks.empty(); }

  // The parameters of |block|, which a jump to it has to match.
  std::span<const InstId> ParamsOf(BlockId block) const {
    if (!Contains(block)) {
      return {};
    }
    const Block& target = Get(block);
    return std::span<const InstId>(target.instructions.data(),
                                   target.param_count);
  }

  // The last instruction of |block|, or an invalid id when the block is
  // empty. A well formed block always has one and it is a terminator.
  InstId TerminatorOf(BlockId block) const {
    const Block& target = Get(block);
    return target.instructions.empty() ? InstId{}
                                       : target.instructions.back();
  }

  BlockId entry() const { return BlockId{0}; }
};

// ----------------------------------------------------------------- module ---

struct Module {
  std::vector<Function> functions;
  // String literals, shared by every function.
  std::vector<std::string> strings;

  const Function* Find(Symbol name) const;
};

}  // namespace ir
}  // namespace wolang

#endif  // WOLANG_IR_IR_H_
