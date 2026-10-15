// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/ir/tasks.h"

#include <algorithm>
#include <cstddef>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "wolang/ir/analysis.h"

namespace wolang {
namespace ir {
namespace {

constexpr std::string_view kRecursiveTask = "E0304";
constexpr std::string_view kForeignTask = "E0305";

// The symbol the frame of a driven task is allocated through. Declared per
// frame type so that the IR stays typed: one symbol, several declarations,
// each with the pointer type its caller wants.
constexpr std::string_view kAllocSymbol = "wolang_frame_alloc";
constexpr std::string_view kFreeSymbol = "wolang_frame_free";

// What the scheduler calls to advance any task by one step.
constexpr std::string_view kDispatchName = "wolang$dispatch";

// Handing a task to the scheduler.
constexpr std::string_view kSpawnSymbol = "wolang_sched_spawn";

// What a task's frame holds, and where.
//
// The state comes first because every machine reads it before anything
// else. Then the parameters, because the driver writes them before the
// machine starts; then the result, which the driver reads after it ends;
// then one field per slot the body keeps, and one per `wait` site for the
// callee's own frame.
struct Frame {
  TypeId type;
  TypeId pointer;
  uint32_t state_field = 0;
  uint32_t first_param_field = 1;
  uint32_t result_field = 0;
  bool has_result = false;
  // By alloca instruction id.
  std::vector<std::pair<InstId, uint32_t>> slots;
  // By wait instruction id, in the order they appear.
  std::vector<std::pair<InstId, uint32_t>> waits;
  uint32_t size = 0;
  // The function that advances this machine by one step, and the number
  // the dispatcher knows it by.
  uint32_t resume = 0;
  uint32_t kind = 0;
};

class Splitter {
 public:
  Splitter(StringInterner* interner,
           TypeStore* types,
           abi::LayoutStore* layouts,
           Module* module,
           const std::function<SourceSpan(uint32_t)>& span_of,
           std::vector<Diagnostic>* diagnostics)
      : interner_(interner),
        types_(types),
        layouts_(layouts),
        module_(module),
        span_of_(span_of),
        diagnostics_(diagnostics) {}

  // `spawn F(args)` becomes a frame, the arguments written into it, and a
  // call to the scheduler. The instruction that was the spawn *becomes* the
  // call, so the handle keeps its id and nothing that used it is rewritten
  // -- the same trick inlining uses.
  void ExpandSpawns(uint32_t index) {
    for (uint32_t b = 0; b < module_->functions[index].blocks.size(); ++b) {
      for (size_t position = 0;
           position < module_->functions[index].blocks[b].instructions.size();
           ++position) {
        const InstId id =
            module_->functions[index].blocks[b].instructions[position];
        if (module_->functions[index].Get(id).opcode !=
            Opcode::kSpawnCall) {
          continue;
        }
        position = ExpandSpawn(index, BlockId{b}, position, id);
      }
    }
  }

  size_t ExpandSpawn(uint32_t index,
                     BlockId block,
                     size_t position,
                     InstId id) {
    const Inst spawn = module_->functions[index].Get(id);
    const Frame* callee = FrameOf(spawn.payload.callee);
    if (callee == nullptr) {
      // Not a task after all, or one that could not be split. The checker
      // and the recursion report have already said so.
      return position;
    }

    // The declarations this needs, created before anything takes a pointer
    // into the function: adding a declaration can move the vector.
    const uint32_t allocate = Allocator(*callee, /*release=*/false);
    const uint32_t spawner = Spawner();
    const std::vector<InstId> arguments(
        module_->functions[index].operands.begin() +
            spawn.operands_begin,
        module_->functions[index].operands.begin() + spawn.operands_begin +
            spawn.operands_count);

    Function* target = &module_->functions[index];
    // Built in a side list and spliced in before the spawn, so the handle
    // stays where it was.
    std::vector<InstId> prelude;
    const auto emit = [&](Inst inst) {
      const InstId fresh{
          static_cast<uint32_t>(target->instructions.size())};
      target->instructions.push_back(inst);
      target->owners.push_back(block);
      // The span table runs parallel to the instructions (D118).
      target->spans.push_back(target->SpanOf(InstId{}));
      prelude.push_back(fresh);
      return fresh;
    };
    const auto constant = [&](TypeId type, uint64_t value) {
      Inst inst;
      inst.opcode = Opcode::kConstInt;
      inst.type = type;
      inst.payload.integer = value;
      return emit(inst);
    };
    const auto call = [&](uint32_t callee_index, TypeId type,
                          std::vector<InstId> operands) {
      Inst inst;
      inst.opcode = Opcode::kCall;
      inst.type = type;
      inst.payload.callee = callee_index;
      inst.operands_begin = static_cast<uint32_t>(target->operands.size());
      inst.operands_count = static_cast<uint32_t>(operands.size());
      for (InstId operand : operands) {
        target->operands.push_back(operand);
      }
      return emit(inst);
    };
    const auto field_address = [&](InstId base, uint32_t field,
                                   TypeId type) {
      Inst inst;
      inst.opcode = Opcode::kFieldAddress;
      inst.type = types_->Pointer(type, /*is_mutable=*/true);
      inst.field = field;
      inst.operands_begin = static_cast<uint32_t>(target->operands.size());
      inst.operands_count = 1;
      target->operands.push_back(base);
      return emit(inst);
    };
    const auto store = [&](InstId address, InstId value) {
      Inst inst;
      inst.opcode = Opcode::kStore;
      inst.operands_begin = static_cast<uint32_t>(target->operands.size());
      inst.operands_count = 2;
      target->operands.push_back(address);
      target->operands.push_back(value);
      emit(inst);
    };

    const TypeId size_type = types_->Int(IntWidth::k64, false);
    const InstId frame =
        call(allocate, callee->pointer, {constant(size_type, callee->size)});

    const TypeId state_type = FieldType(*callee, callee->state_field);
    store(field_address(frame, callee->state_field, state_type),
          constant(state_type, 0));

    const std::vector<TypeId> fields = types_->ArgumentsOf(callee->type);
    for (size_t i = 0; i < arguments.size(); ++i) {
      const uint32_t field =
          callee->first_param_field + static_cast<uint32_t>(i);
      if (field >= fields.size()) {
        break;
      }
      store(field_address(frame, field, fields[field]), arguments[i]);
    }

    const TypeId opaque =
        types_->Pointer(types_->Int(IntWidth::k8, false), true);
    Inst cast;
    cast.opcode = Opcode::kCast;
    cast.type = opaque;
    cast.operands_begin = static_cast<uint32_t>(target->operands.size());
    cast.operands_count = 1;
    target->operands.push_back(frame);
    const InstId opaque_frame = emit(cast);

    const TypeId kind_type = types_->Int(IntWidth::k32, true);
    const InstId kind = constant(kind_type, callee->kind);

    // The spawn becomes the call that hands the frame over: same id, same
    // type, so the handle is already where everything expects it.
    Inst handed;
    handed.opcode = Opcode::kCall;
    handed.type = spawn.type.is_valid() ? spawn.type : kind_type;
    handed.payload.callee = spawner;
    handed.operands_begin = static_cast<uint32_t>(target->operands.size());
    handed.operands_count = 2;
    target->operands.push_back(kind);
    target->operands.push_back(opaque_frame);
    target->Get(id) = handed;

    Block& body = target->Get(block);
    body.instructions.insert(body.instructions.begin() +
                                 static_cast<long>(position),
                             prelude.begin(), prelude.end());
    return position + prelude.size();
  }

