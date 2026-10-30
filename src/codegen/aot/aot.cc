// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/codegen/aot.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <cstdlib>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "wolang/ir/trap.h"

#if WOLANG_WITH_LLVM
#include <llvm-c/Analysis.h>
#include <llvm-c/Support.h>
#include <llvm-c/Core.h>
#include <llvm-c/DebugInfo.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Transforms/PassBuilder.h>
#endif

namespace wolang {
namespace codegen {

std::string HostTriple() {
#if WOLANG_WITH_LLVM
  char* host = LLVMGetDefaultTargetTriple();
  const std::string held = host != nullptr ? host : "";
  if (host != nullptr) {
    LLVMDisposeMessage(host);
  }
  return held;
#else
  // No backend to ask, and nothing to compare a `--target` against.
  return "";
#endif
}

bool HasAotBackend() {
#if WOLANG_WITH_LLVM
  return true;
#else
  return false;
#endif
}

std::string_view RuntimeArchive() {
#ifdef WOLANG_RUNTIME_ARCHIVE
  return WOLANG_RUNTIME_ARCHIVE;
#else
  return {};
#endif
}

std::string LinkCommand(std::string_view object, std::string_view binary) {
  std::string command;
#if defined(_WIN32)
#ifdef WOLANG_RUNTIME_CRT
  const std::string_view crt = WOLANG_RUNTIME_CRT;
#else
  const std::string_view crt = "/MD";
#endif
  command.append("clang-cl /nologo ");
  command.append(crt);
  command.append(" \"");
  command.append(object);
  command.append("\" \"");
  command.append(RuntimeArchive());
  command.append("\" /Fe\"");
  command.append(binary);
  command.append("\" /link ws2_32.lib bcrypt.lib secur32.lib");
#else
  command.append("c++ -o '");
  command.append(binary);
  command.append("' '");
  command.append(object);
  command.append("' '");
  command.append(RuntimeArchive());
  command.append("' -lpthread");
#if !defined(__APPLE__)
  command.append(" -ldl");
#endif
#endif
  return command;
}

std::string LinkPathFlag(std::string_view path) {
#if defined(_WIN32)
  return " /LIBPATH:\"" + std::string(path) + "\"";
#else
  return " -L'" + std::string(path) + "'";
#endif
}

std::string LinkLibraryFlag(std::string_view library) {
#if defined(_WIN32)
  return " " + std::string(library) + ".lib";
#else
  return " -l" + std::string(library);
#endif
}

std::string BinaryName(std::string_view stem) {
  std::string name(stem);
#if defined(_WIN32)
  if (name.size() < 4 || name.compare(name.size() - 4, 4, ".exe") != 0) {
    name.append(".exe");
  }
#endif
  return name;
}

#if !WOLANG_WITH_LLVM

AotResult EmitObject(const StringInterner&,
                     const TypeStore&,
                     std::span<const FileCode>,
                     const AotOptions&) {
  return AotResult{"this build has no ahead-of-time backend", {}};
}

#else

namespace {

// One function of one file, and which definition it links to.
struct Entry {
  uint32_t file = 0;
  uint32_t local = 0;
  uint32_t link = 0;
  LLVMValueRef function = nullptr;
  // Why it was not translated, or empty.
  std::string skipped;
};

// What a value of a Wolang type is, in LLVM.
//
// Every scalar is an integer or a float of its own width, and everything
// else -- a struct, a slice, a string, a class handle, a pointer -- is an
// address held in an `i64`. One rule, and the reason for it is that the
// optimizer in `ir/passes.h` has already turned element addresses into
// ordinary arithmetic: a value that is added to has to be a number, and a
// number that is read through becomes a pointer at the one place that
// reads it (D98).
class Translator {
 public:
  Translator(const StringInterner& interner,
             const TypeStore& types,
             const SourceManager* sources,
             std::span<const FileCode> files)
      : interner_(interner), types_(types), sources_(sources) {
    for (const FileCode& file : files) {
      files_.push_back(file);
      layouts_.push_back(std::make_unique<abi::LayoutStore>(
          types, *file.resolution));
    }
  }

  AotResult Run(const AotOptions& options);

 private:
  const ir::Function& FunctionOf(const Entry& entry) const {
    return files_[entry.file].module->functions[entry.local];
  }
  abi::LayoutStore& Layouts(uint32_t file) { return *layouts_[file]; }

  // --------------------------------------------------------- debug info ---

  FileId FirstFile() const {
    for (const FileCode& file : files_) {
      if (file.module != nullptr) {
        for (const ir::Function& function : file.module->functions) {
          for (const SourceSpan span : function.spans) {
            if (span.is_valid()) {
              return span.file;
            }
          }
        }
      }
    }
    return FileId{};
  }

  // The debug descriptor for a source file, made once.
  LLVMMetadataRef DebugFileOf(FileId file) {
    if (debug_ == nullptr) {
      return nullptr;
    }
    const auto found = debug_files_.find(file.index);
    if (found != debug_files_.end()) {
      return found->second;
    }
    std::string path =
        sources_ != nullptr && file.is_valid()
            ? std::string(sources_->Path(file))
            : std::string("<wolang>");
    std::string directory = ".";
    const size_t slash = path.rfind('/');
    if (slash != std::string::npos) {
      directory = path.substr(0, slash);
      path = path.substr(slash + 1);
    }
    LLVMMetadataRef descriptor = LLVMDIBuilderCreateFile(
        debug_, path.c_str(), path.size(), directory.c_str(),
        directory.size());
    debug_files_[file.index] = descriptor;
    return descriptor;
  }

  // The subprogram for a function: what a debugger names a frame, and
  // what a line table hangs off.
  LLVMMetadataRef DebugFunctionOf(const Entry& entry,
                                  LLVMValueRef function) {
    if (debug_ == nullptr) {
      return nullptr;
    }
    const ir::Function& ir = FunctionOf(entry);
    SourceSpan where;
    for (const SourceSpan span : ir.spans) {
      if (span.is_valid()) {
        where = span;
        break;
      }
    }
    LLVMMetadataRef file = DebugFileOf(where.file);
    const unsigned line =
        where.is_valid() && sources_ != nullptr
            ? sources_->Position(where).line
            : 1;
    const std::string name(interner_.Text(ir.name));
    size_t length = 0;
    const char* linkage = LLVMGetValueName2(function, &length);
    LLVMMetadataRef signature = LLVMDIBuilderCreateSubroutineType(
        debug_, file, nullptr, 0, LLVMDIFlagZero);
    return LLVMDIBuilderCreateFunction(
        debug_, file, name.c_str(), name.size(), linkage, length, file,
        line, signature, /*IsLocalToUnit=*/0, /*IsDefinition=*/1, line,
        LLVMDIFlagZero, /*IsOptimized=*/0);
  }

  // Where the instruction being translated came from. Attached to
  // everything the builder emits until it moves.
  void SetDebugLocation(SourceSpan span) {
    if (debug_ == nullptr || scope_ == nullptr) {
      return;
    }
    // A synthesized instruction has no span of its own; it belongs to the
    // function it is in, which is what its declaration line says. Every
    // instruction gets a location, because a function with debug
    // information and some locations missing is what LLVM warns about.
    unsigned line = function_line_;
    unsigned column = 1;
    if (span.is_valid() && sources_ != nullptr) {
      const LineColumn at = sources_->Position(span);
      line = at.line;
      column = at.column;
    }
    LLVMSetCurrentDebugLocation2(
        b_, LLVMDIBuilderCreateDebugLocation(ctx_, line, column, scope_,
                                             nullptr));
  }

  // ------------------------------------------------------------- types ---

  LLVMTypeRef I(uint32_t bits) { return LLVMIntTypeInContext(ctx_, bits); }
  LLVMTypeRef I64() { return I(64); }
  LLVMTypeRef Ptr() { return LLVMPointerTypeInContext(ctx_, 0); }

  // The LLVM type a value of |type| is held in.
  LLVMTypeRef Held(uint32_t file, TypeId type) {
    if (!type.is_valid()) {
      return LLVMVoidTypeInContext(ctx_);
    }
    abi::LayoutStore& layouts = Layouts(file);
    switch (layouts.ClassOf(type)) {
      case abi::ValueClass::kVoid:
        return LLVMVoidTypeInContext(ctx_);
      case abi::ValueClass::kFloat:
        return layouts.SizeOf(type) == 4 ? LLVMFloatTypeInContext(ctx_)
                                         : LLVMDoubleTypeInContext(ctx_);
      case abi::ValueClass::kInteger: {
        const uint32_t size = layouts.SizeOf(type);
        return I(size == 0 ? 64 : size * 8);
      }
      default:
        // An aggregate travels as the address of its storage.
        return I64();
    }
  }

  bool IsAggregate(uint32_t file, TypeId type) {
    return type.is_valid() &&
           Layouts(file).ClassOf(type) == abi::ValueClass::kAggregate;
  }

  // ------------------------------------------------------- declarations ---

  // The signature of a Wolang function: an aggregate result is written
  // through a pointer the caller passes first, which is what the platform
  // ABI does with one anyway.
  LLVMTypeRef Signature(const Entry& entry);
  void Declare();
  bool Translate(Entry* entry);
  void EmitEntryPoint();
  void EmitExports(const std::vector<uint32_t>& exported);
  std::string Unsupported(const Entry& entry);

  // ---------------------------------------------------------- emission ---

  LLVMValueRef Const(uint64_t bits, LLVMTypeRef type) {
    return LLVMConstInt(type, bits, /*SignExtend=*/false);
  }
  LLVMValueRef Const64(uint64_t bits) { return Const(bits, I64()); }

  // |value| as an address LLVM will read or write through.
  LLVMValueRef AsPointer(LLVMValueRef value) {
    if (LLVMGetTypeKind(LLVMTypeOf(value)) == LLVMPointerTypeKind) {
      return value;
    }
    return LLVMBuildIntToPtr(b_, Widen(value), Ptr(), "p");
  }
  // |value| as a 64-bit number, which is what every address is held as.
  LLVMValueRef Widen(LLVMValueRef value) {
    LLVMTypeRef type = LLVMTypeOf(value);
    if (LLVMGetTypeKind(type) == LLVMPointerTypeKind) {
      return LLVMBuildPtrToInt(b_, value, I64(), "a");
    }
    if (LLVMGetTypeKind(type) != LLVMIntegerTypeKind) {
      return value;
    }
    const uint32_t bits = LLVMGetIntTypeWidth(type);
    if (bits == 64) {
      return value;
    }
    return LLVMBuildZExt(b_, value, I64(), "w");
  }
  // |value| in |type|, for a jump's argument and a call's.
  LLVMValueRef Coerce(LLVMValueRef value, LLVMTypeRef type);

