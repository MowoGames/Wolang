// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/ir/ir.h"

#include "harness/test.h"
#include "wolang/ir/printer.h"
#include "wolang/ir/verifier.h"

namespace wolang {
namespace {

// Builds small functions by hand, so the IR's own invariants can be tested
// without going through lowering. A broken lowering and a broken verifier
// would otherwise hide each other.
class Builder {
 public:
  Builder() {
    module_.functions.emplace_back();
    function().name = interner_.Intern("f");
    function().type = types_.Function({}, types_.void_type());
    AddBlock("entry");
  }

  StringInterner& interner() { return interner_; }
  TypeStore& types() { return types_; }
  Module& module() { return module_; }
  Function& function() { return module_.functions.front(); }

  BlockId AddBlock(std::string label) {
    const BlockId id{static_cast<uint32_t>(function().blocks.size())};
    Block block;
    block.label = std::move(label);
    function().blocks.push_back(std::move(block));
    return id;
  }

  InstId Add(BlockId block,
             Opcode opcode,
             TypeId type,
             std::vector<InstId> operands = {}) {
    Function& f = function();
    Inst inst;
    inst.opcode = opcode;
    inst.type = type;
    inst.operands_begin = static_cast<uint32_t>(f.operands.size());
    inst.operands_count = static_cast<uint32_t>(operands.size());
    f.operands.insert(f.operands.end(), operands.begin(), operands.end());

    const InstId id{static_cast<uint32_t>(f.instructions.size())};
    f.instructions.push_back(inst);
    f.owners.push_back(block);
    // A parameter has to come before everything else in its block, which the
    // builder keeps true so that a test has to break it on purpose.
    Block& target = f.Get(block);
    if (opcode == Opcode::kParam) {
      target.instructions.insert(
          target.instructions.begin() + target.param_count, id);
      ++target.param_count;
    } else {
      target.instructions.push_back(id);
    }
    return id;
  }

  InstId ConstInt(BlockId block, uint64_t value) {
    const InstId id = Add(block, Opcode::kConstInt, types_.default_int());
    function().Get(id).payload.integer = value;
    return id;
  }

  InstId ConstBool(BlockId block, bool value) {
    const InstId id = Add(block, Opcode::kConstBool, types_.bool_type());
    function().Get(id).payload.boolean = value;
    return id;
  }

  void Terminate(BlockId block,
                 Opcode opcode,
                 std::vector<InstId> operands,
                 std::vector<BlockId> successors) {
    const InstId id = Add(block, opcode, TypeId{}, std::move(operands));
    Function& f = function();
    Inst& inst = f.Get(id);
    inst.successors_begin = static_cast<uint32_t>(f.successors.size());
    inst.successors_count = static_cast<uint32_t>(successors.size());
    f.successors.insert(f.successors.end(), successors.begin(),
                        successors.end());
  }

  VerificationResult Verify() {
    return ::wolang::Verify(interner_, types_, module_, function());
  }

  std::string Print() {
    return PrintFunction(interner_, types_, module_, function());
  }

