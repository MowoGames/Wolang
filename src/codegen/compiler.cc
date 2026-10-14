// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/codegen/compiler.h"

#include <dlfcn.h>
#include <setjmp.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <memory>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "wolang/base/check.h"
#include "wolang/codegen/code_block.h"
#include "wolang/abi/layout.h"
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
       std::span<const FileCode> files)
      : interner_(interner), types_(types) {
    for (const FileCode& file : files) {
      if (file.resolution == nullptr || file.module == nullptr) {
        continue;
      }
      files_.push_back(std::make_unique<FileState>(types, *file.resolution,
                                                   *file.module));
    }
  }

  void Compile();

  interp::Outcome Run(Symbol name,
                      const std::vector<interp::Value>& arguments,
                      std::vector<std::string>* backtrace);

  // Which function an address belongs to, as a name and an offset. What
  // makes a trap from compiled code readable, and what a profiler needs to
  // label a sample.
  std::string Symbolize(uint64_t offset) const;

  // One file's IR and everything derived from it. A layout store belongs to
  // a file because a struct's fields do: they are part of the resolution
  // that declared them.
  struct FileState {
    FileState(const TypeStore& types,
              const FileResolution& resolution,
              const ir::Module& module)
        : module(module), layouts(types, resolution) {}

    const ir::Module& module;
    abi::LayoutStore layouts;
    // Where each string literal's bytes are.
    std::vector<std::pair<uint64_t, uint64_t>> strings;
    // This file's local function index to the global one.
    std::vector<uint32_t> global;
    // Per local function index, for the emitter.
    std::vector<bool> compilable;
    std::vector<uint64_t> foreign;
  };

  struct Entry {
    uint32_t file = 0;
    uint32_t local = 0;
    // The global function this one resolves to: itself for a definition,
    // and the definition for a declaration that names one.
    uint32_t link = 0;
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
    if (found == by_name_.end()) {
      return nullptr;
    }
    const Entry& entry = entries_[found->second];
    return entry.compiled ? &entry : &entries_[entry.link];
  }

  const ir::Function& FunctionOf(const Entry& entry) const {
    return files_[entry.file]->module.functions[entry.local];
  }

  const StringInterner& interner_;
  const TypeStore& types_;
  std::vector<std::unique_ptr<FileState>> files_;

  std::vector<uint32_t> words_;
  std::vector<Entry> entries_;
  std::vector<TrapSite> traps_;
  // By name, for the driver and the tests. A definition wins over a
  // declaration of the same name.
  std::unordered_map<uint32_t, uint32_t> by_name_;
  CodeBlock block_;
  // See Run: both have to outlive a `longjmp` through generated frames.
  Context context_;
  jmp_buf landing_ = {};
  uint32_t compiled_count_ = 0;
  uint32_t skipped_count_ = 0;

 private:
  // A value, as the bytes generated code expects. An interpreter value
  // carries its own shape and machine memory does not, so this boundary is
  // where one becomes the other -- and the only place either tier has to
  // know about the other's representation.
  void Encode(abi::LayoutStore* layouts,
              const interp::Value& value,
              TypeId type,
              uint8_t* destination);
  interp::Value Decode(abi::LayoutStore* layouts,
                       const uint8_t* source,
                       TypeId type);
  // The canonical register form of an integer: extended to 64 bits, which
  // is how compiled code holds one.
  uint64_t CanonicalBits(abi::LayoutStore* layouts,
                         const interp::Value& value,
                         TypeId type);
};

// ---------------------------------------------------------- compiling ---