  LLVMValueRef Val(ir::InstId id) {
    if (!id.is_valid() || id.index >= values_.size() ||
        values_[id.index] == nullptr) {
      return Const64(0);
    }
    return values_[id.index];
  }

  // A block that reports |message| and stops the program. One per site,
  // so that the message says which operation it was.
  void TrapIf(LLVMValueRef condition,
              ir::TrapKind kind,
              const std::string& message);
  LLVMValueRef GlobalText(const std::string& text);
  LLVMValueRef Declaration(const char* name,
                           LLVMTypeRef result,
                           std::vector<LLVMTypeRef> parameters);

  void Emit(ir::InstId id, const ir::Inst& inst);
  void EmitArithmetic(ir::InstId id, const ir::Inst& inst);
  void EmitCompare(ir::InstId id, const ir::Inst& inst);
  void EmitCast(ir::InstId id, const ir::Inst& inst);
  void EmitCall(ir::InstId id, const ir::Inst& inst);
  void EmitTerminator(const ir::Inst& inst);
  void EmitJumpArguments(const ir::Inst& inst, size_t which,
                         ir::BlockId target, size_t first_operand);
  void Copy(LLVMValueRef to, LLVMValueRef from, uint64_t size);
  // |count| rounded up to a multiple of sixteen: what every field's array
  // really holds, so that each one starts 16-byte aligned.
  LLVMValueRef PaddedCount(LLVMValueRef count) {
    return LLVMBuildAnd(b_, LLVMBuildAdd(b_, count, Const64(15), "r"),
                        Const64(~uint64_t{15}), "r");
  }
  void StoreScalar(LLVMValueRef address, LLVMValueRef value, TypeId type);
  LLVMValueRef LoadHeld(LLVMValueRef address, TypeId type, uint64_t at);
  void Produce(ir::InstId id, LLVMValueRef value) {
    values_[id.index] = value;
  }
  // Where an aggregate result lives: storage made once in the entry
  // block, not once per execution -- an `alloca` in a loop would grow the
  // stack until it ran out.
  LLVMValueRef Storage(ir::InstId id) { return storage_[id.index]; }

  TypeId TypeOf(ir::InstId id) const {
    return function_->Contains(id) ? function_->Get(id).type : TypeId{};
  }
  TypeId Addressed(ir::InstId base) const {
    const TypeId type = TypeOf(base);
    if (types_.IsHandle(type) || types_.KindOf(type) != TypeKind::kPointer) {
      return type;
    }
    return types_.ElementOf(type);
  }

  const StringInterner& interner_;
  const TypeStore& types_;
  std::vector<FileCode> files_;
  std::vector<std::unique_ptr<abi::LayoutStore>> layouts_;
  // What a library is called, or empty for a program.
  std::string library_;
  std::vector<Entry> entries_;
  // Which global entry each file's local function index is.
  std::vector<std::vector<uint32_t>> global_;
  std::unordered_map<std::string, LLVMValueRef> declared_;
  std::unordered_map<std::string, LLVMValueRef> texts_;

  LLVMContextRef ctx_ = nullptr;
  LLVMModuleRef mod_ = nullptr;
  LLVMBuilderRef b_ = nullptr;
  // Debug information: what lets a debugger stop on a line of Wolang
  // rather than on an address, and what a profiler needs to attribute a
  // sample to a function (D118).
  LLVMDIBuilderRef debug_ = nullptr;
  LLVMMetadataRef unit_ = nullptr;
  LLVMMetadataRef scope_ = nullptr;
  unsigned function_line_ = 1;
  std::unordered_map<uint32_t, LLVMMetadataRef> debug_files_;
  const SourceManager* sources_ = nullptr;

