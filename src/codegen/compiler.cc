// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/codegen/compiler.h"

#include <setjmp.h>

#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <utility>
#include <vector>

#include "wolang/base/check.h"
#include "wolang/codegen/code_block.h"
#include "wolang/codegen/layout.h"
#include "wolang/codegen/runtime.h"

namespace wolang {
namespace codegen {

bool IsSupportedTarget() {
#if defined(__aarch64__) || defined(_M_ARM64)
  return true;
#else
  return false;
#endif
}

void ReportTrap(Context* context,
                uint32_t kind,
                uint32_t site,
                uint64_t frame) {
  context->trap_kind = kind;
  context->trap_site = site;
  context->frame_count = 0;

  // Walk the frame records. Every compiled function has one, so each step
  // is two loads: the caller's frame pointer and its return address. The
  // walk stops at the first address that is not in the compiled block,
  // which is the trampoline the host called through.
  uint64_t current = frame;
  while (current != 0 && context->frame_count < Context::kMaxFrames) {
    uint64_t next = 0;
    uint64_t link = 0;
    std::memcpy(&next, reinterpret_cast<const void*>(current),
                sizeof(next));
    std::memcpy(&link,
                reinterpret_cast<const void*>(current + sizeof(uint64_t)),
                sizeof(link));
    if (link < context->code_begin || link >= context->code_end) {
      break;
    }
    context->frames[context->frame_count++] = link;
    // A frame pointer that does not move forward is a corrupt chain, and
    // following it would loop.
    if (next <= current) {
      break;
    }
    current = next;
  }

  // The non-local exit a trap is. Nothing between here and the landing pad
  // holds an object that needs unwinding -- a generated frame holds machine
  // values and nothing else -- which is what makes a trap free on the path
  // that does not take one.
  _longjmp(*static_cast<jmp_buf*>(context->landing), 1);
}

class CompiledModule::Impl {
 public:
  Impl(const StringInterner& interner,
       const TypeStore& types,
       const FileResolution& resolution,
       const ir::Module& module)
      : interner_(interner),
        types_(types),
        module_(module),
        layouts_(types, resolution) {}

  void Compile();

  interp::Outcome Run(Symbol name,
                      const std::vector<interp::Value>& arguments,
                      std::vector<std::string>* backtrace);

  // Which function an address belongs to, as a name and an offset. What
  // makes a trap from compiled code readable, and what a profiler needs to
  // label a sample.
  std::string Symbolize(uint64_t offset) const;

  struct Entry {
    bool compiled = false;
    std::string reason;
    // Where the entry trampoline is, in bytes from the start of the block.
    uint32_t stub = 0;
    // Where the body is, in words, which is what a call has to reach.
    uint32_t body_word = 0;
    // The function's whole code, for a dump.
    uint32_t first_word = 0;
    uint32_t word_count = 0;
  };

  const Entry* Find(Symbol name) const {
    const auto found = by_name_.find(name.index);
    return found == by_name_.end() ? nullptr : &entries_[found->second];
  }

  const StringInterner& interner_;
  const TypeStore& types_;
  const ir::Module& module_;
  LayoutStore layouts_;

  std::vector<uint32_t> words_;
  std::vector<Entry> entries_;
  std::vector<TrapSite> traps_;
  std::unordered_map<uint32_t, uint32_t> by_name_;
  CodeBlock block_;
  uint32_t compiled_count_ = 0;
  uint32_t skipped_count_ = 0;