  uint32_t Spawner() {
    const Symbol symbol = interner_->Intern(std::string(kSpawnSymbol));
    for (uint32_t i = 0; i < module_->functions.size(); ++i) {
      if (module_->functions[i].name == symbol) {
        return i;
      }
    }
    const TypeId opaque =
        types_->Pointer(types_->Int(IntWidth::k8, false), true);
    Function declaration;
    declaration.name = symbol;
    declaration.type = types_->Function(
        {types_->Int(IntWidth::k32, true), opaque},
        types_->Int(IntWidth::k32, true));
    declaration.foreign_symbol = symbol;
    declaration.effects_known = false;
    module_->functions.push_back(std::move(declaration));
    return static_cast<uint32_t>(module_->functions.size() - 1);
  }

  // One entry point the scheduler can use for any task: a switch over the
  // kinds, calling each machine's own resume. A handle is then two integers
  // -- a kind and a frame -- and nothing has to hold a function pointer,
  // which is also what keeps this working identically in both tiers.
  void EmitDispatch() {
    if (kinds_.empty()) {
      return;
    }
    const TypeId kind_type = types_->Int(IntWidth::k32, true);
    const TypeId opaque =
        types_->Pointer(types_->Int(IntWidth::k8, false),
                        /*is_mutable=*/true);

    Function dispatch;
    dispatch.name = interner_->Intern(std::string(kDispatchName));
    dispatch.type =
        types_->Function({kind_type, opaque}, types_->bool_type());
    dispatch.effects_known = false;
    const auto position = static_cast<uint32_t>(module_->functions.size());
    module_->functions.push_back(std::move(dispatch));
    Function* target = &module_->functions[position];

    const BlockId entry = AddBlock(target, "entry");
    Inst kind_param;
    kind_param.opcode = Opcode::kParam;
    kind_param.type = kind_type;
    const InstId kind{static_cast<uint32_t>(target->instructions.size())};
    target->instructions.push_back(kind_param);
    target->owners.push_back(entry);
    // The span table runs parallel to the instructions (D118).
    target->spans.push_back(target->SpanOf(InstId{}));
    target->Get(entry).instructions.push_back(kind);
    Inst frame_param;
    frame_param.opcode = Opcode::kParam;
    frame_param.type = opaque;
    const InstId frame{static_cast<uint32_t>(target->instructions.size())};
    target->instructions.push_back(frame_param);
    target->owners.push_back(entry);
    // The span table runs parallel to the instructions (D118).
    target->spans.push_back(target->SpanOf(InstId{}));
    target->Get(entry).instructions.push_back(frame);
    target->Get(entry).param_count = 2;

    std::vector<BlockId> arms;
    for (const auto& [index, resume] : kinds_) {
      const Frame* frame_of = FrameOf(index);
      const BlockId arm = AddBlock(
          target, "kind." + std::string(interner_->Text(
                                module_->functions[index].name)));
      const InstId typed = Add(target, arm, Opcode::kCast,
                               frame_of->pointer, {frame});
      const InstId done =
          Add(target, arm, Opcode::kCall, types_->bool_type(), {typed});
      target->Get(done).payload.callee = resume;
      Return(target, arm, done);
      arms.push_back(arm);
    }

    const BlockId nowhere = AddBlock(target, "nokind");
    Inst unreachable;
    unreachable.opcode = Opcode::kUnreachable;
    const InstId stop{static_cast<uint32_t>(target->instructions.size())};
    target->instructions.push_back(unreachable);
    target->owners.push_back(nowhere);
    // The span table runs parallel to the instructions (D118).
    target->spans.push_back(target->SpanOf(InstId{}));
    target->Get(nowhere).instructions.push_back(stop);

    Inst switch_inst;
    switch_inst.opcode = Opcode::kSwitch;
    switch_inst.operands_begin =
        static_cast<uint32_t>(target->operands.size());
    switch_inst.operands_count = 1;
    target->operands.push_back(kind);
    switch_inst.successors_begin =
        static_cast<uint32_t>(target->successors.size());
    switch_inst.cases_begin = static_cast<uint32_t>(target->cases.size());
    for (size_t i = 0; i < arms.size(); ++i) {
      target->successors.push_back(arms[i]);
      target->cases.push_back(i);
    }
    target->successors.push_back(nowhere);
    switch_inst.successors_count = static_cast<uint32_t>(arms.size() + 1);
    switch_inst.cases_count = static_cast<uint32_t>(arms.size());
    const InstId id{static_cast<uint32_t>(target->instructions.size())};
    target->instructions.push_back(switch_inst);
    target->owners.push_back(entry);
    // The span table runs parallel to the instructions (D118).
    target->spans.push_back(target->SpanOf(InstId{}));
    target->Get(entry).instructions.push_back(id);
  }