void CompiledModule::Impl::Compile() {
  // Every function of every file, in one list. A call that crosses a file
  // is a declaration here and a definition there, and linking the two is
  // what makes the tier compile a program rather than a file.
  for (uint32_t f = 0; f < files_.size(); ++f) {
    FileState& file = *files_[f];
    for (const std::string& text : file.module.strings) {
      file.strings.emplace_back(reinterpret_cast<uint64_t>(text.data()),
                                static_cast<uint64_t>(text.size()));
    }
    for (uint32_t i = 0; i < file.module.functions.size(); ++i) {
      const auto global = static_cast<uint32_t>(entries_.size());
      file.global.push_back(global);
      Entry entry;
      entry.file = f;
      entry.local = i;
      entry.link = global;
      entries_.push_back(std::move(entry));
    }
  }

  // A declaration resolves to the definition it names. By the declaration
  // it came from, not by name: two files may each define a `Helper`, and
  // only the item it points at says which one this is.
  for (uint32_t i = 0; i < entries_.size(); ++i) {
    const ir::Function& function = FunctionOf(entries_[i]);
    if (!function.is_declaration() || function.is_foreign() ||
        !function.has_source()) {
      continue;
    }
    for (uint32_t j = 0; j < entries_.size(); ++j) {
      if (i == j) {
        continue;
      }
      const ir::Function& candidate = FunctionOf(entries_[j]);
      if (candidate.is_declaration() || !candidate.has_source()) {
        continue;
      }
      if (candidate.source_file == function.source_file &&
          candidate.source_item == function.source_item) {
        entries_[i].link = j;
        break;
      }
    }
  }

  // By name, with a definition winning: `wolang asm` and the tests look
  // functions up this way.
  for (uint32_t i = 0; i < entries_.size(); ++i) {
    const ir::Function& function = FunctionOf(entries_[i]);
    const auto existing = by_name_.find(function.name.index);
    if (existing == by_name_.end()) {
      by_name_[function.name.index] = i;
      continue;
    }
    if (FunctionOf(entries_[existing->second]).is_declaration() &&
        !function.is_declaration()) {
      existing->second = i;
    }
  }

  // Foreign symbols, resolved through the host's loader. A symbol that is
  // not there leaves a zero, and a function calling it is left to the
  // interpreter with a reason -- which is better than a module that refuses
  // to compile because one declaration named something missing.
  std::vector<uint64_t> foreign(entries_.size(), 0);
  std::vector<bool> compilable(entries_.size(), true);
  std::vector<std::string> reasons(entries_.size());
  for (uint32_t i = 0; i < entries_.size(); ++i) {
    const ir::Function& function = FunctionOf(entries_[i]);
    if (!function.is_foreign()) {
      continue;
    }
    const std::string symbol(interner_.Text(function.foreign_symbol));
    foreign[i] = reinterpret_cast<uint64_t>(
        ::dlsym(RTLD_DEFAULT, symbol.c_str()));
    reasons[i] = foreign[i] == 0
                     ? "is `extern` and its symbol was not found"
                     : "is `extern`, called at the address the loader gave";
  }

  // What is callable: a definition the emitter accepts, a foreign symbol
  // that resolved, or a declaration whose definition is callable.
  const auto callable = [&](uint32_t global) {
    const Entry& entry = entries_[global];
    if (foreign[global] != 0) {
      return true;
    }
    const uint32_t target = entry.link;
    return compilable[target] &&
           !FunctionOf(entries_[target]).is_declaration();
  };

  const auto refresh = [&] {
    for (uint32_t f = 0; f < files_.size(); ++f) {
      FileState& file = *files_[f];
      file.compilable.assign(file.global.size(), false);
      file.foreign.assign(file.global.size(), 0);
      for (uint32_t i = 0; i < file.global.size(); ++i) {
        const uint32_t global = file.global[i];
        file.compilable[i] = callable(global);
        // A call resolved through a declaration still goes through a `bl`,
        // so only a genuinely foreign target gets an address.
        file.foreign[i] = foreign[entries_[global].link] != 0
                              ? foreign[entries_[global].link]
                              : foreign[global];
      }
    }
  };

  // Ask the emitter once per definition, with every callee assumed fine.
  refresh();
  for (uint32_t i = 0; i < entries_.size(); ++i) {
    const Entry& entry = entries_[i];
    const ir::Function& function = FunctionOf(entry);
    if (function.is_declaration()) {
      if (reasons[i].empty()) {
        reasons[i] = entries_[i].link != i
                         ? "is defined in another file"
                         : "has no body";
      }
      compilable[i] = false;
      continue;
    }
    FileState& file = *files_[entry.file];
    const CompiledFunction attempt =
        EmitFunction(interner_, types_, &file.layouts, file.module, function,
                     file.compilable, file.foreign, file.strings, 0);
    if (!attempt.compiled) {
      compilable[i] = false;
      reasons[i] = attempt.reason;
    }
  }

  // A call into something this tier cannot take makes the caller untakeable
  // too: there is no way back into the interpreter mid-frame.
  bool changed = true;
  while (changed) {
    changed = false;
    refresh();
    for (uint32_t i = 0; i < entries_.size(); ++i) {
      if (!compilable[i] || FunctionOf(entries_[i]).is_declaration()) {
        continue;
      }
      const FileState& file = *files_[entries_[i].file];
      for (const ir::Inst& inst : FunctionOf(entries_[i]).instructions) {
        if (inst.opcode != ir::Opcode::kCall ||
            inst.payload.callee >= file.global.size()) {
          continue;
        }
        if (!file.compilable[inst.payload.callee]) {
          compilable[i] = false;
          reasons[i] = "calls something this tier cannot compile";
          changed = true;
          break;
        }
      }
    }
  }
  refresh();

  std::vector<std::pair<uint32_t, CallFixup>> fixups;
  for (uint32_t i = 0; i < entries_.size(); ++i) {
    Entry& entry = entries_[i];
    const ir::Function& function = FunctionOf(entry);
    if (!compilable[i] || function.is_declaration()) {
      entry.reason = reasons[i];
      ++skipped_count_;
      continue;
    }

    FileState& file = *files_[entry.file];
    const auto base = static_cast<uint32_t>(words_.size());
    CompiledFunction compiled =
        EmitFunction(interner_, types_, &file.layouts, file.module, function,
                     file.compilable, file.foreign, file.strings,
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
      fixups.emplace_back(entry.file,
                          CallFixup{base + fixup.word, fixup.callee});
    }
    ++compiled_count_;
  }

  // Only once every function has a place can the calls between them be
  // filled in. One module, one block of code: a `bl` reaches 128 megabytes
  // and that is more code than this tier will ever emit at once.
  for (const auto& [file_index, fixup] : fixups) {
    const FileState& file = *files_[file_index];
    WOLANG_CHECK(fixup.callee < file.global.size());
    const Entry& callee = entries_[entries_[file.global[fixup.callee]].link];
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

uint64_t CompiledModule::Impl::CanonicalBits(abi::LayoutStore* layouts,
                                             const interp::Value& value,
                                             TypeId type) {
  const uint32_t bits = layouts->BitsOf(type);
  if (value.kind() == interp::Value::Kind::kBool) {
    return value.boolean() ? 1 : 0;
  }
  if (bits >= 64 || bits == 0) {
    return value.integer();
  }
  const uint64_t mask = (uint64_t{1} << bits) - 1;
  uint64_t raw = value.integer() & mask;
  if (layouts->IsSignedInteger(type) &&
      (raw & (uint64_t{1} << (bits - 1))) != 0) {
    raw |= ~mask;
  }
  return raw;
}

void CompiledModule::Impl::Encode(abi::LayoutStore* layouts,
                                  const interp::Value& value,
                                  TypeId type,
                                  uint8_t* destination) {
  switch (layouts->ClassOf(type)) {
    case abi::ValueClass::kVoid:
    case abi::ValueClass::kUnsupported:
      return;

    case abi::ValueClass::kInteger: {
      const uint64_t raw = CanonicalBits(layouts, value, type);
      const uint32_t size = layouts->SizeOf(type);
      std::memcpy(destination, &raw, size);
      return;
    }

    case abi::ValueClass::kFloat: {
      if (layouts->BitsOf(type) == 64) {
        const double number = value.floating();
        std::memcpy(destination, &number, sizeof(number));
      } else {
        const float number = static_cast<float>(value.floating());
        std::memcpy(destination, &number, sizeof(number));
      }
      return;
    }

    case abi::ValueClass::kAggregate:
      break;
  }

  const abi::Layout& layout = layouts->Of(type);
  if (types_.KindOf(type) == TypeKind::kStr) {
    // A view of the bytes the interpreter's value owns. The value outlives
    // the call -- the caller's argument vector holds it -- which is the
    // same reason a `str` is safe to pass at all.
    const char* bytes = value.kind() == interp::Value::Kind::kString
                            ? value.text().data()
                            : nullptr;
    const uint64_t length = value.kind() == interp::Value::Kind::kString
                                ? value.text().size()
                                : 0;
    const auto address = reinterpret_cast<uint64_t>(bytes);
    std::memcpy(destination, &address, sizeof(address));
    std::memcpy(destination + 8, &length, sizeof(length));
    return;
  }
  if (types_.KindOf(type) == TypeKind::kOptional) {
    const uint8_t tag = value.kind() == interp::Value::Kind::kOptional &&
                                value.has_value()
                            ? 1
                            : 0;
    destination[0] = tag;
    if (tag != 0 && layout.offsets.size() > 1) {
      Encode(layouts, value.inside(), types_.ElementOf(type),
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
    const uint32_t stride = layouts->StrideOf(type);
    for (size_t i = 0; i < elements.size(); ++i) {
      Encode(layouts, elements[i], element, destination + i * stride);
    }
    return;
  }
  if (layouts->IsTagOnly(type)) {
    if (!elements.empty()) {
      const uint32_t tag = static_cast<uint32_t>(elements[0].integer());
      std::memcpy(destination, &tag, sizeof(tag));
    }
    return;
  }
  for (size_t i = 0; i < elements.size() && i < layout.offsets.size(); ++i) {
    const TypeId field = layouts->FieldType(type, static_cast<uint32_t>(i));
    if (field.is_valid()) {
      Encode(layouts, elements[i], field, destination + layout.offsets[i]);
    }
  }
}

interp::Value CompiledModule::Impl::Decode(abi::LayoutStore* layouts,
                                           const uint8_t* source,
                                           TypeId type) {
  switch (layouts->ClassOf(type)) {
    case abi::ValueClass::kVoid:
      return interp::Value::Void();
    case abi::ValueClass::kUnsupported:
      return interp::Value();

    case abi::ValueClass::kInteger: {
      uint64_t raw = 0;
      std::memcpy(&raw, source, layouts->SizeOf(type));
      if (types_.KindOf(type) == TypeKind::kBool) {
        return interp::Value::Bool((raw & 1) != 0);
      }
      const Type& described = types_.Get(type);
      return interp::Value::Int(raw, described.int_width,
                                described.is_signed);
    }

    case abi::ValueClass::kFloat: {
      if (layouts->BitsOf(type) == 64) {
        double number = 0;
        std::memcpy(&number, source, sizeof(number));
        return interp::Value::Float(number, FloatWidth::k64);
      }
      float number = 0;
      std::memcpy(&number, source, sizeof(number));
      return interp::Value::Float(static_cast<double>(number),
                                  FloatWidth::k32);
    }

    case abi::ValueClass::kAggregate:
      break;
  }

  const abi::Layout& layout = layouts->Of(type);
  if (types_.KindOf(type) == TypeKind::kStr) {
    uint64_t address = 0;
    uint64_t length = 0;
    std::memcpy(&address, source, sizeof(address));
    std::memcpy(&length, source + 8, sizeof(length));
    if (address == 0) {
      return interp::Value::Str(std::string());
    }
    return interp::Value::Str(
        std::string(reinterpret_cast<const char*>(address),
                    static_cast<size_t>(length)));
  }
  if (types_.KindOf(type) == TypeKind::kOptional) {
    if (source[0] == 0) {
      return interp::Value::Absent();
    }
    const TypeId inside = types_.ElementOf(type);
    const uint32_t offset =
        layout.offsets.size() > 1 ? layout.offsets[1] : 0;
    return interp::Value::Present(
        Decode(layouts, source + offset, inside));
  }
  if (types_.KindOf(type) == TypeKind::kArray) {
    const TypeId element = types_.ElementOf(type);
    const uint32_t stride = layouts->StrideOf(type);
    std::vector<interp::Value> elements;
    for (uint64_t i = 0; i < types_.Get(type).length; ++i) {
      elements.push_back(Decode(layouts, source + i * stride, element));
    }
    return interp::Value::Aggregate(std::move(elements));
  }
  if (layouts->IsTagOnly(type)) {
    uint32_t tag = 0;
    std::memcpy(&tag, source, sizeof(tag));
    std::vector<interp::Value> elements;
    elements.push_back(interp::Value::Int(tag, IntWidth::k32, true));
    return interp::Value::Aggregate(std::move(elements));
  }
  std::vector<interp::Value> elements;
  for (size_t i = 0; i < layout.offsets.size(); ++i) {
    const TypeId field = layouts->FieldType(type, static_cast<uint32_t>(i));
    if (!field.is_valid()) {
      continue;
    }
    elements.push_back(
        Decode(layouts, source + layout.offsets[i], field));
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

  FileState& file = *files_[entry->file];
  abi::LayoutStore* layouts = &file.layouts;
  const ir::Function& function = FunctionOf(*entry);
  const std::span<const ir::InstId> params =
      function.ParamsOf(function.entry());

  // One slot per parameter: the bits of a scalar, or the address of an
  // aggregate the caller laid out.
  std::vector<uint64_t> slots(std::max<size_t>(params.size(), 1), 0);
  std::vector<std::vector<uint8_t>> buffers(params.size());
  for (size_t i = 0; i < params.size(); ++i) {
    const TypeId type = function.Get(params[i]).type;
    if (layouts->ClassOf(type) == abi::ValueClass::kAggregate) {
      buffers[i].assign(std::max(layouts->SizeOf(type), 8u), 0);
      if (i < arguments.size()) {
        Encode(layouts, arguments[i], type, buffers[i].data());
      }
      uint8_t* address = buffers[i].data();
      std::memcpy(&slots[i], &address, sizeof(address));
      continue;
    }
    if (i < arguments.size()) {
      Encode(layouts, arguments[i], type,
             reinterpret_cast<uint8_t*>(&slots[i]));
    }
  }

  const TypeId returned = types_.ElementOf(function.type);
  std::vector<uint8_t> result(
      std::max(returned.is_valid() ? layouts->SizeOf(returned) : 0u, 16u), 0);

  // The context and the landing pad are members rather than locals, and
  // that is not a style choice: an object of automatic storage that is
  // changed between `setjmp` and `longjmp` has an indeterminate value
  // afterwards unless it is volatile. A local context is changed exactly
  // that way -- by generated code, through a pointer -- so the optimizer is
  // entitled to fold the trap kind back to the zero it was initialized
  // with. The release build did, and every trap came back as no trap at
  // all. Members live in the heap object, where the rule does not apply.
  //
  // One run at a time, consequently: a compiled call cannot be re-entered
  // from inside another one.
  context_ = Context();
  context_.landing = &landing_;
  context_.report = &ReportTrap;
  context_.code_begin = reinterpret_cast<uint64_t>(block_.AddressOf(0));
  context_.code_end =
      context_.code_begin + words_.size() * sizeof(uint32_t);

  const auto stub = reinterpret_cast<EntryPoint>(
      const_cast<void*>(block_.AddressOf(entry->stub)));

  if (_setjmp(landing_) == 0) {
    stub(&context_, slots.data(), result.data());
    if (returned.is_valid()) {
      outcome.value = Decode(layouts, result.data(), returned);
    } else {
      outcome.value = interp::Value::Void();
    }
    return outcome;
  }

  outcome.trap = static_cast<ir::TrapKind>(context_.trap_kind);
  if (context_.trap_site < traps_.size()) {
    outcome.where = traps_[context_.trap_site].where;
    outcome.message = traps_[context_.trap_site].message;
  }
  if (backtrace != nullptr) {
    for (uint32_t i = 0; i < context_.frame_count; ++i) {
      const std::string frame =
          Symbolize(context_.frames[i] - context_.code_begin);
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
  const auto word = static_cast<uint32_t>(offset / sizeof(uint32_t));
  for (const Entry& entry : entries_) {
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
    text.append(interner_.Text(FunctionOf(entry).name));
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
  for (const Impl::Entry& entry : impl_->entries_) {
    if (!entry.compiled) {
      continue;
    }
    listing.push_back(std::pair<std::string, uint32_t>(
        std::string(impl_->interner_.Text(impl_->FunctionOf(entry).name)),
        entry.word_count * static_cast<uint32_t>(sizeof(uint32_t))));
  }
  return listing;
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
                                        std::span<const FileCode> files) {
  auto compiled = std::unique_ptr<CompiledModule>(new CompiledModule());
  compiled->impl_ =
      std::make_unique<CompiledModule::Impl>(interner, types, files);
  compiled->impl_->Compile();
  return compiled;
}

std::unique_ptr<CompiledModule> Compile(const StringInterner& interner,
                                        const TypeStore& types,
                                        const FileResolution& resolution,
                                        const ir::Module& module) {
  const FileCode one{&resolution, &module};
  return Compile(interner, types, std::span<const FileCode>(&one, 1));
}

}  // namespace codegen
}  // namespace wolang