  // The function being translated.
  const ir::Function* function_ = nullptr;
  uint32_t file_ = 0;
  LLVMValueRef llvm_function_ = nullptr;
  std::string function_name_;
  // The pointer an aggregate result is written through.
  LLVMValueRef result_out_ = nullptr;
  std::vector<LLVMValueRef> values_;
  std::vector<LLVMValueRef> storage_;
  std::vector<LLVMBasicBlockRef> blocks_;
};

// ------------------------------------------------------------ declaring ---

LLVMTypeRef Translator::Signature(const Entry& entry) {
  const ir::Function& function = FunctionOf(entry);
  const TypeId result = types_.ElementOf(function.type);
  std::vector<LLVMTypeRef> parameters;
  const bool indirect = IsAggregate(entry.file, result);
  if (indirect) {
    parameters.push_back(I64());
  }
  if (!function.is_declaration()) {
    for (ir::InstId param : function.ParamsOf(function.entry())) {
      parameters.push_back(Held(entry.file, function.Get(param).type));
    }
  } else {
    if (function.self_type.is_valid()) {
      parameters.push_back(I64());
    }
    for (TypeId argument : types_.ArgumentsOf(function.type)) {
      parameters.push_back(Held(entry.file, argument));
    }
  }
  LLVMTypeRef returned =
      indirect ? LLVMVoidTypeInContext(ctx_) : Held(entry.file, result);
  return LLVMFunctionType(returned, parameters.data(),
                          static_cast<unsigned>(parameters.size()),
                          /*IsVarArg=*/0);
}

void Translator::Declare() {
  for (uint32_t f = 0; f < files_.size(); ++f) {
    global_.emplace_back();
    for (uint32_t i = 0; i < files_[f].module->functions.size(); ++i) {
      global_[f].push_back(static_cast<uint32_t>(entries_.size()));
      Entry entry;
      entry.file = f;
      entry.local = i;
      entry.link = static_cast<uint32_t>(entries_.size());
      entries_.push_back(entry);
    }
  }

  // A declaration resolves to the definition it names -- by the item it
  // came from, not by name, because two files may each define a `Helper`.
  for (uint32_t i = 0; i < entries_.size(); ++i) {
    const ir::Function& function = FunctionOf(entries_[i]);
    if (!function.is_declaration() || function.is_foreign() ||
        !function.has_source()) {
      continue;
    }
    for (uint32_t j = 0; j < entries_.size(); ++j) {
      const ir::Function& candidate = FunctionOf(entries_[j]);
      if (i == j || candidate.is_declaration() || !candidate.has_source()) {
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

  // One LLVM function per definition, and per foreign symbol. A name is
  // made unique by its index: two files may define the same name, and the
  // linker would otherwise have to choose.
  for (uint32_t i = 0; i < entries_.size(); ++i) {
    Entry& entry = entries_[i];
    const ir::Function& function = FunctionOf(entry);
    if (function.is_foreign()) {
      const std::string symbol(interner_.Text(function.foreign_symbol));
      const auto existing = declared_.find(symbol);
      if (existing != declared_.end()) {
        entry.function = existing->second;
        continue;
      }
      entry.function = LLVMAddFunction(mod_, symbol.c_str(), Signature(entry));
      declared_[symbol] = entry.function;
      continue;
    }
    if (function.is_declaration()) {
      // A declaration that found no definition and is not foreign: the
      // one case is `Print` in a file compiled on its own, which the
      // interpreter answers with an intrinsic. Pointed at the runtime's
      // own, so that a binary built from such a file prints rather than
      // trapping (D161).
      // `link` points at itself until a definition is found, so a
      // declaration that still points at itself found none.
      if (entry.link == i &&
          interner_.Text(function.name) == "Print" &&
          types_.ArgumentsOf(function.type).size() == 1) {
        const auto existing = declared_.find("wolang_print");
        entry.function = existing != declared_.end()
                             ? existing->second
                             : LLVMAddFunction(mod_, "wolang_print",
                                               Signature(entry));
        declared_["wolang_print"] = entry.function;
      }
      continue;
    }
    std::string name = "wo$";
    name.append(std::to_string(i));
    name.push_back('$');
    for (const char letter : interner_.Text(function.name)) {
      name.push_back((letter >= 'a' && letter <= 'z') ||
                             (letter >= 'A' && letter <= 'Z') ||
                             (letter >= '0' && letter <= '9')
                         ? letter
                         : '_');
    }
    entry.function = LLVMAddFunction(mod_, name.c_str(), Signature(entry));
  }
}

// ---------------------------------------------------------------- helpers ---

LLVMValueRef Translator::GlobalText(const std::string& text) {
  const auto existing = texts_.find(text);
  if (existing != texts_.end()) {
    return existing->second;
  }
  LLVMValueRef global = LLVMAddGlobal(
      mod_,
      LLVMArrayType(LLVMInt8TypeInContext(ctx_),
                    static_cast<unsigned>(text.size() + 1)),
      "wo$text");
  LLVMSetInitializer(global,
                     LLVMConstStringInContext(
                         ctx_, text.c_str(),
                         static_cast<unsigned>(text.size()),
                         /*DontNullTerminate=*/0));
  LLVMSetLinkage(global, LLVMPrivateLinkage);
  LLVMSetGlobalConstant(global, 1);
  LLVMSetUnnamedAddress(global, LLVMGlobalUnnamedAddr);
  texts_[text] = global;
  return global;
}

LLVMValueRef Translator::Declaration(const char* name,
                                     LLVMTypeRef result,
                                     std::vector<LLVMTypeRef> parameters) {
  const auto existing = declared_.find(name);
  if (existing != declared_.end()) {
    return existing->second;
  }
  LLVMTypeRef type =
      LLVMFunctionType(result, parameters.data(),
                       static_cast<unsigned>(parameters.size()),
                       /*IsVarArg=*/0);
  LLVMValueRef function = LLVMAddFunction(mod_, name, type);
  declared_[name] = function;
  return function;
}

LLVMValueRef Translator::Coerce(LLVMValueRef value, LLVMTypeRef type) {
  LLVMTypeRef from = LLVMTypeOf(value);
  if (from == type) {
    return value;
  }
  const LLVMTypeKind to_kind = LLVMGetTypeKind(type);
  if (to_kind == LLVMPointerTypeKind) {
    return AsPointer(value);
  }
  if (LLVMGetTypeKind(from) == LLVMPointerTypeKind) {
    value = LLVMBuildPtrToInt(b_, value, I64(), "a");
    from = LLVMTypeOf(value);
  }
  if (to_kind == LLVMIntegerTypeKind &&
      LLVMGetTypeKind(from) == LLVMIntegerTypeKind) {
    const uint32_t have = LLVMGetIntTypeWidth(from);
    const uint32_t want = LLVMGetIntTypeWidth(type);
    if (have == want) {
      return value;
    }
    return have < want ? LLVMBuildZExt(b_, value, type, "w")
                       : LLVMBuildTrunc(b_, value, type, "n");
  }
  if (to_kind == LLVMVoidTypeKind) {
    return value;
  }
  // A float of the wrong width, or an integer where a float was wanted:
  // both are the compiler disagreeing with itself, and a cast keeps the
  // module verifiable so that the disagreement is reported rather than
  // crashing LLVM.
  if (to_kind == LLVMFloatTypeKind || to_kind == LLVMDoubleTypeKind) {
    if (LLVMGetTypeKind(from) == LLVMIntegerTypeKind) {
      return LLVMBuildBitCast(b_, Coerce(value, I(to_kind == LLVMFloatTypeKind
                                                      ? 32
                                                      : 64)),
                              type, "f");
    }
    return to_kind == LLVMFloatTypeKind
               ? LLVMBuildFPTrunc(b_, value, type, "f")
               : LLVMBuildFPExt(b_, value, type, "f");
  }
  return value;
}

void Translator::TrapIf(LLVMValueRef condition,
                        ir::TrapKind kind,
                        const std::string& message) {
  LLVMBasicBlockRef trap =
      LLVMAppendBasicBlockInContext(ctx_, llvm_function_, "trap");
  LLVMBasicBlockRef ok =
      LLVMAppendBasicBlockInContext(ctx_, llvm_function_, "ok");
  LLVMBuildCondBr(b_, condition, trap, ok);
  LLVMPositionBuilderAtEnd(b_, trap);
  LLVMValueRef reporter =
      Declaration("wolang_trap", LLVMVoidTypeInContext(ctx_),
                  {I(32), Ptr(), Ptr()});
  LLVMValueRef arguments[3] = {Const(static_cast<uint64_t>(kind), I(32)),
                               GlobalText(message),
                               GlobalText(function_name_)};
  LLVMBuildCall2(b_, LLVMGlobalGetValueType(reporter), reporter, arguments,
                 3, "");
  LLVMBuildUnreachable(b_);
  LLVMPositionBuilderAtEnd(b_, ok);
}

void Translator::Copy(LLVMValueRef to, LLVMValueRef from, uint64_t size) {
  if (size == 0) {
    return;
  }
  LLVMBuildMemCpy(b_, AsPointer(to), 1, AsPointer(from), 1,
                  Const64(size));
}

void Translator::StoreScalar(LLVMValueRef address,
                             LLVMValueRef value,
                             TypeId type) {
  LLVMValueRef held = Coerce(value, Held(file_, type));
  LLVMBuildStore(b_, held, AsPointer(address));
}

LLVMValueRef Translator::LoadHeld(LLVMValueRef address,
                                  TypeId type,
                                  uint64_t at) {
  LLVMValueRef base = AsPointer(address);
  if (at != 0) {
    LLVMValueRef offset = Const64(at);
    base = LLVMBuildGEP2(b_, LLVMInt8TypeInContext(ctx_), base, &offset, 1,
                         "o");
  }
  return LLVMBuildLoad2(b_, Held(file_, type), base, "v");
}

// What this backend does not translate yet.
//
// Asked before anything is built, not while building it: a call to a
// function that turns out to be unsupported has to be a trap from the
// start, because a body removed afterwards leaves every call to it
// pointing at nothing.
std::string Translator::Unsupported(const Entry& entry) {
  const ir::Function& function = FunctionOf(entry);
  if (function.is_declaration() || function.is_foreign()) {
    return {};
  }
  abi::LayoutStore& layouts = Layouts(entry.file);
  for (const ir::Block& block : function.blocks) {
    for (ir::InstId id : block.instructions) {
      const ir::Inst& inst = function.Get(id);
      if (inst.type.is_valid() && !layouts.IsSupported(inst.type)) {
        return "a type this backend cannot lay out";
      }
      switch (inst.opcode) {
        case ir::Opcode::kSpawnCall:
        case ir::Opcode::kWaitCall:
          return "a task";
        default:
          break;
      }
    }
  }
  return {};
}

// ------------------------------------------------------------ translating ---

bool Translator::Translate(Entry* entry) {
  const ir::Function& function = FunctionOf(*entry);
  if (function.is_declaration() || function.is_foreign() ||
      entry->function == nullptr) {
    return true;
  }
  function_ = &function;
  file_ = entry->file;
  llvm_function_ = entry->function;
  function_name_ = std::string("@") + std::string(
                                          interner_.Text(function.name));
  // The subprogram first, and a location inside it before anything is
  // emitted: a location belongs to the function it is in, and one left
  // over from the previous function is what the verifier calls a `!dbg`
  // attachment pointing at the wrong subprogram (D118).
  scope_ = DebugFunctionOf(*entry, llvm_function_);
  function_line_ = 1;
  for (const SourceSpan span : function.spans) {
    if (span.is_valid() && sources_ != nullptr) {
      function_line_ = sources_->Position(span).line;
      break;
    }
  }
  if (scope_ != nullptr) {
    LLVMSetSubprogram(llvm_function_, scope_);
  } else {
    LLVMSetCurrentDebugLocation2(b_, nullptr);
  }

  abi::LayoutStore& layouts = Layouts(file_);
  const size_t count = function.instructions.size();
  values_.assign(count, nullptr);
  storage_.assign(count, nullptr);
  blocks_.clear();
  for (const ir::Block& block : function.blocks) {
    blocks_.push_back(LLVMAppendBasicBlockInContext(
        ctx_, llvm_function_,
        block.label.empty() ? "b" : block.label.c_str()));
  }

  // Storage for everything that lives in memory, made once in the entry
  // block. Once per execution would grow the stack every time round a
  // loop, because nothing gives an `alloca` back until the function
  // returns.
  LLVMPositionBuilderAtEnd(b_, blocks_[0]);
  SetDebugLocation(SourceSpan());
  LLVMTypeRef byte = LLVMInt8TypeInContext(ctx_);
  for (const ir::Block& block : function.blocks) {
    for (ir::InstId id : block.instructions) {
      const ir::Inst& inst = function.Get(id);
      uint64_t size = 0;
      uint32_t align = 8;
      if (inst.opcode == ir::Opcode::kAlloca) {
        const uint32_t slots = inst.payload.slots == 0 ? 1 : inst.payload.slots;
        size = static_cast<uint64_t>(std::max(layouts.SizeOf(inst.type), 1u)) *
               slots;
        align = std::max(layouts.AlignOf(inst.type), 1u);
      } else if (ir::ProducesValue(inst.opcode) &&
                 IsAggregate(file_, inst.type)) {
        size = std::max(layouts.SizeOf(inst.type), 1u);
        align = std::max(layouts.AlignOf(inst.type), 1u);
      }
      if (size == 0) {
        continue;
      }
      LLVMValueRef room =
          LLVMBuildArrayAlloca(b_, byte, Const64(size), "room");
      LLVMSetAlignment(room, align);
      // Zeroed, the way the dev tier's prologue zeroes a frame. It
      // matters where a program writes some of an aggregate and hands the
      // whole of it to C: `var address: [16]u8` with two bytes written is
      // a socket address whose other fourteen have to be zero, and a
      // binary that left them as whatever the stack held did not bind
      // (D98). LLVM drops the ones it can prove dead.
      LLVMBuildMemSet(b_, room, Const(0, byte), Const64(size), align);
      storage_[id.index] = room;
    }
  }

  // The function's own parameters are the entry block's.
  const bool indirect = IsAggregate(file_, types_.ElementOf(function.type));
  result_out_ = indirect ? LLVMGetParam(llvm_function_, 0) : nullptr;
  unsigned next = indirect ? 1 : 0;
  for (ir::InstId param : function.ParamsOf(function.entry())) {
    values_[param.index] = LLVMGetParam(llvm_function_, next++);
  }

  // A block parameter is a phi node: the form the IR was given so that
  // this translation would be one line (D98).
  for (size_t i = 1; i < function.blocks.size(); ++i) {
    LLVMPositionBuilderAtEnd(b_, blocks_[i]);
    SetDebugLocation(SourceSpan());
    for (ir::InstId param :
         function.ParamsOf(ir::BlockId{static_cast<uint32_t>(i)})) {
      values_[param.index] =
          LLVMBuildPhi(b_, Held(file_, function.Get(param).type), "p");
    }
  }

  for (size_t i = 0; i < function.blocks.size(); ++i) {
    const ir::Block& block = function.blocks[i];
    LLVMPositionBuilderAtEnd(b_, blocks_[i]);
    for (size_t at = block.param_count; at < block.instructions.size();
         ++at) {
      const ir::InstId id = block.instructions[at];
      SetDebugLocation(function.SpanOf(id));
      Emit(id, function.Get(id));
    }
    // A block the IR left without a terminator cannot fall off the end of
    // a function in LLVM.
    if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(b_)) == nullptr) {
      LLVMBuildUnreachable(b_);
    }
  }
  LLVMSetCurrentDebugLocation2(b_, nullptr);
  scope_ = nullptr;
  return true;
}

// -------------------------------------------------------------- emission ---

void Translator::Emit(ir::InstId id, const ir::Inst& inst) {
  abi::LayoutStore& layouts = Layouts(file_);
  const std::span<const ir::InstId> operands = function_->OperandsOf(inst);
  switch (inst.opcode) {
    case ir::Opcode::kParam:
      return;

    case ir::Opcode::kConstInt:
      Produce(id, Const(inst.payload.integer, Held(file_, inst.type)));
      return;

    case ir::Opcode::kConstBool:
      Produce(id, Const(inst.payload.boolean ? 1 : 0,
                        Held(file_, inst.type)));
      return;

    case ir::Opcode::kConstFloat: {
      LLVMTypeRef type = Held(file_, inst.type);
      Produce(id, LLVMConstReal(LLVMGetTypeKind(type) == LLVMFloatTypeKind
                                    ? LLVMFloatTypeInContext(ctx_)
                                    : LLVMDoubleTypeInContext(ctx_),
                                inst.payload.floating));
      return;
    }

    case ir::Opcode::kConstString: {
      // The bytes live in the object file; the value is the pair that
      // points at them, which is what a `str` is everywhere (D98).
      const std::vector<std::string>& pool = files_[file_].module->strings;
      const std::string& text =
          inst.payload.string < pool.size() ? pool[inst.payload.string]
                                            : std::string();
      const abi::Layout& layout = layouts.Of(inst.type);
      LLVMValueRef room = Storage(id);
      LLVMValueRef bytes =
          LLVMBuildPtrToInt(b_, GlobalText(text), I64(), "s");
      LLVMBuildStore(b_, bytes, AsPointer(room));
      const uint64_t at = layout.offsets.size() > 1 ? layout.offsets[1] : 8;
      LLVMValueRef length = Const64(text.size());
      LLVMValueRef where = Const64(at);
      LLVMBuildStore(
          b_, length,
          LLVMBuildGEP2(b_, LLVMInt8TypeInContext(ctx_), AsPointer(room),
                        &where, 1, "o"));
      Produce(id, LLVMBuildPtrToInt(b_, room, I64(), "a"));
      return;
    }

    case ir::Opcode::kConstNull: {
      if (IsAggregate(file_, inst.type)) {
        LLVMBuildMemSet(b_, AsPointer(Storage(id)),
                        Const(0, LLVMInt8TypeInContext(ctx_)),
                        Const64(std::max(layouts.SizeOf(inst.type), 1u)), 1);
        Produce(id, LLVMBuildPtrToInt(b_, Storage(id), I64(), "a"));
        return;
      }
      Produce(id, Const(0, Held(file_, inst.type)));
      return;
    }

    case ir::Opcode::kUndef:
      Produce(id, IsAggregate(file_, inst.type)
                      ? LLVMBuildPtrToInt(b_, Storage(id), I64(), "a")
                      : LLVMGetUndef(Held(file_, inst.type)));
      return;

    case ir::Opcode::kAlloca:
      Produce(id, LLVMBuildPtrToInt(b_, Storage(id), I64(), "a"));
      return;

    case ir::Opcode::kLocalAddress:
      // Where the slot is. The slot's value already is its address here,
      // which is what `&x` on a whole local needs (D94).
      Produce(id, Widen(Val(operands[0])));
      return;

    case ir::Opcode::kGlobalAddress: {
      // Asked of the runtime rather than baked in, so that a global
      // written by a compiled tier is the one the reference tier reads
      // (D88).
      const TypeId held = types_.ElementOf(inst.type);
      LLVMValueRef call = Declaration("wolang_global_address", I64(),
                                      {I64(), I64()});
      LLVMValueRef arguments[2] = {
          Const64(inst.payload.integer),
          Const64(held.is_valid() ? std::max(layouts.SizeOf(held), 1u)
                                  : 8u)};
      Produce(id, LLVMBuildCall2(b_, LLVMGlobalGetValueType(call), call,
                                 arguments, 2, "g"));
      return;
    }

    case ir::Opcode::kClassTable: {
      const std::vector<ir::ClassInfo>& classes =
          files_[file_].module->classes;
      const uint32_t which = inst.payload.klass;
      Produce(id, Const64(which < classes.size() ? classes[which].token
                                                 : 0));
      return;
    }

    case ir::Opcode::kTypeSize: {
      const TypeId measured{static_cast<uint32_t>(inst.payload.integer)};
      Produce(id, Const(measured.is_valid()
                            ? std::max(layouts.SizeOf(measured), 1u)
                            : 1u,
                        Held(file_, inst.type)));
      return;
    }

    case ir::Opcode::kSoaNew: {
      // Storage for a run of records, as either one array per field or
      // one record after another. How big that is, is this tier's own
      // answer -- which is why it is an operation and not a call lowering
      // wrote (D73).
      const TypeId record = types_.ElementOf(inst.type);
      LLVMValueRef count = Widen(Val(operands[0]));
      LLVMValueRef bytes = nullptr;
      if (types_.Get(inst.type).is_soa) {
        bytes = LLVMBuildMul(
            b_, PaddedCount(count),
            Const64(layouts.SoaPrefixOf(record,
                                        layouts.FieldCount(record))),
            "x");
      } else {
        bytes = LLVMBuildMul(b_, count,
                             Const64(layouts.StrideOf(inst.type)), "x");
      }
      LLVMValueRef allocate =
          Declaration("wolang_object_alloc", I64(), {I64()});
      LLVMValueRef storage = LLVMBuildCall2(
          b_, LLVMGlobalGetValueType(allocate), allocate, &bytes, 1, "s");
      TrapIf(LLVMBuildICmp(b_, LLVMIntEQ, storage, Const64(0), "c"),
             ir::TrapKind::kOutOfMemory, "out of memory");
      // The view: where the storage is and how many records it holds.
      LLVMBuildStore(b_, storage, AsPointer(Storage(id)));
      LLVMValueRef where = Const64(8);
      LLVMBuildStore(
          b_, count,
          LLVMBuildGEP2(b_, LLVMInt8TypeInContext(ctx_),
                        AsPointer(Storage(id)), &where, 1, "o"));
      Produce(id, LLVMBuildPtrToInt(b_, Storage(id), I64(), "a"));
      return;
    }

    case ir::Opcode::kSoaAddress: {
      // The fields before this one, times the padded count, plus the
      // index: one multiply each, which is the whole of what `soa` costs
      // over one array (D73).
      const TypeId base = TypeOf(operands[0]);
      const TypeId record = types_.KindOf(base) == TypeKind::kPointer
                                ? types_.ElementOf(base)
                                : base;
      LLVMValueRef index = Widen(Val(operands[1]));
      LLVMValueRef count = Widen(Val(operands[2]));
      TrapIf(LLVMBuildICmp(b_, LLVMIntUGE, index, count, "c"),
             ir::TrapKind::kOutOfBounds, "index is outside the length");
      const TypeId field = layouts.FieldType(record, inst.field);
      const uint32_t size = field.is_valid() ? layouts.SizeOf(field) : 0;
      LLVMValueRef array = LLVMBuildMul(
          b_, PaddedCount(count),
          Const64(layouts.SoaPrefixOf(record, inst.field)), "x");
      LLVMValueRef inside =
          LLVMBuildMul(b_, index, Const64(size), "x");
      Produce(id, LLVMBuildAdd(
                      b_, Widen(Val(operands[0])),
                      LLVMBuildAdd(b_, array, inside, "o"), "o"));
      return;
    }

    case ir::Opcode::kNew: {
      const TypeId object = types_.KindOf(inst.type) == TypeKind::kPointer
                                ? types_.ElementOf(inst.type)
                                : inst.type;
      LLVMValueRef allocate =
          Declaration("wolang_object_alloc", I64(), {I64()});
      LLVMValueRef size = Const64(layouts.ObjectOf(object).size);
      LLVMValueRef handle = LLVMBuildCall2(
          b_, LLVMGlobalGetValueType(allocate), allocate, &size, 1, "n");
      TrapIf(LLVMBuildICmp(b_, LLVMIntEQ, handle, Const64(0), "c"),
             ir::TrapKind::kOutOfMemory, "out of memory");
      Produce(id, handle);
      return;
    }

    case ir::Opcode::kFuncAddress: {
      const uint32_t which = global_[file_][inst.payload.callee];
      const Entry& target = entries_[entries_[which].link];
      Produce(id, target.function != nullptr
                      ? LLVMBuildPtrToInt(b_, target.function, I64(), "f")
                      : Const64(0));
      return;
    }

    case ir::Opcode::kLoad: {
      LLVMValueRef from = Val(operands[0]);
      if (IsAggregate(file_, inst.type)) {
        Copy(Storage(id), from, std::max(layouts.SizeOf(inst.type), 1u));
        Produce(id, LLVMBuildPtrToInt(b_, Storage(id), I64(), "a"));
        return;
      }
      Produce(id, LoadHeld(from, inst.type, 0));
      return;
    }

    case ir::Opcode::kStore: {
      const TypeId type = TypeOf(operands[1]);
      if (IsAggregate(file_, type)) {
        Copy(Val(operands[0]), Val(operands[1]),
             std::max(layouts.SizeOf(type), 1u));
        return;
      }
      StoreScalar(Val(operands[0]), Val(operands[1]), type);
      return;
    }

    case ir::Opcode::kMakeStruct:
    case ir::Opcode::kMakeArray: {
      const abi::Layout& layout = layouts.Of(inst.type);
      const bool is_array = types_.KindOf(inst.type) == TypeKind::kArray;
      const uint32_t stride = is_array ? layouts.StrideOf(inst.type) : 0;
      uint32_t which = 0;
      for (ir::InstId operand : operands) {
        const uint64_t at =
            is_array ? static_cast<uint64_t>(which) * stride
                     : (which < layout.offsets.size() ? layout.offsets[which]
                                                      : 0);
        LLVMValueRef where = Const64(at);
        LLVMValueRef address =
            LLVMBuildGEP2(b_, LLVMInt8TypeInContext(ctx_),
                          AsPointer(Storage(id)), &where, 1, "o");
        const TypeId type = TypeOf(operand);
        if (IsAggregate(file_, type)) {
          Copy(address, Val(operand), std::max(layouts.SizeOf(type), 1u));
        } else {
          StoreScalar(address, Val(operand), type);
        }
        ++which;
      }
      Produce(id, LLVMBuildPtrToInt(b_, Storage(id), I64(), "a"));
      return;
    }

    case ir::Opcode::kExtract: {
      const abi::Layout& layout = layouts.Of(TypeOf(operands[0]));
      const uint64_t at = inst.field < layout.offsets.size()
                              ? layout.offsets[inst.field]
                              : 0;
      if (IsAggregate(file_, inst.type)) {
        LLVMValueRef where = Const64(at);
        LLVMValueRef address =
            LLVMBuildGEP2(b_, LLVMInt8TypeInContext(ctx_),
                          AsPointer(Val(operands[0])), &where, 1, "o");
        Copy(Storage(id), address, std::max(layouts.SizeOf(inst.type), 1u));
        Produce(id, LLVMBuildPtrToInt(b_, Storage(id), I64(), "a"));
        return;
      }
      Produce(id, LoadHeld(Val(operands[0]), inst.type, at));
      return;
    }

    case ir::Opcode::kInsert: {
      const abi::Layout& layout = layouts.Of(inst.type);
      Copy(Storage(id), Val(operands[0]),
           std::max(layouts.SizeOf(inst.type), 1u));
      const uint64_t at = inst.field < layout.offsets.size()
                              ? layout.offsets[inst.field]
                              : 0;
      LLVMValueRef where = Const64(at);
      LLVMValueRef address =
          LLVMBuildGEP2(b_, LLVMInt8TypeInContext(ctx_),
                        AsPointer(Storage(id)), &where, 1, "o");
      const TypeId type = TypeOf(operands[1]);
      if (IsAggregate(file_, type)) {
        Copy(address, Val(operands[1]), std::max(layouts.SizeOf(type), 1u));
      } else {
        StoreScalar(address, Val(operands[1]), type);
      }
      Produce(id, LLVMBuildPtrToInt(b_, Storage(id), I64(), "a"));
      return;
    }

    case ir::Opcode::kIndex: {
      const TypeId array = TypeOf(operands[0]);
      const uint64_t length = types_.Get(array).length;
      const uint32_t stride = layouts.StrideOf(array);
      LLVMValueRef index = Widen(Val(operands[1]));
      // Unsigned, which catches a negative index as the very large one it
      // reads as -- the same answer the reference tier gives.
      TrapIf(LLVMBuildICmp(b_, LLVMIntUGE, index, Const64(length), "c"),
             ir::TrapKind::kOutOfBounds,
             "index is outside a length of " + std::to_string(length));
      LLVMValueRef at =
          LLVMBuildMul(b_, index, Const64(stride), "x");
      LLVMValueRef address =
          LLVMBuildGEP2(b_, LLVMInt8TypeInContext(ctx_),
                        AsPointer(Val(operands[0])), &at, 1, "o");
      if (IsAggregate(file_, inst.type)) {
        Copy(Storage(id), address, std::max(layouts.SizeOf(inst.type), 1u));
        Produce(id, LLVMBuildPtrToInt(b_, Storage(id), I64(), "a"));
        return;
      }
      Produce(id, LoadHeld(address, inst.type, 0));
      return;
    }

    case ir::Opcode::kFieldAddress: {
      const abi::Layout& layout = layouts.ObjectOf(Addressed(operands[0]));
      const uint64_t at = inst.field < layout.offsets.size()
                              ? layout.offsets[inst.field]
                              : 0;
      Produce(id, LLVMBuildAdd(b_, Widen(Val(operands[0])), Const64(at),
                               "o"));
      return;
    }

    case ir::Opcode::kIndexCheck: {
      LLVMValueRef index = Widen(Val(operands[0]));
      LLVMValueRef length = Widen(Val(operands[1]));
      TrapIf(LLVMBuildICmp(b_, LLVMIntUGE, index, length, "c"),
             ir::TrapKind::kOutOfBounds, "index is outside the length");
      return;
    }

    case ir::Opcode::kIndexAddress: {
      // The stride is the size of what the *result* points at: the base
      // is an array when the index is into a slot and the element itself
      // when it is into a slice.
      const uint32_t stride = layouts.StrideOf(inst.type);
      LLVMValueRef index = Widen(Val(operands[1]));
      TrapIf(LLVMBuildICmp(b_, LLVMIntUGE, index,
                           Widen(Val(operands[2])), "c"),
             ir::TrapKind::kOutOfBounds, "index is outside the length");
      LLVMValueRef at = LLVMBuildMul(b_, index, Const64(stride), "x");
      Produce(id, LLVMBuildAdd(b_, Widen(Val(operands[0])), at, "o"));
      return;
    }

    case ir::Opcode::kMakeVariant: {
      // The tag, then what the variant carries at the offsets the whole
      // enum gave it (D89).
      LLVMBuildStore(b_, Const(inst.field, I(32)),
                     AsPointer(Storage(id)));
      uint32_t position = 0;
      for (ir::InstId operand : operands) {
        const uint64_t at =
            layouts.VariantFieldOffset(inst.type, inst.field, position);
        LLVMValueRef where = Const64(at);
        LLVMValueRef address =
            LLVMBuildGEP2(b_, LLVMInt8TypeInContext(ctx_),
                          AsPointer(Storage(id)), &where, 1, "o");
        const TypeId type = TypeOf(operand);
        if (IsAggregate(file_, type)) {
          Copy(address, Val(operand), std::max(layouts.SizeOf(type), 1u));
        } else {
          StoreScalar(address, Val(operand), type);
        }
        ++position;
      }
      Produce(id, LLVMBuildPtrToInt(b_, Storage(id), I64(), "a"));
      return;
    }

    case ir::Opcode::kVariantPayload: {
      const uint64_t at = layouts.VariantFieldOffset(
          TypeOf(operands[0]), inst.field,
          static_cast<uint32_t>(inst.payload.integer));
      if (IsAggregate(file_, inst.type)) {
        LLVMValueRef where = Const64(at);
        LLVMValueRef address =
            LLVMBuildGEP2(b_, LLVMInt8TypeInContext(ctx_),
                          AsPointer(Val(operands[0])), &where, 1, "o");
        Copy(Storage(id), address, std::max(layouts.SizeOf(inst.type), 1u));
        Produce(id, LLVMBuildPtrToInt(b_, Storage(id), I64(), "a"));
        return;
      }
      Produce(id, LoadHeld(Val(operands[0]), inst.type, at));
      return;
    }

    case ir::Opcode::kOptionalWrap: {
      const abi::Layout& layout = layouts.Of(inst.type);
      LLVMBuildStore(b_, Const(1, LLVMInt8TypeInContext(ctx_)),
                     AsPointer(Storage(id)));
      const uint64_t at = layout.offsets.size() > 1 ? layout.offsets[1] : 0;
      LLVMValueRef where = Const64(at);
      LLVMValueRef address =
          LLVMBuildGEP2(b_, LLVMInt8TypeInContext(ctx_),
                        AsPointer(Storage(id)), &where, 1, "o");
      const TypeId type = TypeOf(operands[0]);
      if (IsAggregate(file_, type)) {
        Copy(address, Val(operands[0]), std::max(layouts.SizeOf(type), 1u));
      } else {
        StoreScalar(address, Val(operands[0]), type);
      }
      Produce(id, LLVMBuildPtrToInt(b_, Storage(id), I64(), "a"));
      return;
    }

    case ir::Opcode::kOptionalIsPresent: {
      LLVMValueRef tag = LLVMBuildLoad2(b_, LLVMInt8TypeInContext(ctx_),
                                        AsPointer(Val(operands[0])), "t");
      Produce(id, Coerce(tag, Held(file_, inst.type)));
      return;
    }

    case ir::Opcode::kOptionalValue: {
      const abi::Layout& layout = layouts.Of(TypeOf(operands[0]));
      LLVMValueRef tag = LLVMBuildLoad2(b_, LLVMInt8TypeInContext(ctx_),
                                        AsPointer(Val(operands[0])), "t");
      TrapIf(LLVMBuildICmp(b_, LLVMIntEQ, tag,
                           Const(0, LLVMInt8TypeInContext(ctx_)), "c"),
             ir::TrapKind::kUndefinedValue,
             "read the value of an optional that is absent");
      const uint64_t at = layout.offsets.size() > 1 ? layout.offsets[1] : 0;
      if (IsAggregate(file_, inst.type)) {
        LLVMValueRef where = Const64(at);
        LLVMValueRef address =
            LLVMBuildGEP2(b_, LLVMInt8TypeInContext(ctx_),
                          AsPointer(Val(operands[0])), &where, 1, "o");
        Copy(Storage(id), address, std::max(layouts.SizeOf(inst.type), 1u));
        Produce(id, LLVMBuildPtrToInt(b_, Storage(id), I64(), "a"));
        return;
      }
      Produce(id, LoadHeld(Val(operands[0]), inst.type, at));
      return;
    }

    case ir::Opcode::kCall:
    case ir::Opcode::kCallIndirect:
    case ir::Opcode::kVirtualCall:
      EmitCall(id, inst);
      return;

    case ir::Opcode::kCast:
      EmitCast(id, inst);
      return;

    case ir::Opcode::kEq:
    case ir::Opcode::kNe:
    case ir::Opcode::kLt:
    case ir::Opcode::kLe:
    case ir::Opcode::kGt:
    case ir::Opcode::kGe:
      EmitCompare(id, inst);
      return;

    case ir::Opcode::kReturn:
    case ir::Opcode::kJump:
    case ir::Opcode::kBranch:
    case ir::Opcode::kSwitch:
    case ir::Opcode::kUnreachable:
      EmitTerminator(inst);
      return;

    default:
      EmitArithmetic(id, inst);
      return;
  }
}

// ------------------------------------------------------- arithmetic ---

void Translator::EmitArithmetic(ir::InstId id, const ir::Inst& inst) {
  abi::LayoutStore& layouts = Layouts(file_);
  const std::span<const ir::InstId> operands = function_->OperandsOf(inst);
  const bool is_float =
      layouts.ClassOf(inst.type) == abi::ValueClass::kFloat;
  const bool is_signed = layouts.IsSignedInteger(inst.type);
  LLVMValueRef lhs = operands.empty() ? Const64(0) : Val(operands[0]);
  LLVMValueRef rhs =
      operands.size() > 1 ? Val(operands[1]) : Const64(0);
  LLVMTypeRef type = Held(file_, inst.type);
  if (!is_float) {
    lhs = Coerce(lhs, type);
    rhs = Coerce(rhs, type);
  }

  switch (inst.opcode) {
    case ir::Opcode::kLogicalNot:
      Produce(id, LLVMBuildXor(b_, lhs, Const(1, type), "l"));
      return;
    case ir::Opcode::kBitNot:
      Produce(id, LLVMBuildNot(b_, lhs, "n"));
      return;
    case ir::Opcode::kBitAnd:
      Produce(id, LLVMBuildAnd(b_, lhs, rhs, "a"));
      return;
    case ir::Opcode::kBitOr:
      Produce(id, LLVMBuildOr(b_, lhs, rhs, "o"));
      return;
    case ir::Opcode::kBitXor:
      Produce(id, LLVMBuildXor(b_, lhs, rhs, "x"));
      return;
    case ir::Opcode::kNeg:
      if (is_float) {
        Produce(id, LLVMBuildFNeg(b_, lhs, "f"));
        return;
      }
      if (!inst.is_unchecked && is_signed) {
        // The one value whose negation is not representable.
        const uint32_t bits = LLVMGetIntTypeWidth(type);
        const uint64_t least = uint64_t{1} << (bits - 1);
        TrapIf(LLVMBuildICmp(b_, LLVMIntEQ, lhs, Const(least, type), "c"),
               ir::TrapKind::kOverflow, "negation overflowed");
      }
      Produce(id, LLVMBuildNeg(b_, lhs, "g"));
      return;

    case ir::Opcode::kShl:
    case ir::Opcode::kShr: {
      // Shifting by the width or more is a trap rather than a number,
      // because the machines disagree about what it would be.
      const uint32_t bits = LLVMGetIntTypeWidth(type);
      if (!inst.is_unchecked) {
        TrapIf(LLVMBuildICmp(b_, LLVMIntUGE, rhs, Const(bits, type), "c"),
               ir::TrapKind::kOverflow, "shifted by more than the width");
      }
      if (inst.opcode == ir::Opcode::kShl) {
        Produce(id, LLVMBuildShl(b_, lhs, rhs, "s"));
        return;
      }
      Produce(id, is_signed ? LLVMBuildAShr(b_, lhs, rhs, "s")
                            : LLVMBuildLShr(b_, lhs, rhs, "s"));
      return;
    }

    case ir::Opcode::kDiv:
    case ir::Opcode::kRem: {
      if (is_float) {
        Produce(id, inst.opcode == ir::Opcode::kDiv
                        ? LLVMBuildFDiv(b_, lhs, rhs, "d")
                        : LLVMBuildFRem(b_, lhs, rhs, "r"));
        return;
      }
      TrapIf(LLVMBuildICmp(b_, LLVMIntEQ, rhs, Const(0, type), "c"),
             ir::TrapKind::kDivisionByZero, "divided by zero");
      if (is_signed) {
        // The one division that overflows: the least value by -1.
        const uint32_t bits = LLVMGetIntTypeWidth(type);
        const uint64_t least = uint64_t{1} << (bits - 1);
        LLVMValueRef is_least =
            LLVMBuildICmp(b_, LLVMIntEQ, lhs, Const(least, type), "c");
        LLVMValueRef minus_one = LLVMBuildICmp(
            b_, LLVMIntEQ, rhs, LLVMConstAllOnes(type), "c");
        TrapIf(LLVMBuildAnd(b_, is_least, minus_one, "c"),
               ir::TrapKind::kOverflow, "the quotient does not fit");
      }
      if (inst.opcode == ir::Opcode::kDiv) {
        Produce(id, is_signed ? LLVMBuildSDiv(b_, lhs, rhs, "d")
                              : LLVMBuildUDiv(b_, lhs, rhs, "d"));
        return;
      }
      Produce(id, is_signed ? LLVMBuildSRem(b_, lhs, rhs, "r")
                            : LLVMBuildURem(b_, lhs, rhs, "r"));
      return;
    }

    case ir::Opcode::kAdd:
    case ir::Opcode::kSub:
    case ir::Opcode::kMul: {
      if (is_float) {
        Produce(id, inst.opcode == ir::Opcode::kAdd
                        ? LLVMBuildFAdd(b_, lhs, rhs, "f")
                        : inst.opcode == ir::Opcode::kSub
                              ? LLVMBuildFSub(b_, lhs, rhs, "f")
                              : LLVMBuildFMul(b_, lhs, rhs, "f"));
        return;
      }
      if (inst.is_unchecked) {
        // Arithmetic the compiler wrote for itself -- an address, a
        // stride, a length -- which is not checked: a trap in it would
        // name a line the program never wrote.
        Produce(id, inst.opcode == ir::Opcode::kAdd
                        ? LLVMBuildAdd(b_, lhs, rhs, "a")
                        : inst.opcode == ir::Opcode::kSub
                              ? LLVMBuildSub(b_, lhs, rhs, "s")
                              : LLVMBuildMul(b_, lhs, rhs, "m"));
        return;
      }
      // Checked, through the intrinsic that gives the answer and whether
      // it fit in one go.
      std::string name = "llvm.";
      name.push_back(is_signed ? 's' : 'u');
      name.append(inst.opcode == ir::Opcode::kAdd
                      ? "add"
                      : inst.opcode == ir::Opcode::kSub ? "sub" : "mul");
      name.append(".with.overflow");
      const unsigned which = LLVMLookupIntrinsicID(
          name.c_str(), static_cast<size_t>(name.size()));
      LLVMTypeRef arguments[1] = {type};
      LLVMValueRef intrinsic =
          LLVMGetIntrinsicDeclaration(mod_, which, arguments, 1);
      LLVMValueRef pair[2] = {lhs, rhs};
      LLVMValueRef answer = LLVMBuildCall2(
          b_, LLVMIntrinsicGetType(ctx_, which, arguments, 1), intrinsic,
          pair, 2, "v");
      TrapIf(LLVMBuildExtractValue(b_, answer, 1, "f"),
             ir::TrapKind::kOverflow,
             std::string(ir::OpcodeMnemonic(inst.opcode)) + " overflowed");
      Produce(id, LLVMBuildExtractValue(b_, answer, 0, "v"));
      return;
    }

    default:
      // Nothing else reaches here: every opcode is either handled above
      // or in `Emit`.
      Produce(id, LLVMGetUndef(type));
      return;
  }
}

void Translator::EmitCompare(ir::InstId id, const ir::Inst& inst) {
  abi::LayoutStore& layouts = Layouts(file_);
  const std::span<const ir::InstId> operands = function_->OperandsOf(inst);
  const TypeId operand_type = TypeOf(operands[0]);
  LLVMValueRef lhs = Val(operands[0]);
  LLVMValueRef rhs = Val(operands[1]);
  LLVMValueRef answer = nullptr;
  if (layouts.ClassOf(operand_type) == abi::ValueClass::kFloat) {
    LLVMRealPredicate how = LLVMRealOEQ;
    switch (inst.opcode) {
      case ir::Opcode::kNe: how = LLVMRealONE; break;
      case ir::Opcode::kLt: how = LLVMRealOLT; break;
      case ir::Opcode::kLe: how = LLVMRealOLE; break;
      case ir::Opcode::kGt: how = LLVMRealOGT; break;
      case ir::Opcode::kGe: how = LLVMRealOGE; break;
      default: break;
    }
    answer = LLVMBuildFCmp(b_, how, lhs, rhs, "c");
  } else {
    const bool is_signed = layouts.IsSignedInteger(operand_type);
    LLVMTypeRef type = Held(file_, operand_type);
    lhs = Coerce(lhs, type);
    rhs = Coerce(rhs, type);
    LLVMIntPredicate how = LLVMIntEQ;
    switch (inst.opcode) {
      case ir::Opcode::kNe: how = LLVMIntNE; break;
      case ir::Opcode::kLt: how = is_signed ? LLVMIntSLT : LLVMIntULT; break;
      case ir::Opcode::kLe: how = is_signed ? LLVMIntSLE : LLVMIntULE; break;
      case ir::Opcode::kGt: how = is_signed ? LLVMIntSGT : LLVMIntUGT; break;
      case ir::Opcode::kGe: how = is_signed ? LLVMIntSGE : LLVMIntUGE; break;
      default: break;
    }
    answer = LLVMBuildICmp(b_, how, lhs, rhs, "c");
  }
  // A `bool` is a byte here, the way it is in memory.
  Produce(id, LLVMBuildZExt(b_, answer, Held(file_, inst.type), "b"));
}

void Translator::EmitCast(ir::InstId id, const ir::Inst& inst) {
  abi::LayoutStore& layouts = Layouts(file_);
  const ir::InstId source = function_->OperandsOf(inst)[0];
  const TypeId from = TypeOf(source);
  const TypeId to = inst.type;
  const bool from_float =
      layouts.ClassOf(from) == abi::ValueClass::kFloat;
  const bool to_float = layouts.ClassOf(to) == abi::ValueClass::kFloat;
  LLVMValueRef value = Val(source);
  LLVMTypeRef target = Held(file_, to);

  if (from_float && to_float) {
    Produce(id, Coerce(value, target));
    return;
  }
  if (!from_float && to_float) {
    value = Coerce(value, Held(file_, from));
    Produce(id, layouts.IsSignedInteger(from)
                    ? LLVMBuildSIToFP(b_, value, target, "f")
                    : LLVMBuildUIToFP(b_, value, target, "f"));
    return;
  }
  if (from_float && !to_float) {
    // Truncation toward zero, with everything the target cannot hold
    // trapping: not a number, and anything past either bound. Checked
    // before converting, because LLVM's conversion of a value out of
    // range is poison rather than a number -- and the saturating
    // intrinsic is what makes the value itself well defined whichever
    // way the check goes.
    LLVMTypeRef wide = LLVMTypeOf(value);
    const uint32_t bits = LLVMGetIntTypeWidth(target);
    const bool is_signed = layouts.IsSignedInteger(to);
    TrapIf(LLVMBuildFCmp(b_, LLVMRealUNO, value, value, "c"),
           ir::TrapKind::kOverflow,
           "the value does not fit the integer it was cast to");
    // Both bounds are powers of two, which every float width holds
    // exactly -- so the comparison is exact at the boundary, which is
    // where this is easy to get wrong.
    const double high = is_signed ? std::ldexp(1.0, bits - 1)
                                  : std::ldexp(1.0, bits);
    TrapIf(LLVMBuildFCmp(b_, LLVMRealOGE, value,
                         LLVMConstReal(wide, high), "c"),
           ir::TrapKind::kOverflow,
           "the value does not fit the integer it was cast to");
    // Truncation is why the unsigned bound is minus one rather than
    // zero: `-0.5 as u32` is zero, not a fault.
    const double low = is_signed ? -std::ldexp(1.0, bits - 1) : -1.0;
    TrapIf(LLVMBuildFCmp(b_, is_signed ? LLVMRealOLT : LLVMRealOLE, value,
                         LLVMConstReal(wide, low), "c"),
           ir::TrapKind::kOverflow,
           "the value does not fit the integer it was cast to");
    const char* name = is_signed ? "llvm.fptosi.sat" : "llvm.fptoui.sat";
    const unsigned which = LLVMLookupIntrinsicID(name, std::strlen(name));
    LLVMTypeRef overloads[2] = {target, wide};
    Produce(id, LLVMBuildCall2(
                    b_, LLVMIntrinsicGetType(ctx_, which, overloads, 2),
                    LLVMGetIntrinsicDeclaration(mod_, which, overloads, 2),
                    &value, 1, "i"));
    return;
  }

  if (types_.KindOf(from) == TypeKind::kPointer &&
      types_.KindOf(to) == TypeKind::kPointer) {
    Produce(id, Widen(value));
    return;
  }

  // Integer to integer. One that cannot hold the value traps rather than
  // keeping the low bits: `as` is a conversion, and overflow traps
  // everywhere else in this language.
  LLVMTypeRef wide = I(64);
  LLVMTypeRef narrow = Held(file_, to);
  LLVMValueRef extended =
      layouts.IsSignedInteger(from)
          ? LLVMBuildSExtOrBitCast(b_, Coerce(value, Held(file_, from)),
                                   wide, "e")
          : LLVMBuildZExtOrBitCast(b_, Coerce(value, Held(file_, from)),
                                   wide, "e");
  LLVMValueRef narrowed = LLVMBuildTrunc(b_, extended, narrow, "n");
  LLVMValueRef again = layouts.IsSignedInteger(to)
                           ? LLVMBuildSExtOrBitCast(b_, narrowed, wide, "e")
                           : LLVMBuildZExtOrBitCast(b_, narrowed, wide, "e");
  if (LLVMGetIntTypeWidth(narrow) < 64) {
    TrapIf(LLVMBuildICmp(b_, LLVMIntNE, again, extended, "c"),
           ir::TrapKind::kOverflow,
           "the value does not fit the integer it was cast to");
  } else if (layouts.IsSignedInteger(from) != layouts.IsSignedInteger(to)) {
    // Other signedness at 64 bits: fits exactly when not negative (D226).
    TrapIf(LLVMBuildICmp(b_, LLVMIntSLT, extended,
                         LLVMConstInt(wide, 0, 0), "c"),
           ir::TrapKind::kOverflow,
           "the value does not fit the integer it was cast to");
  }
  Produce(id, narrowed);
}

// ------------------------------------------------------------------ calls ---

void Translator::EmitCall(ir::InstId id, const ir::Inst& inst) {
  const std::span<const ir::InstId> operands = function_->OperandsOf(inst);
  const bool indirect_result = IsAggregate(file_, inst.type);

  // Where the arguments start, and what is being called.
  LLVMValueRef callee = nullptr;
  LLVMTypeRef signature = nullptr;
  size_t first = 0;
  if (inst.opcode == ir::Opcode::kCall) {
    const uint32_t which = global_[file_][inst.payload.callee];
    const Entry& target = entries_[entries_[which].link];
    if (target.function == nullptr) {
      // A declaration whose body is nowhere. A trap rather than a jump
      // into nothing, which is what this was before the message existed.
      TrapIf(Const(1, LLVMInt1TypeInContext(ctx_)), ir::TrapKind::kNoBody,
             "called a function with no body");
      if (inst.type.is_valid()) {
        Produce(id, indirect_result
                        ? LLVMBuildPtrToInt(b_, Storage(id), I64(), "a")
                        : LLVMGetUndef(Held(file_, inst.type)));
      }
      return;
    }
    callee = target.function;
    signature = LLVMGlobalGetValueType(callee);
  } else if (inst.opcode == ir::Opcode::kCallIndirect) {
    callee = AsPointer(Val(operands[0]));
    first = 1;
  } else {
    // A virtual call: the body is asked for by the token the receiver
    // carries and the method's selector, which is the same number in
    // every tier (D85).
    LLVMValueRef lookup =
        Declaration("wolang_class_lookup", I64(), {I64(), I64()});
    LLVMValueRef asked[2] = {Widen(Val(operands[0])),
                             Const64(inst.payload.integer)};
    LLVMValueRef code = LLVMBuildCall2(
        b_, LLVMGlobalGetValueType(lookup), lookup, asked, 2, "m");
    TrapIf(LLVMBuildICmp(b_, LLVMIntEQ, code, Const64(0), "c"),
           ir::TrapKind::kNoBody,
           "a virtual call found no body for its method");
    callee = AsPointer(code);
    first = 1;
  }

  std::vector<LLVMValueRef> arguments;
  if (indirect_result) {
    arguments.push_back(LLVMBuildPtrToInt(b_, Storage(id), I64(), "a"));
  }
  for (size_t i = first; i < operands.size(); ++i) {
    arguments.push_back(Widen(Val(operands[i])));
  }

  if (signature != nullptr) {
    // The callee's own shape, with every argument put into the type it
    // declares: the two were derived from the same function type, so this
    // only ever narrows a word back to the width it came from.
    const unsigned wanted = LLVMCountParamTypes(signature);
    std::vector<LLVMTypeRef> parameters(wanted, nullptr);
    if (wanted != 0) {
      LLVMGetParamTypes(signature, parameters.data());
    }
    if (wanted == arguments.size()) {
      for (unsigned i = 0; i < wanted; ++i) {
        arguments[i] = Coerce(arguments[i], parameters[i]);
      }
    } else {
      // The shapes disagree, which is the compiler disagreeing with
      // itself. Said out loud rather than passed to LLVM, which would
      // crash on it.
      signature = nullptr;
    }
  }
  if (signature == nullptr) {
    std::vector<LLVMTypeRef> parameters;
    parameters.reserve(arguments.size());
    for (LLVMValueRef argument : arguments) {
      parameters.push_back(LLVMTypeOf(argument));
    }
    signature = LLVMFunctionType(
        indirect_result || !inst.type.is_valid()
            ? LLVMVoidTypeInContext(ctx_)
            : Held(file_, inst.type),
        parameters.data(), static_cast<unsigned>(parameters.size()),
        /*IsVarArg=*/0);
  }

  LLVMValueRef answer =
      LLVMBuildCall2(b_, signature, callee, arguments.data(),
                     static_cast<unsigned>(arguments.size()), "");
  if (!inst.type.is_valid()) {
    return;
  }
  if (indirect_result) {
    Produce(id, LLVMBuildPtrToInt(b_, Storage(id), I64(), "a"));
    return;
  }
  Produce(id, Coerce(answer, Held(file_, inst.type)));
}

// ------------------------------------------------------------ terminators ---

void Translator::EmitJumpArguments(const ir::Inst& inst,
                                   size_t which,
                                   ir::BlockId target,
                                   size_t first_operand) {
  const std::span<const ir::InstId> operands = function_->OperandsOf(inst);
  const std::span<const ir::InstId> parameters = function_->ParamsOf(target);
  LLVMBasicBlockRef from = LLVMGetInsertBlock(b_);
  for (size_t i = 0; i < parameters.size(); ++i) {
    const size_t at = first_operand + i;
    LLVMValueRef phi = Val(parameters[i]);
    if (phi == nullptr || LLVMGetInstructionOpcode(phi) != LLVMPHI) {
      continue;
    }
    LLVMValueRef value =
        at < operands.size()
            ? Coerce(Val(operands[at]), LLVMTypeOf(phi))
            : LLVMGetUndef(LLVMTypeOf(phi));
    LLVMAddIncoming(phi, &value, &from, 1);
  }
  (void)which;
}

void Translator::EmitTerminator(const ir::Inst& inst) {
  const std::span<const ir::InstId> operands = function_->OperandsOf(inst);
  const std::span<const ir::BlockId> successors =
      function_->SuccessorsOf(inst);
  switch (inst.opcode) {
    case ir::Opcode::kReturn: {
      if (operands.empty()) {
        LLVMBuildRetVoid(b_);
        return;
      }
      const TypeId type = TypeOf(operands[0]);
      if (IsAggregate(file_, type)) {
        // Written through the pointer the caller passed, which is what
        // the platform ABI does with an aggregate anyway.
        Copy(result_out_ != nullptr ? result_out_ : Const64(0),
             Val(operands[0]),
             std::max(Layouts(file_).SizeOf(type), 1u));
        LLVMBuildRetVoid(b_);
        return;
      }
      LLVMTypeRef wanted =
          LLVMGetReturnType(LLVMGlobalGetValueType(llvm_function_));
      if (LLVMGetTypeKind(wanted) == LLVMVoidTypeKind) {
        LLVMBuildRetVoid(b_);
        return;
      }
      LLVMBuildRet(b_, Coerce(Val(operands[0]), wanted));
      return;
    }

    case ir::Opcode::kJump: {
      if (successors.empty()) {
        LLVMBuildUnreachable(b_);
        return;
      }
      EmitJumpArguments(inst, 0, successors[0], 0);
      LLVMBuildBr(b_, blocks_[successors[0].index]);
      return;
    }

    case ir::Opcode::kBranch: {
      if (successors.size() < 2) {
        LLVMBuildUnreachable(b_);
        return;
      }
      EmitJumpArguments(inst, 0, successors[0], 1);
      EmitJumpArguments(inst, 1, successors[1], 1);
      LLVMValueRef condition = Val(operands[0]);
      LLVMBuildCondBr(
          b_,
          LLVMBuildICmp(b_, LLVMIntNE, Widen(condition), Const64(0), "c"),
          blocks_[successors[0].index], blocks_[successors[1].index]);
      return;
    }

    case ir::Opcode::kSwitch: {
      if (successors.empty()) {
        LLVMBuildUnreachable(b_);
        return;
      }
      // The default is last, and the cases are parallel to the rest.
      const ir::BlockId fallback = successors.back();
      for (ir::BlockId target : successors) {
        EmitJumpArguments(inst, 0, target, 1);
      }
      LLVMValueRef subject = Widen(Val(operands[0]));
      LLVMValueRef table = LLVMBuildSwitch(
          b_, subject, blocks_[fallback.index],
          static_cast<unsigned>(successors.size() - 1));
      const std::span<const uint64_t> cases = function_->CasesOf(inst);
      for (size_t i = 0; i + 1 < successors.size(); ++i) {
        LLVMAddCase(table,
                    Const64(i < cases.size() ? cases[i] : 0),
                    blocks_[successors[i].index]);
      }
      return;
    }

    default:
      LLVMBuildUnreachable(b_);
      return;
  }
}

// ------------------------------------------------------------- the driver ---

AotResult Translator::Run(const AotOptions& options) {
  AotResult result;
  static const bool sm_Parsed = [] {
    const char* given = std::getenv("WOLANG_LLVM_ARGS");
    if (given == nullptr || given[0] == 0) {
      return false;
    }

    static std::vector<std::string> words;
    words.push_back("wolang");
    std::string word;
    for (const char* c = given; ; ++c) {
      if (*c == ' ' || *c == 0) {
        if (!word.empty()) {
          words.push_back(word);
          word.clear();
        }
        if (*c == 0) {
          break;
        }
        continue;
      }
      word.push_back(*c);
    }

    static std::vector<const char*> pointers;
    for (const std::string& held : words) {
      pointers.push_back(held.c_str());
    }
    LLVMParseCommandLineOptions(static_cast<int>(pointers.size()), pointers.data(), "wolang");
    return true;
  }();
  (void)sm_Parsed;
  // Every target this build has, so that `--target` can name one (D160).
  // When the build asked for this machine only, that is what there is --
  // and a triple for anything else then says so rather than producing a
  // binary for the wrong machine.
#if defined(WOLANG_ALL_TARGETS)
  LLVMInitializeAllTargetInfos();
  LLVMInitializeAllTargets();
  LLVMInitializeAllTargetMCs();
  LLVMInitializeAllAsmPrinters();
  LLVMInitializeAllAsmParsers();
#else
  LLVMInitializeNativeTarget();
  LLVMInitializeNativeAsmPrinter();
#endif

  char* host = LLVMGetDefaultTargetTriple();
  const std::string triple =
      options.triple.empty() ? std::string(host) : options.triple;
  LLVMDisposeMessage(host);

  char* trouble = nullptr;
  LLVMTargetRef target = nullptr;
  if (LLVMGetTargetFromTriple(triple.c_str(), &target, &trouble) != 0) {
    result.error = trouble != nullptr ? trouble : "no target for the triple";
    if (trouble != nullptr) {
      LLVMDisposeMessage(trouble);
    }
    return result;
  }
  char* cpu = LLVMGetHostCPUName();
  char* features = LLVMGetHostCPUFeatures();
  const bool native = options.triple.empty();
  LLVMTargetMachineRef machine = LLVMCreateTargetMachine(
      target, triple.c_str(), native ? cpu : "generic",
      native ? features : "",
      options.optimization >= 3 ? LLVMCodeGenLevelAggressive
                                : options.optimization >= 2
                                      ? LLVMCodeGenLevelDefault
                                      : options.optimization >= 1
                                            ? LLVMCodeGenLevelLess
                                            : LLVMCodeGenLevelNone,
      LLVMRelocPIC, LLVMCodeModelDefault);
  LLVMDisposeMessage(cpu);
  LLVMDisposeMessage(features);

  ctx_ = LLVMContextCreate();
  mod_ = LLVMModuleCreateWithNameInContext("wolang", ctx_);
  b_ = LLVMCreateBuilderInContext(ctx_);
  LLVMSetTarget(mod_, triple.c_str());
  LLVMTargetDataRef data = LLVMCreateTargetDataLayout(machine);
  LLVMSetModuleDataLayout(mod_, data);

  if (sources_ != nullptr) {
    debug_ = LLVMCreateDIBuilder(mod_);
    // The unit is named after the first file: a Wolang package is many
    // files and one module, and DWARF wants one unit per object.
    const LLVMMetadataRef first = DebugFileOf(
        files_.empty() ? FileId{} : FirstFile());
    unit_ = LLVMDIBuilderCreateCompileUnit(
        debug_, LLVMDWARFSourceLanguageC99, first, "wolang", 6,
        /*isOptimized=*/options.optimization > 0, "", 0, 0, "", 0,
        LLVMDWARFEmissionFull, 0, /*SplitDebugInlining=*/0,
        /*DebugInfoForProfiling=*/0, "", 0, "", 0);
    // Which DWARF version the object says it is. Without this the
    // debugger reads the line table and finds nothing to read it with.
    LLVMAddModuleFlag(
        mod_, LLVMModuleFlagBehaviorWarning, "Debug Info Version", 18,
        LLVMValueAsMetadata(LLVMConstInt(I(32), 3, 0)));
    LLVMAddModuleFlag(
        mod_, LLVMModuleFlagBehaviorWarning, "Dwarf Version", 13,
        LLVMValueAsMetadata(LLVMConstInt(I(32), 4, 0)));
  }

  Declare();
  for (Entry& entry : entries_) {
    entry.skipped = Unsupported(entry);
    if (entry.skipped.empty()) {
      continue;
    }
    // Left out, and said so. A call to it traps with a message rather
    // than jumping into nothing, and the driver prints the list -- a
    // binary with a missing body in it is incomplete, and which body is
    // what makes that worth fixing.
    if (entry.function != nullptr) {
      LLVMDeleteFunction(entry.function);
      entry.function = nullptr;
    }
    std::string note(interner_.Text(FunctionOf(entry).name));
    note.append(": ");
    note.append(entry.skipped);
    result.skipped.push_back(std::move(note));
  }
  for (Entry& entry : entries_) {
    Translate(&entry);
  }
  library_ = options.library;
  EmitEntryPoint();
  if (!library_.empty()) {
    EmitExports(options.exported);
  }

  if (debug_ != nullptr) {
    // Before the verifier: unfinished metadata is what it would complain
    // about, and the passes read the line table as well.
    LLVMDIBuilderFinalize(debug_);
  }
  char* complaint = nullptr;
  if (LLVMVerifyModule(mod_, LLVMReturnStatusAction, &complaint) != 0) {
    result.error = "the generated module did not verify: ";
    result.error.append(complaint != nullptr ? complaint : "");
  }
  if (complaint != nullptr) {
    LLVMDisposeMessage(complaint);
  }

  if (const char* dump = std::getenv("WOLANG_AOT_IR"); dump != nullptr && dump[0] != 0) {
    char* failed = nullptr;
    LLVMPrintModuleToFile(mod_, dump, &failed);
    if (failed != nullptr) {
      LLVMDisposeMessage(failed);
    }
  }

  if (result.error.empty() && options.optimization > 0) {
    std::string pipeline = "default<O";
    pipeline.push_back(static_cast<char>('0' + options.optimization));
    pipeline.push_back('>');
    if (const char* given = std::getenv("WOLANG_AOT_PASSES"); given != nullptr && given[0] != 0) {
      pipeline = given;
    }
    LLVMPassBuilderOptionsRef passes = LLVMCreatePassBuilderOptions();
    if (LLVMErrorRef failed =
            LLVMRunPasses(mod_, pipeline.c_str(), machine, passes)) {
      char* said = LLVMGetErrorMessage(failed);
      result.error = said != nullptr ? said : "the optimizer failed";
      LLVMDisposeErrorMessage(said);
    }
    LLVMDisposePassBuilderOptions(passes);
    if (const char* dump = std::getenv("WOLANG_AOT_IR"); dump != nullptr && dump[0] != 0) {
      char* failed = nullptr;
      LLVMPrintModuleToFile(mod_, (std::string(dump) + ".opt").c_str(), &failed);
      if (failed != nullptr) {
        LLVMDisposeMessage(failed);
      }
    }
  }

  if (result.error.empty()) {
    char* failed = nullptr;
    std::string path = options.object_path;
    if (LLVMTargetMachineEmitToFile(machine, mod_, path.c_str(),
                                    LLVMObjectFile, &failed) != 0) {
      result.error = failed != nullptr ? failed : "could not write the object";
    }
    if (failed != nullptr) {
      LLVMDisposeMessage(failed);
    }
  }

  if (debug_ != nullptr) {
    LLVMDIBuilderFinalize(debug_);
    LLVMDisposeDIBuilder(debug_);
    debug_ = nullptr;
  }
  LLVMDisposeTargetData(data);
  LLVMDisposeBuilder(b_);
  LLVMDisposeModule(mod_);
  LLVMContextDispose(ctx_);
  LLVMDisposeTargetMachine(machine);
  return result;
}

// What starts the program.
//
// A binary has to do for itself what the host does when `wolang run`
// starts a program: tell the runtime which body implements which method
// of which class, give the module-level `var`s their first values, and
// only then call `main` (D98).
// Every function the |exported| files offer, under a name a host can
// declare in C: `actors_Tick` for `Tick` in a library called `actors`. An
// alias, so the function itself keeps the name the rest of the module
// calls it by.
void Translator::EmitExports(const std::vector<uint32_t>& exported) {
  for (const Entry& entry : entries_) {
    const ir::Function& function = FunctionOf(entry);
    if (entry.function == nullptr || function.is_declaration() || std::find(exported.begin(), exported.end(), function.source_file) == exported.end()) {
      continue;
    }
    const std::string_view name = interner_.Text(function.name);
    if (name.find_first_of(".$<") != std::string_view::npos) {
      continue;
    }
    const Binding* binding = files_[entry.file].resolution->Find(function.name);
    if (binding == nullptr || !binding->is_public) {
      continue;
    }
    const std::string exported_name = library_ + "_" + std::string(name);
    LLVMAddAlias2(mod_, LLVMGlobalGetValueType(entry.function), 0, entry.function, exported_name.c_str());
  }
}

void Translator::EmitEntryPoint() {
  LLVMTypeRef i32 = I(32);
  const std::string start_name = library_.empty() ? std::string("wo$start") : library_ + "_start";
  LLVMValueRef start = LLVMAddFunction(
      mod_, start_name.c_str(),
      LLVMFunctionType(LLVMVoidTypeInContext(ctx_), nullptr, 0, 0));
  LLVMBasicBlockRef body =
      LLVMAppendBasicBlockInContext(ctx_, start, "entry");
  LLVMPositionBuilderAtEnd(b_, body);

  LLVMValueRef register_class =
      Declaration("wolang_class_register", LLVMVoidTypeInContext(ctx_),
                  {I64(), I64(), Ptr()});
  for (uint32_t f = 0; f < files_.size(); ++f) {
    for (const ir::ClassInfo& klass : files_[f].module->classes) {
      for (const ir::ClassInfo::Method& method : klass.methods) {
        if (method.function >= global_[f].size()) {
          continue;
        }
        const Entry& target =
            entries_[entries_[global_[f][method.function]].link];
        if (target.function == nullptr) {
          continue;
        }
        LLVMValueRef arguments[3] = {Const64(klass.token),
                                     Const64(method.selector),
                                     target.function};
        LLVMBuildCall2(b_, LLVMGlobalGetValueType(register_class),
                       register_class, arguments, 3, "");
      }
    }
  }

  // The initializers, then `main`. In file order, which is the order the
  // reference tier runs them in.
  for (uint32_t f = 0; f < files_.size(); ++f) {
    const uint32_t which = files_[f].module->initializer;
    if (which == 0xFFFFFFFFu || which >= global_[f].size()) {
      continue;
    }
    const Entry& target = entries_[entries_[global_[f][which]].link];
    if (target.function == nullptr) {
      continue;
    }
    LLVMBuildCall2(b_, LLVMGlobalGetValueType(target.function),
                   target.function, nullptr, 0, "");
  }

  const Symbol entry_name = interner_.Lookup("main");
  const Entry* program = nullptr;
  for (const Entry& entry : entries_) {
    const ir::Function& function = FunctionOf(entry);
    if (entry.function != nullptr && !function.is_declaration() &&
        function.name == entry_name) {
      program = &entry;
      break;
    }
  }
  if (program != nullptr && library_.empty()) {
    LLVMBuildCall2(b_, LLVMGlobalGetValueType(program->function),
                   program->function, nullptr, 0, "");
  }
  LLVMBuildRetVoid(b_);
  // A library's host has a `main` of its own.
  if (!library_.empty()) {
    return;
  }

  // The C entry point, which is what the linker is looking for.
  LLVMTypeRef parameters[2] = {i32, Ptr()};
  LLVMValueRef main =
      LLVMAddFunction(mod_, "main", LLVMFunctionType(i32, parameters, 2, 0));
  LLVMBasicBlockRef shell =
      LLVMAppendBasicBlockInContext(ctx_, main, "entry");
  LLVMPositionBuilderAtEnd(b_, shell);
  // The arguments, handed to the runtime before anything runs: a program
  // that reads them asks the runtime, and the entry point is the one
  // place that has them (D115).
  LLVMValueRef remember =
      Declaration("wolang_args_set", LLVMVoidTypeInContext(ctx_),
                  {i32, Ptr()});
  LLVMValueRef given[2] = {LLVMGetParam(main, 0), LLVMGetParam(main, 1)};
  LLVMBuildCall2(b_, LLVMGlobalGetValueType(remember), remember, given, 2,
                 "");
  LLVMBuildCall2(b_, LLVMGlobalGetValueType(start), start, nullptr, 0, "");
  LLVMBuildRet(b_, Const(0, i32));
}

}  // namespace

AotResult EmitObject(const StringInterner& interner,
                     const TypeStore& types,
                     std::span<const FileCode> files,
                     const AotOptions& options) {
  Translator translator(interner, types, options.sources, files);
  return translator.Run(options);
}

#endif  // WOLANG_WITH_LLVM

}  // namespace codegen
}  // namespace wolang
