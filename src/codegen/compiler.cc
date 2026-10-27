// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/codegen/compiler.h"

#include "wolang/runtime/classes.h"
#include "wolang/runtime/globals.h"

#include "wolang/interp/marshal.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <setjmp.h>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>


#include "wolang/base/check.h"
#include "wolang/codegen/code_block.h"
#include "wolang/abi/layout.h"
#include "wolang/codegen/runtime.h"
#include "wolang/codegen/target.h"
#include "wolang/runtime/builtins.h"
#include "wolang/runtime/library.h"

namespace wolang {
namespace codegen {

bool IsSupportedTarget() {
  return HasBackend(HostTarget());
}

static void SkipUnwinding(jmp_buf* landing) {
#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
  reinterpret_cast<_JUMP_BUFFER*>(landing)->Frame = 0;
#else
  (void)landing;
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
#if defined(_WIN32)
  // The underscore spelling is the one that does not unwind, and this
  // system only has the plain one -- which, with no destructors between
  // here and the landing pad, does the same thing (D184).
  longjmp(*static_cast<jmp_buf*>(context->landing), 1);
#else
  _longjmp(*static_cast<jmp_buf*>(context->landing), 1);
#endif
}

class CompiledModule::Impl {
 public:
  // Puts a nested run's context back when it ends, however it ends.
  struct RestoreOnExit {
    Impl* impl;
    Context context;
    jmp_buf* landing;

    ~RestoreOnExit() {
      impl->context_ = context;
      std::memcpy(&impl->landing_, landing, sizeof(jmp_buf));
    }
  };

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
  // Tells the runtime where every compiled virtual method ended up, which
  // is what a virtual call in compiled code looks up.
  void RegisterClasses();

  interp::Outcome Run(Symbol name,
                      const std::vector<interp::Value>& arguments,
                      std::vector<std::string>* backtrace,
                      bool keep_globals = false);


  // Which function an address belongs to, as a name and an offset. What
  // makes a trap from compiled code readable, and what a profiler needs to
  // label a sample.
  std::string Symbolize(uint64_t offset) const;

  uint64_t code_begin() const {
    return block_.is_installed()
               ? reinterpret_cast<uint64_t>(block_.AddressOf(0))
               : 0;
  }
  uint64_t code_end() const {
    return code_begin() == 0 ? 0 : code_begin() + bytes_.size();
  }

  // The function an address is inside, by name and nothing else: what a
  // profiler's sample is attributed to (D139).
  std::string FunctionAt(uint64_t address) const {
    const uint64_t begin = code_begin();
    if (begin == 0 || address < begin || address >= code_end()) {
      return std::string();
    }
    const std::string named = Symbolize(address - begin);
    const size_t plus = named.find('+');
    return plus == std::string::npos ? named : named.substr(0, plus);
  }

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
    // Where the body is, in bytes, which is what a call has to reach.
    uint32_t body = 0;
    // The function's whole code, for a dump.
    uint32_t begin = 0;
    uint32_t size = 0;
  };

  // The entry for the function declared at |file|:|item|, preferring a
  // definition over a declaration that names it (D140).
  const Entry* FindSource(uint32_t file, uint32_t item) const {
    for (uint32_t i = 0; i < entries_.size(); ++i) {
      const ir::Function& function = FunctionOf(entries_[i]);
      if (function.source_file != file || function.source_item != item) {
        continue;
      }
      const Entry& entry = entries_[i];
      return entry.compiled ? &entry : &entries_[entry.link];
    }
    return nullptr;
  }

  const Entry* Find(Symbol name) const {
    const auto found = by_name_.find(name.index);
    if (found == by_name_.end()) {
      return nullptr;
    }
    const Entry& entry = entries_[found->second];
    return entry.compiled ? &entry : &entries_[entry.link];
  }

  // By the declaration a function came from rather than by its name: a
  // name is not unique across a program (D140).
  interp::Outcome RunSource(uint32_t file,
                            uint32_t item,
                            const std::vector<interp::Value>& arguments,
                            std::vector<std::string>* backtrace,
                            bool keep_globals = false);

  bool ContainsSource(uint32_t file, uint32_t item) const {
    const Entry* entry = FindSource(file, item);
    return entry != nullptr && entry->compiled;
  }

  interp::Outcome RunEntry(const Entry* entry,
                           Symbol name,
                           const std::vector<interp::Value>& arguments,
                           std::vector<std::string>* backtrace,
                           bool keep_globals);

  const ir::Function& FunctionOf(const Entry& entry) const {
    return files_[entry.file]->module.functions[entry.local];
  }