  void Run() {
    // The frame of a task that waits on another contains that task's frame,
    // so the callees have to be laid out first. A cycle is a task that
    // waits on itself, directly or not: its frame would contain itself, and
    // there is no size that satisfies that.
    const std::vector<uint32_t> order = Order();
    for (uint32_t index : order) {
      Split(index);
    }
    EmitDispatch();
    // A `spawn` can be anywhere, including in a function that is not a
    // task, so this is its own walk over everything.
    for (uint32_t i = 0; i < module_->functions.size(); ++i) {
      ExpandSpawns(i);
    }
  }

 private:
  // ------------------------------------------------------------- ordering --

  std::vector<uint32_t> Order() {
    const size_t count = module_->functions.size();
    std::vector<uint32_t> result;
    std::vector<uint8_t> state(count, 0);  // 0 = new, 1 = on the stack, 2 = done
    for (uint32_t i = 0; i < count; ++i) {
      const Function& function = module_->functions[i];
      // A task whose body is in another file has its machine there too.
      // Splitting it here would write a second machine that shares nothing
      // with the real one.
      if (function.is_task &&
          (!function.is_declaration() || function.is_primitive_task)) {
        VisitTask(i, &state, &result);
      }
    }
    return result;
  }

  void VisitTask(uint32_t index,
                 std::vector<uint8_t>* state,
                 std::vector<uint32_t>* result) {
    if ((*state)[index] == 2) {
      return;
    }
    if ((*state)[index] == 1) {
      ReportRecursion(index);
      return;
    }
    (*state)[index] = 1;
    for (const Inst& inst : module_->functions[index].instructions) {
      if (inst.opcode != Opcode::kWaitCall) {
        continue;
      }
      const uint32_t callee = inst.payload.callee;
      if (callee < module_->functions.size() &&
          module_->functions[callee].is_task) {
        VisitTask(callee, state, result);
      }
    }
    (*state)[index] = 2;
    result->push_back(index);
  }

  // A `wait` on a task this file cannot see the inside of. The frame of a
  // task is built from its body -- its slots and the frames of what it
  // waits on -- so a declaration is not enough, and lowering is per file.
  //
  // The way out is a calling convention: each task would also get
  // generated `$framesize`, `$start` and `$result` entry points, so a
  // caller could allocate and fill a frame it cannot lay out. That is the
  // same whole-program step cross-file monomorphization needs, and it
  // belongs with it.
  void ReportUnknownFrame(uint32_t callee) {
    if (callee >= module_->functions.size() ||
        !module_->functions[callee].is_task ||
        reported_.count(callee) != 0) {
      return;
    }
    reported_.insert(callee);
    std::string message = "`";
    message.append(interner_->Text(module_->functions[callee].name));
    message.append("` is a task from another module, and `wait` needs the "
                   "frame it keeps its state in");
    Diagnostic diagnostic;
    diagnostic.severity = Severity::kError;
    diagnostic.code = std::string(kForeignTask);
    diagnostic.message = std::move(message);
    DiagnosticLabel label;
    label.is_primary = true;
    label.span = span_of_(module_->functions[callee].source_item);
    label.message = "spawn it instead, or move the `wait` into its module";
    diagnostic.labels.push_back(std::move(label));
    diagnostics_->push_back(std::move(diagnostic));
  }

  void ReportRecursion(uint32_t index) {
    if (reported_.count(index) != 0) {
      return;
    }
    reported_.insert(index);
    std::string message = "`";
    message.append(interner_->Text(module_->functions[index].name));
    message.append("` waits on itself, so its frame would contain itself");
    Diagnostic diagnostic;
    diagnostic.severity = Severity::kError;
    diagnostic.code = std::string(kRecursiveTask);
    diagnostic.message = std::move(message);
    DiagnosticLabel label;
    label.is_primary = true;
    label.span = span_of_(module_->functions[index].source_item);
    label.message = "give it something to wait on that is not itself";
    diagnostic.labels.push_back(std::move(label));
    diagnostics_->push_back(std::move(diagnostic));
  }

  // ---------------------------------------------------------- frame layout --