 private:
  StringInterner interner_;
  TypeStore types_;
  Module module_;
};

WOLANG_TEST(IrTest, OpcodeTableIsComplete) {
  for (uint16_t i = 0; i < static_cast<uint16_t>(Opcode::kCount); ++i) {
    const auto opcode = static_cast<Opcode>(i);
    WOLANG_EXPECT(!OpcodeMnemonic(opcode).empty());
  }
  WOLANG_EXPECT_EQ(OpcodeMnemonic(Opcode::kAdd), "add");
  WOLANG_EXPECT_EQ(OpcodeMnemonic(Opcode::kReturn), "ret");
}

WOLANG_TEST(IrTest, TerminatorsAndValuesAreDistinguished) {
  WOLANG_EXPECT(IsTerminator(Opcode::kReturn));
  WOLANG_EXPECT(IsTerminator(Opcode::kBranch));
  WOLANG_EXPECT(!IsTerminator(Opcode::kAdd));

  // A store and every terminator produce nothing; everything else does.
  WOLANG_EXPECT(!ProducesValue(Opcode::kStore));
  WOLANG_EXPECT(!ProducesValue(Opcode::kJump));
  WOLANG_EXPECT(ProducesValue(Opcode::kLoad));
  WOLANG_EXPECT(ProducesValue(Opcode::kParam));
}

WOLANG_TEST(VerifierTest, AcceptsAWellFormedFunction) {
  Builder b;
  const BlockId entry = b.function().entry();
  const InstId one = b.ConstInt(entry, 1);
  const InstId two = b.ConstInt(entry, 2);
  const InstId sum = b.Add(entry, Opcode::kAdd, b.types().default_int(),
                           {one, two});
  b.Terminate(entry, Opcode::kReturn, {sum}, {});

  const VerificationResult result = b.Verify();
  WOLANG_EXPECT(result.ok());
}

WOLANG_TEST(VerifierTest, RejectsABlockWithNoTerminator) {
  Builder b;
  const BlockId entry = b.function().entry();
  b.ConstInt(entry, 1);

  const VerificationResult result = b.Verify();
  WOLANG_ASSERT(!result.ok());
  WOLANG_EXPECT(result.problems.front().find("not a terminator") !=
                std::string::npos);
}

WOLANG_TEST(VerifierTest, RejectsATerminatorInTheMiddle) {
  Builder b;
  const BlockId entry = b.function().entry();
  b.Terminate(entry, Opcode::kReturn, {}, {});
  b.ConstInt(entry, 1);

  const VerificationResult result = b.Verify();
  WOLANG_ASSERT(!result.ok());
  WOLANG_EXPECT(result.problems.front().find("not its last instruction") !=
                std::string::npos);
}

WOLANG_TEST(VerifierTest, RejectsAUseBeforeItsDefinition) {
  Builder b;
  const BlockId entry = b.function().entry();
  const InstId one = b.ConstInt(entry, 1);
  const InstId sum = b.Add(entry, Opcode::kAdd, b.types().default_int(),
                           {one, one});
  // Swap the two, so the add comes first and uses a value defined after it.
  Block& block = b.function().Get(entry);
  std::swap(block.instructions[0], block.instructions[1]);
  b.Terminate(entry, Opcode::kReturn, {sum}, {});

  const VerificationResult result = b.Verify();
  WOLANG_ASSERT(!result.ok());
  WOLANG_EXPECT(result.problems.front().find("before it is defined") !=
                std::string::npos);
}

WOLANG_TEST(VerifierTest, RejectsAUseThatIsNotDominated) {
  // The definition is on one arm of a branch and the use is after the join,
  // so it is defined on one path and not the other. This is the check a phi
  // based IR gets wrong silently.
  Builder b;
  const BlockId entry = b.function().entry();
  const BlockId left = b.AddBlock("left");
  const BlockId right = b.AddBlock("right");
  const BlockId join = b.AddBlock("join");

  const InstId condition = b.ConstBool(entry, true);
  b.Terminate(entry, Opcode::kBranch, {condition}, {left, right});

  const InstId only_on_the_left = b.ConstInt(left, 7);
  b.Terminate(left, Opcode::kJump, {}, {join});
  b.Terminate(right, Opcode::kJump, {}, {join});

  const InstId use = b.Add(join, Opcode::kNeg, b.types().default_int(),
                           {only_on_the_left});
  b.Terminate(join, Opcode::kReturn, {use}, {});

  const VerificationResult result = b.Verify();
  WOLANG_ASSERT(!result.ok());
  WOLANG_EXPECT(result.problems.front().find("does not dominate") !=
                std::string::npos);
}

WOLANG_TEST(VerifierTest, AcceptsTheSameValuePassedAsABlockParameter) {
  // The fix for the case above: the join block takes a parameter, and each
  // arm passes its own value.
  Builder b;
  const BlockId entry = b.function().entry();
  const BlockId left = b.AddBlock("left");
  const BlockId right = b.AddBlock("right");
  const BlockId join = b.AddBlock("join");

  const InstId condition = b.ConstBool(entry, true);
  b.Terminate(entry, Opcode::kBranch, {condition}, {left, right});

  const InstId parameter =
      b.Add(join, Opcode::kParam, b.types().default_int());

  const InstId seven = b.ConstInt(left, 7);
  b.Terminate(left, Opcode::kJump, {seven}, {join});
  const InstId eight = b.ConstInt(right, 8);
  b.Terminate(right, Opcode::kJump, {eight}, {join});

  b.Terminate(join, Opcode::kReturn, {parameter}, {});

  const VerificationResult result = b.Verify();
  WOLANG_EXPECT(result.ok());
}

WOLANG_TEST(VerifierTest, RejectsAJumpWithTheWrongNumberOfArguments) {
  Builder b;
  const BlockId entry = b.function().entry();
  const BlockId target = b.AddBlock("target");
  b.Add(target, Opcode::kParam, b.types().default_int());

  b.Terminate(entry, Opcode::kJump, {}, {target});
  b.Terminate(target, Opcode::kReturn, {}, {});

  const VerificationResult result = b.Verify();
  WOLANG_ASSERT(!result.ok());
  WOLANG_EXPECT(result.problems.front().find("passes 0 arguments") !=
                std::string::npos);
}

WOLANG_TEST(VerifierTest, RejectsAJumpWithAMismatchedArgument) {
  Builder b;
  const BlockId entry = b.function().entry();
  const BlockId target = b.AddBlock("target");
  b.Add(target, Opcode::kParam, b.types().default_int());

  const InstId wrong = b.ConstBool(entry, true);
  b.Terminate(entry, Opcode::kJump, {wrong}, {target});
  b.Terminate(target, Opcode::kReturn, {}, {});

  const VerificationResult result = b.Verify();
  WOLANG_ASSERT(!result.ok());
  WOLANG_EXPECT(result.problems.front().find("expects the type of") !=
                std::string::npos);
}

WOLANG_TEST(VerifierTest, RejectsAnUnreachableBlock) {
  Builder b;
  const BlockId entry = b.function().entry();
  const BlockId orphan = b.AddBlock("orphan");
  b.Terminate(entry, Opcode::kReturn, {}, {});
  b.Terminate(orphan, Opcode::kReturn, {}, {});

  const VerificationResult result = b.Verify();
  WOLANG_ASSERT(!result.ok());
  WOLANG_EXPECT(result.problems.front().find("not reachable") !=
                std::string::npos);
}

WOLANG_TEST(VerifierTest, RejectsMismatchedOperandTypes) {
  Builder b;
  const BlockId entry = b.function().entry();
  const InstId number = b.ConstInt(entry, 1);
  const InstId flag = b.ConstBool(entry, true);
  const InstId sum = b.Add(entry, Opcode::kAdd, b.types().default_int(),
                           {number, flag});
  b.Terminate(entry, Opcode::kReturn, {sum}, {});

  const VerificationResult result = b.Verify();
  WOLANG_ASSERT(!result.ok());
  WOLANG_EXPECT(result.problems.front().find("two different types") !=
                std::string::npos);
}

WOLANG_TEST(VerifierTest, RejectsBranchingOnANonBool) {
  Builder b;
  const BlockId entry = b.function().entry();
  const BlockId left = b.AddBlock("left");
  const BlockId right = b.AddBlock("right");
  const InstId number = b.ConstInt(entry, 1);
  b.Terminate(entry, Opcode::kBranch, {number}, {left, right});
  b.Terminate(left, Opcode::kReturn, {}, {});
  b.Terminate(right, Opcode::kReturn, {}, {});

  const VerificationResult result = b.Verify();
  WOLANG_ASSERT(!result.ok());
  WOLANG_EXPECT(result.problems.front().find("not a bool") !=
                std::string::npos);
}

WOLANG_TEST(VerifierTest, RejectsAValueWithNoType) {
  Builder b;
  const BlockId entry = b.function().entry();
  const InstId bad = b.Add(entry, Opcode::kConstInt, TypeId{});
  b.Terminate(entry, Opcode::kReturn, {bad}, {});

  const VerificationResult result = b.Verify();
  WOLANG_ASSERT(!result.ok());
  WOLANG_EXPECT(result.problems.front().find("no type") != std::string::npos);
}

WOLANG_TEST(PrinterTest, NamesValuesByTheirIndex) {
  Builder b;
  const BlockId entry = b.function().entry();
  const InstId one = b.ConstInt(entry, 1);
  const InstId two = b.ConstInt(entry, 2);
  const InstId sum = b.Add(entry, Opcode::kAdd, b.types().default_int(),
                           {one, two});
  b.Terminate(entry, Opcode::kReturn, {sum}, {});

  WOLANG_EXPECT_EQ(b.Print(),
                   "fun @f() {\n"
                   "^0:  // entry\n"
                   "  %0 = const.int 1\n"
                   "  %1 = const.int 2\n"
                   "  %2 = add %0, %1\n"
                   "  ret %2\n"
                   "}\n");
}

WOLANG_TEST(PrinterTest, PrintsBlockParametersAndEdges) {
  Builder b;
  const BlockId entry = b.function().entry();
  const BlockId join = b.AddBlock("join");
  const InstId parameter =
      b.Add(join, Opcode::kParam, b.types().default_int());
  const InstId seven = b.ConstInt(entry, 7);
  b.Terminate(entry, Opcode::kJump, {seven}, {join});
  b.Terminate(join, Opcode::kReturn, {parameter}, {});

  WOLANG_EXPECT_EQ(b.Print(),
                   "fun @f() {\n"
                   "^0:  // entry\n"
                   "  %1 = const.int 7\n"
                   "  jmp %1 ^1\n"
                   "^1(%0: i32):  // join\n"
                   "  ret %0\n"
                   "}\n");
}

}  // namespace
}  // namespace wolang