 private:
  // A value, as the bytes generated code expects. An interpreter value
  // carries its own shape and machine memory does not, so this boundary is
  // where one becomes the other -- and the only place either tier has to
  // know about the other's representation.
  void Encode(const interp::Value& value, TypeId type, uint8_t* destination);
  interp::Value Decode(const uint8_t* source, TypeId type);
  // The canonical register form of an integer: extended to 64 bits, which
  // is how compiled code holds one.
  uint64_t CanonicalBits(const interp::Value& value, TypeId type);
};

// ---------------------------------------------------------- compiling ---

void CompiledModule::Impl::Compile() {
  const size_t count = module_.functions.size();
  entries_.assign(count, Entry{});
  for (size_t i = 0; i < count; ++i) {
    by_name_[module_.functions[i].name.index] = static_cast<uint32_t>(i);
  }

  // Ask the emitter once per function what it thinks, with every callee
  // assumed fine. Emitting is the only honest answer to "can this be
  // compiled", so there is no second opinion to keep in step.
  std::vector<bool> compilable(count, true);
  std::vector<std::string> reasons(count);
  for (size_t i = 0; i < count; ++i) {
    const CompiledFunction attempt =
        EmitFunction(interner_, types_, &layouts_, module_,
                     module_.functions[i], compilable, 0);
    if (!attempt.compiled) {
      compilable[i] = false;
      reasons[i] = attempt.reason;
    }
  }

  // A call into something this tier cannot take makes the caller
  // untakeable too: there is no way back into the interpreter mid-frame.
  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t i = 0; i < count; ++i) {
      if (!compilable[i]) {
        continue;
      }
      for (const ir::Inst& inst : module_.functions[i].instructions) {
        if (inst.opcode != ir::Opcode::kCall) {
          continue;
        }
        if (inst.payload.callee >= count ||
            !compilable[inst.payload.callee]) {
          compilable[i] = false;
          reasons[i] = "calls something this tier cannot compile";
          changed = true;
          break;
        }
      }
    }
  }

  std::vector<CallFixup> fixups;
  for (size_t i = 0; i < count; ++i) {
    Entry& entry = entries_[i];
    if (!compilable[i]) {
      entry.reason = reasons[i];
      ++skipped_count_;
      continue;
    }

    const uint32_t base = static_cast<uint32_t>(words_.size());
    CompiledFunction compiled =
        EmitFunction(interner_, types_, &layouts_, module_,
                     module_.functions[i], compilable,
                     static_cast<uint32_t>(traps_.size()));
    WOLANG_CHECK(compiled.compiled);

    entry.compiled = true;
    entry.first_word = base;
    entry.word_count = static_cast<uint32_t>(compiled.words.size());
    entry.stub = (base + compiled.stub / sizeof(uint32_t)) *
                 static_cast<uint32_t>(sizeof(uint32_t));
    entry.body_word = base + compiled.body / sizeof(uint32_t);
    words_.insert(words_.end(), compiled.words.begin(),
                  compiled.words.end());
    for (TrapSite& site : compiled.traps) {
      traps_.push_back(std::move(site));
    }
    for (const CallFixup& fixup : compiled.calls) {
      fixups.push_back(CallFixup{base + fixup.word, fixup.callee});
    }
    ++compiled_count_;
  }

  // Only once every function has a place can the calls between them be
  // filled in. One module, one block of code: a `bl` reaches 128 megabytes
  // and that is more code than this tier will ever emit at once.
  for (const CallFixup& fixup : fixups) {
    WOLANG_CHECK(fixup.callee < entries_.size());
    const Entry& callee = entries_[fixup.callee];
    WOLANG_CHECK(callee.compiled);
    const int32_t distance = static_cast<int32_t>(callee.body_word) -
                             static_cast<int32_t>(fixup.word);
    WOLANG_CHECK(distance >= -(1 << 25) && distance < (1 << 25));
    words_[fixup.word] |= static_cast<uint32_t>(distance) & 0x03FFFFFFu;
  }

  if (!words_.empty()) {
    block_.Install(words_);
  }
}

// -------------------------------------------------------- marshalling ---

uint64_t CompiledModule::Impl::CanonicalBits(const interp::Value& value,
                                             TypeId type) {
  const uint32_t bits = layouts_.BitsOf(type);
  if (value.kind() == interp::Value::Kind::kBool) {
    return value.boolean() ? 1 : 0;
  }
  if (bits >= 64 || bits == 0) {
    return value.integer();
  }
  const uint64_t mask = (uint64_t{1} << bits) - 1;
  uint64_t raw = value.integer() & mask;
  if (layouts_.IsSignedInteger(type) &&
      (raw & (uint64_t{1} << (bits - 1))) != 0) {
    raw |= ~mask;
  }
  return raw;
}