  const Frame* FrameOf(uint32_t index) {
    const auto found = frames_.find(index);
    return found == frames_.end() ? nullptr : &found->second;
  }

  Frame BuildFrame(uint32_t index) {
    const Function& function = module_->functions[index];
    std::vector<TypeId> fields;
    Frame frame;

    frame.state_field = static_cast<uint32_t>(fields.size());
    fields.push_back(types_->Int(IntWidth::k32, /*is_signed=*/true));

    frame.first_param_field = static_cast<uint32_t>(fields.size());
    for (TypeId param : types_->ArgumentsOf(function.type)) {
      fields.push_back(param);
    }

    const TypeId returned = types_->ElementOf(function.type);
    if (returned.is_valid() &&
        types_->KindOf(returned) != TypeKind::kVoid) {
      frame.has_result = true;
      frame.result_field = static_cast<uint32_t>(fields.size());
      fields.push_back(returned);
    }

    for (uint32_t i = 0; i < function.instructions.size(); ++i) {
      const Inst& inst = function.instructions[i];
      if (inst.opcode == Opcode::kAlloca) {
        frame.slots.emplace_back(InstId{i},
                                 static_cast<uint32_t>(fields.size()));
        fields.push_back(inst.type);
        continue;
      }
      if (inst.opcode != Opcode::kWaitCall) {
        continue;
      }
      const Frame* callee = FrameOf(inst.payload.callee);
      frame.waits.emplace_back(InstId{i},
                               static_cast<uint32_t>(fields.size()));
      // A callee whose frame is unknown -- a recursive task, already
      // reported -- gets a state-sized placeholder so the layout stays
      // well formed.
      fields.push_back(callee != nullptr
                           ? callee->type
                           : types_->Int(IntWidth::k32, true));
    }

    frame.type = types_->Tuple(fields);
    frame.pointer = types_->Pointer(frame.type, /*is_mutable=*/true);
    frame.size = layouts_->SizeOf(frame.type);
    return frame;
  }

  // ------------------------------------------------------------- building --

  // One instruction appended to |block| of |target|.
  InstId Add(Function* target,
             BlockId block,
             Opcode opcode,
             TypeId type,
             std::vector<InstId> operands = {},
             uint32_t field = 0) {
    Inst inst;
    inst.opcode = opcode;
    inst.type = type;
    inst.field = field;
    inst.operands_begin = static_cast<uint32_t>(target->operands.size());
    inst.operands_count = static_cast<uint32_t>(operands.size());
    for (InstId operand : operands) {
      target->operands.push_back(operand);
    }
    const InstId id{static_cast<uint32_t>(target->instructions.size())};
    target->instructions.push_back(inst);
    target->owners.push_back(block);
    // The span table runs parallel to the instructions (D118).
    target->spans.push_back(target->SpanOf(InstId{}));
    target->Get(block).instructions.push_back(id);
    return id;
  }

  BlockId AddBlock(Function* target, std::string label) {
    Block block;
    block.label = std::move(label);
    target->blocks.push_back(std::move(block));
    return BlockId{static_cast<uint32_t>(target->blocks.size() - 1)};
  }

  void Jump(Function* target, BlockId from, BlockId to,
            std::vector<InstId> arguments = {}) {
    Inst inst;
    inst.opcode = Opcode::kJump;
    inst.operands_begin = static_cast<uint32_t>(target->operands.size());
    inst.operands_count = static_cast<uint32_t>(arguments.size());
    for (InstId argument : arguments) {
      target->operands.push_back(argument);
    }
    inst.successors_begin =
        static_cast<uint32_t>(target->successors.size());
    inst.successors_count = 1;
    target->successors.push_back(to);
    const InstId id{static_cast<uint32_t>(target->instructions.size())};
    target->instructions.push_back(inst);
    target->owners.push_back(from);
    // The span table runs parallel to the instructions (D118).
    target->spans.push_back(target->SpanOf(InstId{}));
    target->Get(from).instructions.push_back(id);
  }

  void Branch(Function* target, BlockId from, InstId condition, BlockId yes,
              BlockId no) {
    Inst inst;
    inst.opcode = Opcode::kBranch;
    inst.operands_begin = static_cast<uint32_t>(target->operands.size());
    inst.operands_count = 1;
    target->operands.push_back(condition);
    inst.successors_begin =
        static_cast<uint32_t>(target->successors.size());
    inst.successors_count = 2;
    target->successors.push_back(yes);
    target->successors.push_back(no);
    const InstId id{static_cast<uint32_t>(target->instructions.size())};
    target->instructions.push_back(inst);
    target->owners.push_back(from);
    // The span table runs parallel to the instructions (D118).
    target->spans.push_back(target->SpanOf(InstId{}));
    target->Get(from).instructions.push_back(id);
  }

  void Return(Function* target, BlockId from, InstId value) {
    Inst inst;
    inst.opcode = Opcode::kReturn;
    inst.operands_begin = static_cast<uint32_t>(target->operands.size());
    inst.operands_count = value.is_valid() ? 1 : 0;
    if (value.is_valid()) {
      target->operands.push_back(value);
    }
    const InstId id{static_cast<uint32_t>(target->instructions.size())};
    target->instructions.push_back(inst);
    target->owners.push_back(from);
    // The span table runs parallel to the instructions (D118).
    target->spans.push_back(target->SpanOf(InstId{}));
    target->Get(from).instructions.push_back(id);
  }