  const StringInterner& interner_;
  const TypeStore& types_;
  const Target target_ = HostTarget();
  std::vector<std::unique_ptr<FileState>> files_;

  std::vector<uint8_t> bytes_;
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
          candidate.source_item == function.source_item &&
          candidate.source_slot == function.source_slot) {
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
    void* address = runtime::LookupSymbol(symbol);
    if (address == nullptr) {
      address = runtime::FindSymbol(symbol);
    }
    foreign[i] = reinterpret_cast<uint64_t>(address);
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
        EmitFunction(target_, interner_, types_, &file.layouts, file.module, function,
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
  std::vector<std::pair<uint32_t, AddressFixup>> addresses;
  for (uint32_t i = 0; i < entries_.size(); ++i) {
    Entry& entry = entries_[i];
    const ir::Function& function = FunctionOf(entry);
    if (!compilable[i] || function.is_declaration()) {
      entry.reason = reasons[i];
      ++skipped_count_;
      continue;
    }

    FileState& file = *files_[entry.file];
    // Placed where the code so far ends, so a call between two functions
    // of the same module is a displacement rather than an address.
    const auto base = static_cast<uint32_t>(bytes_.size());
    CompiledFunction compiled =
        EmitFunction(target_, interner_, types_, &file.layouts, file.module, function,
                     file.compilable, file.foreign, file.strings,
                     static_cast<uint32_t>(traps_.size()));
    WOLANG_CHECK(compiled.compiled);

    entry.compiled = true;
    entry.begin = base;
    entry.size = static_cast<uint32_t>(compiled.bytes.size());
    entry.stub = base + compiled.stub;
    entry.body = base + compiled.body;
    bytes_.insert(bytes_.end(), compiled.bytes.begin(),
                  compiled.bytes.end());
    for (TrapSite& site : compiled.traps) {
      traps_.push_back(std::move(site));
    }
    for (const CallFixup& fixup : compiled.calls) {
      fixups.emplace_back(entry.file,
                          CallFixup{base + fixup.at, fixup.callee});
    }
    for (const AddressFixup& fixup : compiled.addresses) {
      addresses.emplace_back(entry.file,
                             AddressFixup{base + fixup.at, fixup.callee});
    }
    ++compiled_count_;
  }

  // Only once every function has a place can the calls between them be
  // filled in. One module, one block of code: the shortest reach of the
  // targets here is 128 megabytes, and that is more code than this tier
  // will ever emit at once.
  for (const auto& [file_index, fixup] : fixups) {
    const FileState& file = *files_[file_index];
    WOLANG_CHECK(fixup.callee < file.global.size());
    const Entry& callee = entries_[entries_[file.global[fixup.callee]].link];
    WOLANG_CHECK(callee.compiled);
    PatchCall(target_, &bytes_, fixup.at, callee.body);
  }

  if (!bytes_.empty()) {
    if (addresses.empty()) {
      block_.Install(bytes_);
    } else {
      // A function used as a value needs its own address, so the block has
      // to be placed before the code is finished: reserve, patch, then
      // commit. The pages are writable in between and executable only
      // after, which is the same bargain `Install` makes in one step.
      const auto origin =
          reinterpret_cast<uint64_t>(block_.Reserve(bytes_.size()));
      if (origin != 0) {
        for (const auto& [file_index, fixup] : addresses) {
          const FileState& file = *files_[file_index];
          WOLANG_CHECK(fixup.callee < file.global.size());
          const Entry& callee =
              entries_[entries_[file.global[fixup.callee]].link];
          WOLANG_CHECK(callee.compiled);
          PatchAddress(target_, &bytes_, fixup.at, origin + callee.body);
        }
        block_.Commit(bytes_);
      }
    }
  }

  RegisterClasses();
}

// Where a virtual call finds its body. The addresses are known only once
// the code is placed, and the number an object carries is its class's
// declaration -- so this is the one place that can connect the two.
void CompiledModule::Impl::RegisterClasses() {
  if (!block_.is_installed()) {
    return;
  }
  for (const std::unique_ptr<FileState>& file : files_) {
    if (file == nullptr) {
      continue;
    }
    for (const ir::ClassInfo& klass : file->module.classes) {
      for (const ir::ClassInfo::Method& method : klass.methods) {
        if (method.function >= file->global.size()) {
          continue;
        }
        const Entry& entry =
            entries_[entries_[file->global[method.function]].link];
        if (!entry.compiled) {
          continue;
        }
        runtime::wolang_class_register(
            klass.token, method.selector,
            const_cast<void*>(block_.AddressOf(entry.body)));
      }
    }
  }
}

interp::Outcome CompiledModule::Impl::Run(
    Symbol name,
    const std::vector<interp::Value>& arguments,
    std::vector<std::string>* backtrace,
    bool keep_globals) {
  return RunEntry(Find(name), name, arguments, backtrace, keep_globals);
}

interp::Outcome CompiledModule::Impl::RunSource(
    uint32_t file,
    uint32_t item,
    const std::vector<interp::Value>& arguments,
    std::vector<std::string>* backtrace,
    bool keep_globals) {
  const Entry* entry = FindSource(file, item);
  const Symbol name = entry != nullptr ? FunctionOf(*entry).name : Symbol{};
  return RunEntry(entry, name, arguments, backtrace, keep_globals);
}

interp::Outcome CompiledModule::Impl::RunEntry(
    const Entry* entry,
    Symbol name,
    const std::vector<interp::Value>& arguments,
    std::vector<std::string>* backtrace,
    bool keep_globals) {
  interp::Outcome outcome;
  if (entry == nullptr || !entry->compiled || !block_.is_installed()) {
    outcome.trap = ir::TrapKind::kNoBody;
    outcome.where = "@" + std::string(interner_.Text(name));
    outcome.message = "was not compiled";
    return outcome;
  }

  // Module-level `var`s start from their type's zero and then take the
  // values the source wrote, every time something is run: the storage
  // outlives one run, and a differential check against a mutated global
  // would be comparing nothing (D88).
  if (!keep_globals) {
    runtime::wolang_global_reset();
  }
  for (const std::unique_ptr<FileState>& other : files_) {
    if (keep_globals) {
      break;
    }
    if (other == nullptr ||
        other->module.initializer >= other->module.functions.size()) {
      continue;
    }
    const Symbol init = other->module.functions[other->module.initializer].name;
    if (const Entry* found = Find(init); found != nullptr && found->compiled) {
      const auto body = reinterpret_cast<void (*)()>(
          const_cast<void*>(block_.AddressOf(found->body)));
      body();
    }
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
        interp::Encode(types_, layouts, arguments[i], type,
                       buffers[i].data());
      }
      uint8_t* address = buffers[i].data();
      std::memcpy(&slots[i], &address, sizeof(address));
      continue;
    }
    if (i < arguments.size()) {
      interp::Encode(types_, layouts, arguments[i], type,
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
  // A run can happen inside a run: the scheduler calls back into the tier
  // to advance a task, and that task may start another one. The context
  // and the landing pad are members because they have to outlive a
  // `longjmp`, so a nested run saves and restores them around itself.
  const Context outer_context = context_;
  jmp_buf outer_landing;
  std::memcpy(&outer_landing, &landing_, sizeof(jmp_buf));
  const RestoreOnExit restore{this, outer_context, &outer_landing};

  context_ = Context();
  context_.landing = &landing_;
  context_.report = &ReportTrap;
  context_.code_begin = reinterpret_cast<uint64_t>(block_.AddressOf(0));
  context_.code_end =
      context_.code_begin + bytes_.size();

  const auto stub = reinterpret_cast<EntryPoint>(
      const_cast<void*>(block_.AddressOf(entry->stub)));

  if (_setjmp(landing_) == 0) {
    SkipUnwinding(&landing_);
    stub(&context_, slots.data(), result.data());
    if (returned.is_valid()) {
      outcome.value =
        interp::Decode(types_, layouts, result.data(), returned);
    } else {
      outcome.value = interp::Value::Void();
    }
    return outcome;
  }

  outcome.trap = static_cast<ir::TrapKind>(context_.trap_kind);
  if (context_.trap_site < traps_.size()) {
    outcome.where = traps_[context_.trap_site].where;
    outcome.message = traps_[context_.trap_site].message;
    outcome.span = traps_[context_.trap_site].span;
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
  const auto at = static_cast<uint32_t>(offset);
  for (const Entry& entry : entries_) {
    if (!entry.compiled || at < entry.begin ||
        at >= entry.begin + entry.size) {
      continue;
    }
    if (at < entry.body) {
      // The entry trampoline, which belongs to the host side of the call
      // rather than to the function.
      return {};
    }
    std::string text = "@";
    text.append(interner_.Text(FunctionOf(entry).name));
    text.append("+0x");
    char buffer[16] = {};
    std::snprintf(buffer, sizeof(buffer), "%x",
                  static_cast<unsigned>(offset - entry.begin));
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

uint64_t CompiledModule::code_begin() const {
  return impl_->code_begin();
}

uint64_t CompiledModule::code_end() const {
  return impl_->code_end();
}

std::string CompiledModule::FunctionAt(uint64_t address) const {
  return impl_->FunctionAt(address);
}

interp::Outcome CompiledModule::Run(
    Symbol name,
    const std::vector<interp::Value>& arguments,
    std::vector<std::string>* backtrace,
    bool keep_globals) const {
  return impl_->Run(name, arguments, backtrace, keep_globals);
}

interp::Outcome CompiledModule::RunSource(
    uint32_t file,
    uint32_t item,
    const std::vector<interp::Value>& arguments,
    std::vector<std::string>* backtrace,
    bool keep_globals) const {
  return impl_->RunSource(file, item, arguments, backtrace, keep_globals);
}

bool CompiledModule::ContainsSource(uint32_t file, uint32_t item) const {
  return impl_->ContainsSource(file, item);
}

uintptr_t CompiledModule::Prepare(uint32_t file, uint32_t item) const {
  const Impl::Entry* entry = impl_->FindSource(file, item);
  return entry != nullptr && entry->compiled ? reinterpret_cast<uintptr_t>(entry) : 0;
}

bool CompiledModule::CallPrepared(uintptr_t prepared, const uint64_t* slots, void* result, interp::Outcome* trapped) const {
  Impl& impl = *impl_;
  const auto* entry = reinterpret_cast<const Impl::Entry*>(prepared);
  if (entry == nullptr || !impl.block_.is_installed()) {
    trapped->trap = ir::TrapKind::kNoBody;
    trapped->message = "was not compiled";
    return false;
  }

  // The same frame `ResumeTask` sets up: a run may be inside a run, so the
  // context and the landing pad are saved around this one.
  const Context outer_context = impl.context_;
  jmp_buf outer_landing;
  std::memcpy(&outer_landing, &impl.landing_, sizeof(jmp_buf));
  const Impl::RestoreOnExit restore{&impl, outer_context, &outer_landing};

  impl.context_ = Context();
  impl.context_.landing = &impl.landing_;
  impl.context_.report = &ReportTrap;
  impl.context_.code_begin = reinterpret_cast<uint64_t>(impl.block_.AddressOf(0));
  impl.context_.code_end = impl.context_.code_begin + impl.bytes_.size();

  const auto stub = reinterpret_cast<EntryPoint>(const_cast<void*>(impl.block_.AddressOf(entry->stub)));
  if (_setjmp(impl.landing_) != 0) {
    trapped->trap = static_cast<ir::TrapKind>(impl.context_.trap_kind);
    if (impl.context_.trap_site < impl.traps_.size()) {
      trapped->where = impl.traps_[impl.context_.trap_site].where;
      trapped->message = impl.traps_[impl.context_.trap_site].message;
      trapped->span = impl.traps_[impl.context_.trap_site].span;
    }
    return false;
  }

  SkipUnwinding(&impl.landing_);
  stub(&impl.context_, const_cast<uint64_t*>(slots), result);
  return true;
}

bool CompiledModule::ResumeTask(Symbol dispatch,
                                int32_t kind,
                                void* frame,
                                bool* faulted) const {
  Impl& impl = *impl_;
  const Impl::Entry* entry = impl.Find(dispatch);
  if (entry == nullptr || !entry->compiled || !impl.block_.is_installed()) {
    *faulted = true;
    return true;
  }

  // Nothing here allocates: two slots in, one word out, and the stub is
  // the same one every step.
  uint64_t slots[2] = {static_cast<uint64_t>(static_cast<int64_t>(kind)),
                       reinterpret_cast<uint64_t>(frame)};
  uint64_t result = 0;

  const Context outer_context = impl.context_;
  jmp_buf outer_landing;
  std::memcpy(&outer_landing, &impl.landing_, sizeof(jmp_buf));
  const Impl::RestoreOnExit restore{&impl, outer_context, &outer_landing};

  impl.context_ = Context();
  impl.context_.landing = &impl.landing_;
  impl.context_.report = &ReportTrap;
  impl.context_.code_begin =
      reinterpret_cast<uint64_t>(impl.block_.AddressOf(0));
  impl.context_.code_end =
      impl.context_.code_begin + impl.bytes_.size();

  const auto stub = reinterpret_cast<EntryPoint>(
      const_cast<void*>(impl.block_.AddressOf(entry->stub)));
  if (_setjmp(impl.landing_) != 0) {
    *faulted = true;
    return true;
  }

  SkipUnwinding(&impl.landing_);
  stub(&impl.context_, slots, &result);
  *faulted = false;
  return (result & 1) != 0;
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
        entry.size));
  }
  return listing;
}

size_t CompiledModule::size() const {
  return impl_->bytes_.size();
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