void CompiledModule::Impl::Encode(const interp::Value& value,
                                  TypeId type,
                                  uint8_t* destination) {
  switch (layouts_.ClassOf(type)) {
    case ValueClass::kVoid:
    case ValueClass::kUnsupported:
      return;

    case ValueClass::kInteger: {
      const uint64_t raw = CanonicalBits(value, type);
      const uint32_t size = layouts_.SizeOf(type);
      std::memcpy(destination, &raw, size);
      return;
    }

    case ValueClass::kFloat: {
      if (layouts_.BitsOf(type) == 64) {
        const double number = value.floating();
        std::memcpy(destination, &number, sizeof(number));
      } else {
        const float number = static_cast<float>(value.floating());
        std::memcpy(destination, &number, sizeof(number));
      }
      return;
    }

    case ValueClass::kAggregate:
      break;
  }

  const Layout& layout = layouts_.Of(type);
  if (types_.KindOf(type) == TypeKind::kOptional) {
    const uint8_t tag = value.kind() == interp::Value::Kind::kOptional &&
                                value.has_value()
                            ? 1
                            : 0;
    destination[0] = tag;
    if (tag != 0 && layout.offsets.size() > 1) {
      Encode(value.inside(), types_.ElementOf(type),
             destination + layout.offsets[1]);
    }
    return;
  }

  if (value.kind() != interp::Value::Kind::kAggregate) {
    return;
  }
  const std::vector<interp::Value>& elements = value.elements();
  if (types_.KindOf(type) == TypeKind::kArray) {
    const TypeId element = types_.ElementOf(type);
    const uint32_t stride = layouts_.StrideOf(type);
    for (size_t i = 0; i < elements.size(); ++i) {
      Encode(elements[i], element,
             destination + i * stride);
    }
    return;
  }
  if (layouts_.IsTagOnly(type)) {
    if (!elements.empty()) {
      const uint32_t tag = static_cast<uint32_t>(elements[0].integer());
      std::memcpy(destination, &tag, sizeof(tag));
    }
    return;
  }
  for (size_t i = 0; i < elements.size() && i < layout.offsets.size(); ++i) {
    const TypeId field = layouts_.FieldType(type, static_cast<uint32_t>(i));
    if (field.is_valid()) {
      Encode(elements[i], field, destination + layout.offsets[i]);
    }
  }
}

interp::Value CompiledModule::Impl::Decode(const uint8_t* source,
                                           TypeId type) {
  switch (layouts_.ClassOf(type)) {
    case ValueClass::kVoid:
      return interp::Value::Void();
    case ValueClass::kUnsupported:
      return interp::Value();

    case ValueClass::kInteger: {
      uint64_t raw = 0;
      std::memcpy(&raw, source, layouts_.SizeOf(type));
      if (types_.KindOf(type) == TypeKind::kBool) {
        return interp::Value::Bool((raw & 1) != 0);
      }
      const Type& described = types_.Get(type);
      return interp::Value::Int(raw, described.int_width,
                                described.is_signed);
    }

    case ValueClass::kFloat: {
      if (layouts_.BitsOf(type) == 64) {
        double number = 0;
        std::memcpy(&number, source, sizeof(number));
        return interp::Value::Float(number, FloatWidth::k64);
      }
      float number = 0;
      std::memcpy(&number, source, sizeof(number));
      return interp::Value::Float(static_cast<double>(number),
                                  FloatWidth::k32);
    }

    case ValueClass::kAggregate:
      break;
  }

  const Layout& layout = layouts_.Of(type);
  if (types_.KindOf(type) == TypeKind::kOptional) {
    if (source[0] == 0) {
      return interp::Value::Absent();
    }
    const TypeId inside = types_.ElementOf(type);
    const uint32_t offset =
        layout.offsets.size() > 1 ? layout.offsets[1] : 0;
    return interp::Value::Present(Decode(source + offset, inside));
  }
  if (types_.KindOf(type) == TypeKind::kArray) {
    const TypeId element = types_.ElementOf(type);
    const uint32_t stride = layouts_.StrideOf(type);
    std::vector<interp::Value> elements;
    for (uint64_t i = 0; i < types_.Get(type).length; ++i) {
      elements.push_back(Decode(source + i * stride, element));
    }
    return interp::Value::Aggregate(std::move(elements));
  }
  if (layouts_.IsTagOnly(type)) {
    uint32_t tag = 0;
    std::memcpy(&tag, source, sizeof(tag));
    std::vector<interp::Value> elements;
    elements.push_back(interp::Value::Int(tag, IntWidth::k32, true));
    return interp::Value::Aggregate(std::move(elements));
  }
  std::vector<interp::Value> elements;
  for (size_t i = 0; i < layout.offsets.size(); ++i) {
    const TypeId field = layouts_.FieldType(type, static_cast<uint32_t>(i));
    if (!field.is_valid()) {
      continue;
    }
    elements.push_back(Decode(source + layout.offsets[i], field));
  }
  return interp::Value::Aggregate(std::move(elements));
}