  InstId Constant(Function* target, BlockId block, TypeId type,
                  uint64_t value) {
    Inst inst;
    inst.opcode = Opcode::kConstInt;
    inst.type = type;
    inst.payload.integer = value;
    const InstId id{static_cast<uint32_t>(target->instructions.size())};
    target->instructions.push_back(inst);
    target->owners.push_back(block);
    // The span table runs parallel to the instructions (D118).
    target->spans.push_back(target->SpanOf(InstId{}));
    target->Get(block).instructions.push_back(id);
    return id;
  }

  InstId Bool(Function* target, BlockId block, bool value) {
    Inst inst;
    inst.opcode = Opcode::kConstBool;
    inst.type = types_->bool_type();
    inst.payload.boolean = value;
    const InstId id{static_cast<uint32_t>(target->instructions.size())};
    target->instructions.push_back(inst);
    target->owners.push_back(block);
    // The span table runs parallel to the instructions (D118).
    target->spans.push_back(target->SpanOf(InstId{}));
    target->Get(block).instructions.push_back(id);
    return id;
  }

  // The address of one field of the frame.
  InstId FieldAddress(Function* target,
                      BlockId block,
                      const Frame& frame,
                      InstId frame_pointer,
                      uint32_t field,
                      TypeId field_type) {
    return Add(target, block, Opcode::kFieldAddress,
               types_->Pointer(field_type, /*is_mutable=*/true),
               {frame_pointer}, field);
  }

  TypeId FieldType(const Frame& frame, uint32_t field) {
    const std::vector<TypeId> fields = types_->ArgumentsOf(frame.type);
    return field < fields.size() ? fields[field] : types_->error();
  }

  // ------------------------------------------------------------ splitting --

  void Split(uint32_t index) {
    const Frame frame = BuildFrame(index);
    frames_.emplace(index, frame);

    const uint32_t resume = module_->functions[index].is_declaration()
                                ? EmitPrimitive(index, frames_[index])
                                : EmitResume(index, frames_[index]);
    frames_[index].resume = resume;
    frames_[index].kind = static_cast<uint32_t>(kinds_.size());
    kinds_.emplace_back(index, resume);
    EmitDriver(index, frames_[index], resume);
    module_->functions[index].is_task = false;
  }

  // A task with no body is the platform floor for suspension: it suspends
  // once and then finishes, which is exactly what yielding is. Everything
  // else a scheduler wants -- sleeping, waiting on a socket -- is this plus
  // a condition, and arrives with the scheduler.
  uint32_t EmitPrimitive(uint32_t index, const Frame& frame) {
    Function resume;
    resume.name = ResumeName(index);
    resume.type = types_->Function({frame.pointer}, types_->bool_type());
    resume.effects_known = true;

    const auto position = static_cast<uint32_t>(module_->functions.size());
    module_->functions.push_back(std::move(resume));
    Function* target = &module_->functions[position];

    const BlockId entry = AddBlock(target, "entry");
    const TypeId state_type = FieldType(frame, frame.state_field);
    Inst param;
    param.opcode = Opcode::kParam;
    param.type = frame.pointer;
    const InstId frame_pointer{
        static_cast<uint32_t>(target->instructions.size())};
    target->instructions.push_back(param);
    target->owners.push_back(entry);
    // The span table runs parallel to the instructions (D118).
    target->spans.push_back(target->SpanOf(InstId{}));
    target->Get(entry).instructions.push_back(frame_pointer);
    target->Get(entry).param_count = 1;

    const InstId state_address = FieldAddress(target, entry, frame,
                                              frame_pointer,
                                              frame.state_field, state_type);
    const InstId state =
        Add(target, entry, Opcode::kLoad, state_type, {state_address});
    const InstId zero = Constant(target, entry, state_type, 0);
    const InstId first = Add(target, entry, Opcode::kEq,
                             types_->bool_type(), {state, zero});

    const BlockId suspend = AddBlock(target, "suspend");
    const BlockId done = AddBlock(target, "done");
    Branch(target, entry, first, suspend, done);

    const InstId one = Constant(target, suspend, state_type, 1);
    Add(target, suspend, Opcode::kStore, TypeId{}, {state_address, one});
    Return(target, suspend, Bool(target, suspend, false));

    Return(target, done, Bool(target, done, true));
    return position;
  }

  // The state machine. Every block of the body is copied; a `wait` ends the
  // block it is in and continues in the one control comes back to, which is
  // also the block the dispatch jumps to when the task is resumed.
  uint32_t EmitResume(uint32_t index, const Frame& frame) {
    // A copy, because the module's vector grows while this runs.
    const Function source = module_->functions[index];

    Function resume;
    resume.name = ResumeName(index);
    resume.type = types_->Function({frame.pointer}, types_->bool_type());
    resume.effects = source.effects;
    resume.effects_known = source.effects_known;
    const auto position = static_cast<uint32_t>(module_->functions.size());
    module_->functions.push_back(std::move(resume));
    Function* target = &module_->functions[position];

    const TypeId state_type = FieldType(frame, frame.state_field);
    const BlockId dispatch = AddBlock(target, "dispatch");

    Inst param;
    param.opcode = Opcode::kParam;
    param.type = frame.pointer;
    const InstId frame_pointer{
        static_cast<uint32_t>(target->instructions.size())};
    target->instructions.push_back(param);
    target->owners.push_back(dispatch);
    // The span table runs parallel to the instructions (D118).
    target->spans.push_back(target->SpanOf(InstId{}));
    target->Get(dispatch).instructions.push_back(frame_pointer);
    target->Get(dispatch).param_count = 1;

    const InstId state_address =
        FieldAddress(target, dispatch, frame, frame_pointer,
                     frame.state_field, state_type);
    const InstId state =
        Add(target, dispatch, Opcode::kLoad, state_type, {state_address});

    // Every value the body refers to that is not its own: the parameters,
    // read back out of the frame on each resume.
    std::vector<InstId> map(source.instructions.size(), InstId{});
    const std::span<const InstId> parameters =
        source.ParamsOf(source.entry());
    for (size_t i = 0; i < parameters.size(); ++i) {
      const uint32_t field =
          frame.first_param_field + static_cast<uint32_t>(i);
      const TypeId type = FieldType(frame, field);
      const InstId address = FieldAddress(target, dispatch, frame,
                                          frame_pointer, field, type);
      map[parameters[i].index] =
          Add(target, dispatch, Opcode::kLoad, type, {address});
    }

    // Every address into the frame is taken here, in the one block that
    // dominates every state: the dispatch can jump straight into the middle
    // of the body, so an address taken in an earlier block would not be
    // defined on that path. This is the thing a state machine gets wrong,
    // and the verifier is what says so.
    std::vector<InstId> slot_address(source.instructions.size(), InstId{});
    for (const auto& [slot, field] : frame.slots) {
      slot_address[slot.index] =
          FieldAddress(target, dispatch, frame, frame_pointer, field,
                       FieldType(frame, field));
    }
    std::vector<InstId> wait_address(source.instructions.size(), InstId{});
    for (const auto& [wait, field] : frame.waits) {
      wait_address[wait.index] =
          FieldAddress(target, dispatch, frame, frame_pointer, field,
                       FieldType(frame, field));
    }

    const LinearOrder order = Linearize(source);
    std::vector<BlockId> arrival(source.blocks.size(), BlockId{});
    for (BlockId block : order.blocks) {
      arrival[block.index] =
          AddBlock(target, "task." + source.Get(block).label);
    }

    // State zero is the beginning; state k is where the k-th `wait`
    // resumes.
    std::vector<BlockId> states;
    states.push_back(arrival[source.entry().index]);

    for (BlockId block : order.blocks) {
      BlockId current = arrival[block.index];
      const Block& body = source.Get(block);

      // A block parameter stays a block parameter: a value that depends on
      // which edge arrived is the same question here as it was there. The
      // entry's parameters are the exception -- they are the function's,
      // and they live in the frame now.
      const size_t first =
          block == source.entry() ? body.param_count : 0;
      for (size_t i = first; i < body.param_count; ++i) {
        const InstId id = body.instructions[i];
        Inst copy;
        copy.opcode = Opcode::kParam;
        copy.type = source.Get(id).type;
        const InstId fresh{
            static_cast<uint32_t>(target->instructions.size())};
        target->instructions.push_back(copy);
        target->owners.push_back(current);
        // The span table runs parallel to the instructions (D118).
        target->spans.push_back(target->SpanOf(InstId{}));
        Block& into = target->Get(current);
        into.instructions.insert(into.instructions.begin() +
                                     into.param_count,
                                 fresh);
        ++into.param_count;
        map[id.index] = fresh;
      }

      for (size_t i = body.param_count; i < body.instructions.size(); ++i) {
        const InstId id = body.instructions[i];
        const Inst& inst = source.Get(id);
        const auto operand = [&](size_t index) {
          const std::span<const InstId> operands = source.OperandsOf(inst);
          return index < operands.size() && operands[index].is_valid()
                     ? map[operands[index].index]
                     : InstId{};
        };

        if (inst.opcode == Opcode::kAlloca) {
          // A slot becomes a field of the frame, and its address is what
          // every load and store already took.
          map[id.index] = slot_address[id.index];
          continue;
        }

        if (inst.opcode == Opcode::kWaitCall) {
          current = EmitSuspension(target, current, wait_address[id.index],
                                   state_address, state_type, source, inst,
                                   id, &map, &states);
          continue;
        }

        if (inst.opcode == Opcode::kReturn) {
          if (frame.has_result && inst.operands_count != 0) {
            const TypeId type = FieldType(frame, frame.result_field);
            const InstId address =
                FieldAddress(target, current, frame, frame_pointer,
                             frame.result_field, type);
            Add(target, current, Opcode::kStore, TypeId{},
                {address, operand(0)});
          }
          Return(target, current, Bool(target, current, true));
          continue;
        }

        // Everything else is copied with its operands mapped.
        Inst copy = inst;
        copy.operands_begin =
            static_cast<uint32_t>(target->operands.size());
        copy.operands_count = inst.operands_count;
        for (size_t j = 0; j < inst.operands_count; ++j) {
          target->operands.push_back(operand(j));
        }
        copy.successors_begin =
            static_cast<uint32_t>(target->successors.size());
        copy.successors_count = inst.successors_count;
        for (BlockId successor : source.SuccessorsOf(inst)) {
          target->successors.push_back(
              successor.index < arrival.size() ? arrival[successor.index]
                                               : BlockId{});
        }
        copy.cases_begin = static_cast<uint32_t>(target->cases.size());
        copy.cases_count = inst.cases_count;
        for (uint64_t value : source.CasesOf(inst)) {
          target->cases.push_back(value);
        }
        const InstId fresh{
            static_cast<uint32_t>(target->instructions.size())};
        target->instructions.push_back(copy);
        target->owners.push_back(current);
        // The span table runs parallel to the instructions (D118).
        target->spans.push_back(target->SpanOf(InstId{}));
        target->Get(current).instructions.push_back(fresh);
        map[id.index] = fresh;
      }
    }

    // The dispatch, now that every state has a block to arrive at.
    Inst switch_inst;
    switch_inst.opcode = Opcode::kSwitch;
    switch_inst.operands_begin =
        static_cast<uint32_t>(target->operands.size());
    switch_inst.operands_count = 1;
    target->operands.push_back(state);
    switch_inst.successors_begin =
        static_cast<uint32_t>(target->successors.size());
    switch_inst.cases_begin = static_cast<uint32_t>(target->cases.size());
    for (size_t i = 0; i < states.size(); ++i) {
      target->successors.push_back(states[i]);
      target->cases.push_back(i);
    }
    // The default is unreachable: a state nobody wrote cannot be read.
    const BlockId nowhere = AddBlock(target, "task.nostate");
    Inst unreachable;
    unreachable.opcode = Opcode::kUnreachable;
    const InstId stop{static_cast<uint32_t>(target->instructions.size())};
    target->instructions.push_back(unreachable);
    target->owners.push_back(nowhere);
    // The span table runs parallel to the instructions (D118).
    target->spans.push_back(target->SpanOf(InstId{}));
    target->Get(nowhere).instructions.push_back(stop);

    target->successors.push_back(nowhere);
    switch_inst.successors_count =
        static_cast<uint32_t>(states.size() + 1);
    switch_inst.cases_count = static_cast<uint32_t>(states.size());
    const InstId dispatch_id{
        static_cast<uint32_t>(target->instructions.size())};
    target->instructions.push_back(switch_inst);
    target->owners.push_back(dispatch);
    // The span table runs parallel to the instructions (D118).
    target->spans.push_back(target->SpanOf(InstId{}));
    target->Get(dispatch).instructions.push_back(dispatch_id);

    return position;
  }