// ------------------------------------------------------------ running ---

interp::Outcome CompiledModule::Impl::Run(
    Symbol name,
    const std::vector<interp::Value>& arguments,
    std::vector<std::string>* backtrace) {
  interp::Outcome outcome;
  const Entry* entry = Find(name);
  if (entry == nullptr || !entry->compiled || !block_.is_installed()) {
    outcome.trap = ir::TrapKind::kNoBody;
    outcome.where = "@" + std::string(interner_.Text(name));
    outcome.message = "was not compiled";
    return outcome;
  }

  const ir::Function& function =
      module_.functions[by_name_[name.index]];
  const std::span<const ir::InstId> params =
      function.ParamsOf(function.entry());

  // One slot per parameter: the bits of a scalar, or the address of an
  // aggregate the caller laid out.
  std::vector<uint64_t> slots(std::max<size_t>(params.size(), 1), 0);
  std::vector<std::vector<uint8_t>> buffers(params.size());
  for (size_t i = 0; i < params.size(); ++i) {
    const TypeId type = function.Get(params[i]).type;
    if (layouts_.ClassOf(type) == ValueClass::kAggregate) {
      buffers[i].assign(std::max(layouts_.SizeOf(type), 8u), 0);
      if (i < arguments.size()) {
        Encode(arguments[i], type, buffers[i].data());
      }
      uint8_t* address = buffers[i].data();
      std::memcpy(&slots[i], &address, sizeof(address));
      continue;
    }
    if (i < arguments.size()) {
      Encode(arguments[i], type,
             reinterpret_cast<uint8_t*>(&slots[i]));
    }
  }

  const TypeId returned = types_.ElementOf(function.type);
  std::vector<uint8_t> result(
      std::max(returned.is_valid() ? layouts_.SizeOf(returned) : 0u, 16u), 0);

  Context context;
  jmp_buf landing;
  context.landing = &landing;
  context.report = &ReportTrap;
  context.code_begin =
      reinterpret_cast<uint64_t>(block_.AddressOf(0));
  context.code_end = context.code_begin + words_.size() * sizeof(uint32_t);

  const auto stub = reinterpret_cast<EntryPoint>(
      const_cast<void*>(block_.AddressOf(entry->stub)));

  if (_setjmp(landing) == 0) {
    stub(&context, slots.data(), result.data());
    if (returned.is_valid()) {
      outcome.value = Decode(result.data(), returned);
    } else {
      outcome.value = interp::Value::Void();
    }
    return outcome;
  }

  outcome.trap = static_cast<ir::TrapKind>(context.trap_kind);
  if (context.trap_site < traps_.size()) {
    outcome.where = traps_[context.trap_site].where;
    outcome.message = traps_[context.trap_site].message;
  }
  if (backtrace != nullptr) {
    for (uint32_t i = 0; i < context.frame_count; ++i) {
      const std::string frame =
          Symbolize(context.frames[i] - context.code_begin);
      // An empty name is the trampoline, which is where the host entered
      // compiled code: there is nothing above it worth naming.
      if (frame.empty()) {
        break;
      }
      backtrace->push_back(frame);
    }
  }
  return outcome;
}