  // One `wait`: store the callee's arguments into its frame, poll it, and
  // record where to come back to when it is not finished. Returns the block
  // the rest of the body continues in.
  BlockId EmitSuspension(Function* target,
                         BlockId current,
                         InstId sub,
                         InstId state_address,
                         TypeId state_type,
                         const Function& source,
                         const Inst& inst,
                         InstId id,
                         std::vector<InstId>* map,
                         std::vector<BlockId>* states) {
    const Frame* callee = FrameOf(inst.payload.callee);
    if (callee == nullptr) {
      // Either a recursive task -- already reported -- or one defined in
      // another file, whose frame this file cannot lay out.
      ReportUnknownFrame(inst.payload.callee);
      // Leave the body unreachable rather than emitting a machine that
      // cannot work.
      Inst unreachable;
      unreachable.opcode = Opcode::kUnreachable;
      const InstId stop{static_cast<uint32_t>(target->instructions.size())};
      target->instructions.push_back(unreachable);
      target->owners.push_back(current);
      // The span table runs parallel to the instructions (D118).
      target->spans.push_back(target->SpanOf(InstId{}));
      target->Get(current).instructions.push_back(stop);
      return AddBlock(target, "task.unreachable");
    }

    // The callee starts at its beginning, with the arguments this call
    // passed. Written here rather than on resume: the dispatch jumps past
    // this block, which is what keeps the arguments from being written
    // again.
    const TypeId callee_state = FieldType(*callee, callee->state_field);
    const InstId callee_state_address =
        Add(target, current, Opcode::kFieldAddress,
            types_->Pointer(callee_state, true), {sub},
            callee->state_field);
    Add(target, current, Opcode::kStore, TypeId{},
        {callee_state_address, Constant(target, current, callee_state, 0)});

    const std::span<const InstId> arguments = source.OperandsOf(inst);
    for (size_t i = 0; i < arguments.size(); ++i) {
      const uint32_t argument_field =
          callee->first_param_field + static_cast<uint32_t>(i);
      const std::vector<TypeId> callee_fields =
          types_->ArgumentsOf(callee->type);
      if (argument_field >= callee_fields.size()) {
        break;
      }
      const TypeId type = callee_fields[argument_field];
      const InstId address =
          Add(target, current, Opcode::kFieldAddress,
              types_->Pointer(type, true), {sub}, argument_field);
      Add(target, current, Opcode::kStore, TypeId{},
          {address, (*map)[arguments[i].index]});
    }

    const BlockId poll = AddBlock(target, "task.poll");
    const BlockId suspend = AddBlock(target, "task.suspend");
    const BlockId resumed = AddBlock(target, "task.resumed");
    Jump(target, current, poll);

    const InstId done = Add(target, poll, Opcode::kCall,
                            types_->bool_type(), {sub});
    target->Get(done).payload.callee = callee->resume;
    Branch(target, poll, done, resumed, suspend);

    // Suspending is recording where to come back to and returning.
    const auto state = static_cast<uint64_t>(states->size());
    states->push_back(poll);
    Add(target, suspend, Opcode::kStore, TypeId{},
        {state_address, Constant(target, suspend, state_type, state)});
    Return(target, suspend, Bool(target, suspend, false));

    if (callee->has_result) {
      const TypeId type = FieldType(*callee, callee->result_field);
      const InstId address =
          Add(target, resumed, Opcode::kFieldAddress,
              types_->Pointer(type, true), {sub}, callee->result_field);
      (*map)[id.index] =
          Add(target, resumed, Opcode::kLoad, type, {address});
    }
    return resumed;
  }