std::string CompiledModule::Impl::Symbolize(uint64_t offset) const {
  const uint32_t word = static_cast<uint32_t>(offset / sizeof(uint32_t));
  for (size_t i = 0; i < entries_.size(); ++i) {
    const Entry& entry = entries_[i];
    if (!entry.compiled || word < entry.first_word ||
        word >= entry.first_word + entry.word_count) {
      continue;
    }
    if (word < entry.body_word) {
      // The entry trampoline, which belongs to the host side of the call
      // rather than to the function.
      return {};
    }
    std::string text = "@";
    text.append(interner_.Text(module_.functions[i].name));
    text.append("+0x");
    char buffer[16] = {};
    std::snprintf(buffer, sizeof(buffer), "%x",
                  static_cast<unsigned>(offset -
                                        entry.first_word *
                                            sizeof(uint32_t)));
    text.append(buffer);
    return text;
  }
  return "<outside the compiled code>";
}

// ------------------------------------------------------- the interface ---

CompiledModule::CompiledModule() = default;
CompiledModule::~CompiledModule() = default;

bool CompiledModule::Contains(Symbol name) const {
  const Impl::Entry* entry = impl_->Find(name);
  return entry != nullptr && entry->compiled;
}

std::string_view CompiledModule::SkipReason(Symbol name) const {
  const Impl::Entry* entry = impl_->Find(name);
  return entry == nullptr ? std::string_view("is not in the module")
                          : std::string_view(entry->reason);
}

interp::Outcome CompiledModule::Run(
    Symbol name,
    const std::vector<interp::Value>& arguments,
    std::vector<std::string>* backtrace) const {
  return impl_->Run(name, arguments, backtrace);
}

std::string CompiledModule::Symbolize(uint32_t offset) const {
  return impl_->Symbolize(offset);
}

std::vector<std::pair<std::string, uint32_t>> CompiledModule::Listing()
    const {
  std::vector<std::pair<std::string, uint32_t>> listing;
  for (size_t i = 0; i < impl_->entries_.size(); ++i) {
    const Impl::Entry& entry = impl_->entries_[i];
    if (!entry.compiled) {
      continue;
    }
    listing.push_back(std::pair<std::string, uint32_t>(
        std::string(impl_->interner_.Text(impl_->module_.functions[i].name)),
        entry.word_count * static_cast<uint32_t>(sizeof(uint32_t))));
  }
  return listing;
}

std::span<const uint32_t> CompiledModule::WordsOf(Symbol name) const {
  const Impl::Entry* entry = impl_->Find(name);
  if (entry == nullptr || !entry->compiled) {
    return {};
  }
  return std::span<const uint32_t>(impl_->words_.data() + entry->first_word,
                                   entry->word_count);
}

size_t CompiledModule::size() const {
  return impl_->words_.size() * sizeof(uint32_t);
}
uint32_t CompiledModule::compiled_count() const {
  return impl_->compiled_count_;
}
uint32_t CompiledModule::skipped_count() const {
  return impl_->skipped_count_;
}

std::unique_ptr<CompiledModule> Compile(const StringInterner& interner,
                                        const TypeStore& types,
                                        const FileResolution& resolution,
                                        const ir::Module& module) {
  auto compiled = std::unique_ptr<CompiledModule>(new CompiledModule());
  compiled->impl_ =
      std::make_unique<CompiledModule::Impl>(interner, types, resolution,
                                             module);
  compiled->impl_->Compile();
  return compiled;
}

}  // namespace codegen
}  // namespace wolang