  // What is left of the original function: a driver that runs the machine
  // to completion. A direct call to a task still works, which is what makes
  // a task testable before there is a scheduler to run it properly.
  void EmitDriver(uint32_t index, const Frame& frame, uint32_t resume) {
    const uint32_t allocate = Allocator(frame, /*release=*/false);
    const uint32_t release = Allocator(frame, /*release=*/true);

    Function driver;
    driver.name = module_->functions[index].name;
    driver.type = module_->functions[index].type;
    driver.effects = module_->functions[index].effects;
    driver.effects_known = module_->functions[index].effects_known;
    driver.source_file = module_->functions[index].source_file;
    driver.source_item = module_->functions[index].source_item;
    module_->functions[index] = std::move(driver);
    Function* target = &module_->functions[index];

    const BlockId entry = AddBlock(target, "entry");
    const std::vector<TypeId> parameters =
        types_->ArgumentsOf(target->type);
    std::vector<InstId> arguments;
    for (TypeId type : parameters) {
      Inst param;
      param.opcode = Opcode::kParam;
      param.type = type;
      const InstId id{static_cast<uint32_t>(target->instructions.size())};
      target->instructions.push_back(param);
      target->owners.push_back(entry);
      // The span table runs parallel to the instructions (D118).
      target->spans.push_back(target->SpanOf(InstId{}));
      target->Get(entry).instructions.push_back(id);
      ++target->Get(entry).param_count;
      arguments.push_back(id);
    }

    const TypeId size_type = types_->Int(IntWidth::k64, false);
    const InstId size = Constant(target, entry, size_type, frame.size);
    const InstId pointer =
        Add(target, entry, Opcode::kCall, frame.pointer, {size});
    target->Get(pointer).payload.callee = allocate;

    const TypeId state_type = FieldType(frame, frame.state_field);
    const InstId state_address =
        FieldAddress(target, entry, frame, pointer, frame.state_field,
                     state_type);
    Add(target, entry, Opcode::kStore, TypeId{},
        {state_address, Constant(target, entry, state_type, 0)});

    for (size_t i = 0; i < arguments.size(); ++i) {
      const uint32_t field =
          frame.first_param_field + static_cast<uint32_t>(i);
      const TypeId type = FieldType(frame, field);
      const InstId address = FieldAddress(target, entry, frame, pointer,
                                          field, type);
      Add(target, entry, Opcode::kStore, TypeId{}, {address, arguments[i]});
    }

    // Spin the machine. Without a scheduler this is what "waiting" is: the
    // only task that can make progress is this one.
    const BlockId step = AddBlock(target, "step");
    const BlockId finish = AddBlock(target, "finish");
    Jump(target, entry, step);

    const InstId done =
        Add(target, step, Opcode::kCall, types_->bool_type(), {pointer});
    target->Get(done).payload.callee = resume;
    Branch(target, step, done, finish, step);

    InstId result;
    if (frame.has_result) {
      const TypeId type = FieldType(frame, frame.result_field);
      const InstId address = FieldAddress(target, finish, frame, pointer,
                                          frame.result_field, type);
      result = Add(target, finish, Opcode::kLoad, type, {address});
    }
    const InstId freed =
        Add(target, finish, Opcode::kCall, types_->void_type(), {pointer});
    target->Get(freed).payload.callee = release;
    Return(target, finish, result);
  }

  Symbol ResumeName(uint32_t index) {
    std::string name(interner_->Text(module_->functions[index].name));
    name.append("$resume");
    return interner_->Intern(name);
  }

  // A foreign declaration for the frame allocator, typed for this frame.
  uint32_t Allocator(const Frame& frame, bool release) {
    std::string name(release ? kFreeSymbol : kAllocSymbol);
    name.push_back('$');
    name.append(std::to_string(frame.type.index));
    const Symbol symbol = interner_->Intern(name);
    for (uint32_t i = 0; i < module_->functions.size(); ++i) {
      if (module_->functions[i].name == symbol) {
        return i;
      }
    }
    Function declaration;
    declaration.name = symbol;
    declaration.type =
        release
            ? types_->Function({frame.pointer}, types_->void_type())
            : types_->Function({types_->Int(IntWidth::k64, false)},
                               frame.pointer);
    declaration.foreign_symbol =
        interner_->Intern(std::string(release ? kFreeSymbol : kAllocSymbol));
    declaration.effects_known = true;
    module_->functions.push_back(std::move(declaration));
    return static_cast<uint32_t>(module_->functions.size() - 1);
  }

  StringInterner* interner_;
  TypeStore* types_;
  abi::LayoutStore* layouts_;
  Module* module_;
  const std::function<SourceSpan(uint32_t)>& span_of_;
  std::vector<Diagnostic>* diagnostics_;
  std::unordered_map<uint32_t, Frame> frames_;
  // Every task, in the order the dispatcher numbers them.
  std::vector<std::pair<uint32_t, uint32_t>> kinds_;
  std::set<uint32_t> reported_;
};

}  // namespace

void SplitTasks(StringInterner* interner,
                TypeStore* types,
                abi::LayoutStore* layouts,
                Module* module,
                const std::function<SourceSpan(uint32_t)>& span_of,
                std::vector<Diagnostic>* diagnostics) {
  bool any = false;
  for (const Function& function : module->functions) {
    any = any || function.is_task;
  }
  if (!any) {
    return;
  }
  Splitter splitter(interner, types, layouts, module, span_of, diagnostics);
  splitter.Run();
}

}  // namespace ir
}  // namespace wolang
