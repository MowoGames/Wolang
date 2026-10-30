// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.
//
// The wolang command line driver. Only the commands that the current wave
// supports are implemented; see docs/roadmap.md for what each wave adds.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// What only one system has, each asked for where it is. `ucontext` is
// where a signal handler finds the program counter, and on macOS it
// wants `_XOPEN_SOURCE` defined before it is asked for (D139).
#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <process.h>
#else
#if !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 700
#endif
#include <sys/time.h>
#include <ucontext.h>
#include <unistd.h>
#endif


#include "wolang/base/shell.h"
#include "wolang/base/diagnostic.h"
#include "wolang/bind/bind.h"
#include "wolang/bind/reflection.h"
#include "wolang/base/diagnostic_renderer.h"
#include "wolang/base/source_manager.h"
#include "wolang/base/string_interner.h"
#include "wolang/compiler/cache.h"
#include "wolang/compiler/database.h"

#include "wolang/codegen/aot.h"
#include "wolang/debug/dap.h"
#include "wolang/lsp/server.h"
#include "wolang/runtime/builtins.h"
#include "wolang/runtime/env.h"
#include "wolang/runtime/library.h"
#include "wolang/runtime/journal.h"
#include "wolang/codegen/compiler.h"
#include "wolang/abi/layout.h"
#include "wolang/codegen/foreign.h"
#include "wolang/fuzz/generator.h"
#include "wolang/runtime/scheduler.h"
#include "wolang/compiler/workspace.h"
#include "wolang/embed/wolang.hpp"
#include "wolang/format/formatter.h"
#include "wolang/interp/interpreter.h"
#include "wolang/ir/printer.h"
#include "wolang/syntax/ast.h"
#include "wolang/syntax/ast_printer.h"
#include "wolang/syntax/lexer.h"
#include "wolang/syntax/parser.h"

namespace wolang {
namespace {

// From project(VERSION) in CMakeLists.txt, the one place it is written (D248).
constexpr std::string_view kVersion = WOLANG_VERSION_STRING;

struct Options {
  std::string command;
  std::vector<std::string> inputs;
  bool color = true;
  // `run` only: stay on the interpreter even where the dev tier could take
  // the program. Useful for telling a code generator bug from a program
  // bug, which is otherwise the hardest thing to tell apart.
  bool interpret = false;
  bool show_trivia = false;
  // `fmt` only: read the file from standard input and write the result to
  // standard output, which is what an editor that formats a buffer it has
  // not saved needs.
  bool from_stdin = false;
  // `bind` only: which declarations to keep, and where the headers are.
  std::string bind_prefix;
  // `bind --reflection`: the inputs are an engine's reflection JSON, and
  // the C++ glue goes to |bind_glue| (D218).
  bool bind_reflection = false;
  // `build --library actors`: an object a host links, with no `main` and
  // the package's offered functions under `actors_<Name>` (D219).
  std::string library;
  std::string bind_glue;
  std::string bind_module = "mowo";
  std::string bind_effect;
  std::vector<std::string> bind_includes;
  // `new` only: a library has no `main` and no `run`.
  bool new_library = false;
  // `test` only: run the dependencies' tests as well.
  bool test_all = false;
  // `asm` only: the bytes themselves, as hex, so that a disassembler can
  // be pointed at what the dev tier actually emitted. What found D195.
  bool raw = false;
  // `run` only: sample where the time goes, and how often to sample.
  bool profile = false;
  int hertz = 1000;
  // `run` only: write down what the machine answered, or hand back what
  // a previous run wrote (D142).
  std::string record;
  std::string replay;
  // How this binary was invoked, which is how `new` finds the standard
  // library next to an installed compiler.
  std::string program;
  bool show_spans = false;
  // `fuzz` only: also build each program into a binary and run it, which
  // is the only way to compare the ahead-of-time tier (D98). Slower by
  // the cost of a link per program, so it is asked for rather than
  // assumed.
  bool fuzz_aot = false;
  // `fuzz` only: which program to start from, how many, and whether to
  // print what was generated -- which is the first thing wanted when one
  // of them disagrees.
  uint64_t fuzz_seed = 1;
  uint64_t fuzz_count = 100;
  bool show_source = false;
  std::string test_filter;
  // `ir` only: print the IR as lowering left it rather than after the
  // passes.
  bool unoptimized = false;
  // Smaller programs are the ones a disagreement can be read in.
  int fuzz_depth = 3;
  int fuzz_statements = 5;
  // `fmt` only: verify instead of printing, or rewrite the file in place.
  bool check_only = false;
  bool write_in_place = false;
  // Skip the on-disk cache entirely, which is how a suspected cache problem
  // is ruled out.
  bool no_cache = false;
  // `build` only: where the binary goes, whether to stop at the object
  // file, how hard the back end works, and whether to leave out the line
  // information a debugger reads.
  std::string output;
  bool emit_object = false;
  // What machine the binary is for, as an LLVM triple. Empty is this
  // one (D160).
  std::string target;
  bool no_debug = false;
  int optimization = 2;
};

bool StderrIsTerminal() {
#if defined(_WIN32)
  return _isatty(_fileno(stderr)) != 0;
#else
  return isatty(fileno(stderr)) != 0;
#endif
}

int32_t ProcessId() {
#if defined(_WIN32)
  return _getpid();
#else
  return static_cast<int32_t>(::getpid());
#endif
}

void PrintUsage() {
  std::fprintf(
      stdout,
      "wolang %s - the Wolang compiler driver\n"
      "\n"
      "usage: wolang <command> [options] [paths...]\n"
      "\n"
      "commands:\n"
      "  check <paths>   report diagnostics for files or package directories\n"
      "  fmt <files>     format files; prints to stdout by default\n"
      "  fix <paths>     apply what the diagnostics suggest\n"
      "  tokens <files>  print the token stream of the given files\n"
      "  ast <files>     print the syntax tree of the given files\n"
      "  ir <files>      print the intermediate representation\n"
      "  asm <files>     print what the dev tier compiled, and what it did "
      "not\n"
      "  run <paths>     run `main`\n"
      "  build <paths>   compile to a binary that needs no compiler\n"
      "  bench <paths>   run and time every function marked `@Bench`\n"
      "  test <paths>    run every function marked `@Test`\n"
      "  modules <dir>   print the module graph of a package\n"
      "  doc <dir>       write what a package offers, as Markdown\n"
      "  bind <header>   write Wolang declarations for a C header\n"
      "  new <dir>       start a package: a manifest, a source file, a "
      "test\n"
      "  fetch <dir>     get the dependencies and write wolang.lock\n"
      "  debug <dir>     build with line information and start a "
      "debugger\n"
      "  repl            a session: type an expression and see it\n"
      "  lsp             speak the Language Server Protocol on stdin\n"
      "  dap             speak the Debug Adapter Protocol on stdin\n"
      "  systems <dir>  print what systems access and when they run\n"
      "  fuzz            generate programs and compare the tiers\n"
      "  version         print the version and exit\n"
      "  help            print this message\n"
      "\n"
      "options:\n"
      "  --seed <n>      the first seed `fuzz` generates from\n"
      "  --count <n>     how many programs `fuzz` generates\n"
      "  --source        print the programs `fuzz` generated\n"
      "                  (`fuzz <file>` compares that program instead)\n"
      "  --aot           `fuzz`: build and run each program as a binary "
      "too\n"
      "  --unoptimized   `ir`: print what lowering produced, before passes\n"
      "  -o <path>       `build`: where the binary goes\n"
      "  --emit-object   `build`: stop at the object file\n"
      "  --no-debug      `build`: leave out the line information\n"
      "  -O<n>           `build`: how hard the back end optimizes, 0 to 3\n"
      "  --no-color      never emit ANSI colors\n"
      "  --trivia        include comments in `tokens` output\n"
      "  --spans         annotate `ast` output with positions\n"
      "  --check         `fmt` only: fail if a file is not formatted\n"
      "  --write         `fmt` and `fix`: change the files in place\n"
      "  --no-cache      ignore and do not write the on-disk cache\n"
      "  --interpret     `run` only: do not use the compiled tier\n"
      "  --stdin         `fmt` only: read the file from standard input\n"
      "  --prefix <p>    `bind` only: keep declarations starting with p\n"
      "  -I <dir>        `bind` only: where the headers are\n"
      "  --lib           `new` only: a library rather than a program\n"
      "  --all           `test` only: the dependencies' tests too\n"
      "  --profile       `run` only: sample where the time goes\n"
      "  --hertz <n>     how often `--profile` samples, default 1000\n"
      "  --record <file> `run`: write down the clock and the input\n"
      "  --replay <file> `run`: hand back what a recording wrote\n",
      std::string(kVersion).c_str());
}

// fputs stops at the first NUL; text that carries user bytes goes out by
// length instead (D230).
void WriteAll(const std::string& text, std::FILE* stream) {
  std::fwrite(text.data(), 1, text.size(), stream);
}

int Report(const SourceManager& sources,
           const std::vector<Diagnostic>& diagnostics,
           const RenderOptions& options) {
  if (!diagnostics.empty()) {
    const std::string text = RenderDiagnostics(sources, diagnostics, options);
    WriteAll(text, stderr);
  }

  size_t errors = 0;
  size_t warnings = 0;
  for (const Diagnostic& diagnostic : diagnostics) {
    if (diagnostic.severity == Severity::kError) {
      ++errors;
    } else if (diagnostic.severity == Severity::kWarning) {
      ++warnings;
    }
  }
  if (!diagnostics.empty()) {
    std::fprintf(stderr, "\n%zu error%s, %zu warning%s\n", errors,
                 errors == 1 ? "" : "s", warnings,
                 warnings == 1 ? "" : "s");
  }
  return errors == 0 ? 0 : 1;
}

// Adds every input to the database, loading package directories as packages.
bool CollectInputs(Database* database,
                   const Options& options,
                   DiagnosticEngine* load_diagnostics,
                   std::vector<FileId>* files) {
  for (const std::string& input : options.inputs) {
    if (IsPackageDirectory(input)) {
      if (!LoadPackage(database, input, load_diagnostics)) {
        return false;
      }
      // Every module the database now holds, including the ones a
      // previous argument brought in: a shared dependency is loaded once
      // and its files belong to one list, so they are added once too.
      for (const Module& module : database->modules()) {
        for (FileId file : module.files) {
          if (std::find(files->begin(), files->end(), file) ==
              files->end()) {
            files->push_back(file);
          }
        }
      }
      continue;
    }

    std::vector<std::string> paths;
    std::error_code failed;
    if (std::filesystem::is_directory(input, failed)) {
      for (const auto& entry : std::filesystem::recursive_directory_iterator(input, failed)) {
        if (entry.is_regular_file() && entry.path().extension() == ".wo" && entry.path().string().find(".wolang-cache") == std::string::npos) {
          paths.push_back(entry.path().generic_string());
        }
      }
      std::sort(paths.begin(), paths.end());
    }
    else {
      paths.push_back(input);
    }

    for (const std::string& path : paths) {
      std::string error;
      const FileId file = database->LoadFile(path, &error);
      if (!file.is_valid()) {
        std::fprintf(stderr, "wolang: %s\n", error.c_str());
        return false;
      }
      files->push_back(file);
    }
  }
  return true;
}

void PrintTokens(const SourceManager& sources,
                 const StringInterner& interner,
                 const TokenStream& stream,
                 bool show_trivia) {
  std::fprintf(stdout, "%s: %zu tokens\n",
               std::string(sources.Path(stream.file)).c_str(),
               stream.tokens.size());

  for (size_t i = 0; i < stream.tokens.size(); ++i) {
    const Token& token = stream.At(i);
    const LineColumn position = sources.Position(stream.file, token.offset);
    std::string detail;

    switch (token.kind) {
      case TokenKind::kIdentifier:
        detail = std::string(interner.Text(stream.IdentifierOf(token)));
        break;
      case TokenKind::kIntLiteral:
      case TokenKind::kFloatLiteral: {
        const NumberLiteral& number = stream.NumberOf(token);
        detail = number.is_float ? std::to_string(number.float_value)
                                 : std::to_string(number.integer_value);
        if (number.has_unit()) {
          detail += " unit=";
          detail += std::string(interner.Text(number.unit));
        }
        if (number.suffix != NumericSuffix::kNone) {
          detail += " suffix=";
          detail += std::string(NumericSuffixName(number.suffix));
        }
        break;
      }
      case TokenKind::kStringLiteral: {
        const StringLiteral& literal = stream.StringOf(token);
        detail = literal.is_raw ? "raw, " : "";
        detail += std::to_string(literal.parts.size());
        detail += " part(s)";
        if (literal.is_simple()) {
          detail += " \"";
          detail += std::string(literal.simple_text());
          detail += "\"";
        }
        break;
      }
      case TokenKind::kCharLiteral:
        detail = "U+";
        detail += std::to_string(stream.CharValueOf(token));
        break;
      default:
        detail = std::string(sources.SpanText(stream.SpanOf(token)));
        break;
    }

    std::fprintf(stdout, "  %4zu  %-20s %u:%-3u %s%s\n", i,
                 std::string(TokenKindName(token.kind)).c_str(), position.line,
                 position.column, detail.c_str(),
                 token.has_error() ? "  [error]" : "");
  }

  if (show_trivia) {
    for (const SourceSpan& comment : stream.comments) {
      const LineColumn position = sources.Position(comment.file, comment.begin);
      std::fprintf(stdout, "  comment %u:%u %s\n", position.line,
                   position.column,
                   std::string(sources.SpanText(comment)).c_str());
    }
  }
}

// The cache lives beside the package it belongs to, so two checkouts of the
// same project never share one.
std::string CachePathFor(const Options& options) {
  if (options.no_cache || options.inputs.size() != 1 ||
      !IsPackageDirectory(options.inputs.front())) {
    return std::string();
  }
  return options.inputs.front() + "/.wolang-cache/items.wdb";
}

int RunCheck(const Options& options) {
  Database database;
  DiagnosticEngine load_diagnostics;
  std::vector<FileId> files;
  if (!CollectInputs(&database, options, &load_diagnostics, &files)) {
    return 2;
  }

  const std::string cache_path = CachePathFor(options);
  if (!cache_path.empty()) {
    Cache::Load(&database, cache_path);
  }

  RenderOptions render;
  render.color = options.color && StderrIsTerminal();

  std::vector<Diagnostic> all = load_diagnostics.diagnostics();
  for (FileId file : files) {
    const std::vector<Diagnostic>* diagnostics = database.Diagnostics(file);
    if (diagnostics != nullptr) {
      all.insert(all.end(), diagnostics->begin(), diagnostics->end());
    }
  }

  if (!cache_path.empty()) {
    Cache::Save(&database, cache_path);
  }
  return Report(database.sources(), all, render);
}

int RunSyntaxCommand(const Options& options) {
  Database database;
  DiagnosticEngine load_diagnostics;
  std::vector<FileId> files;
  if (!CollectInputs(&database, options, &load_diagnostics, &files)) {
    return 2;
  }

  RenderOptions render;
  render.color = options.color && StderrIsTerminal();
  AstPrintOptions print;
  print.include_spans = options.show_spans;

  std::vector<Diagnostic> all = load_diagnostics.diagnostics();
  for (FileId file : files) {
    if (options.command == "tokens") {
      const TokenizedFile* tokens = database.Tokens(file);
      if (tokens != nullptr) {
        PrintTokens(database.sources(), database.interner(), tokens->tokens,
                    options.show_trivia);
      }
    } else if (options.command == "ir") {
      const LoweredFile* lowered = database.Lowered(file);
      if (lowered != nullptr) {
        // `--unoptimized` prints the IR as lowering left it, which is what
        // a verifier complaint prefixed "lowering:" is about -- the
        // optimized dump may have inlined the instruction away.
        std::fputs(ir::PrintModule(database.interner(), database.types(),
                                   options.unoptimized
                                       ? lowered->unoptimized
                                       : lowered->module)
                       .c_str(),
                   stdout);
        // A verifier complaint is a compiler bug, so it goes to stderr with
        // the IR still on stdout -- the dump is what the bug report needs.
        for (const std::string& problem : lowered->problems) {
          std::fprintf(stderr, "wolang: internal: %s\n", problem.c_str());
        }
        if (!lowered->problems.empty()) {
          return 70;
        }
      }
    } else if (options.command == "ast") {
      const ParsedFile* parsed = database.Parse(file);
      if (parsed != nullptr) {
        const std::string text = PrintAst(&database.sources(),
                                          database.interner(), parsed->ast,
                                          print);
        WriteAll(text, stdout);
      }
    }

    const std::vector<Diagnostic>* diagnostics = database.Diagnostics(file);
    if (diagnostics != nullptr) {
      all.insert(all.end(), diagnostics->begin(), diagnostics->end());
    }
  }
  return Report(database.sources(), all, render);
}

// `fmt --stdin`: the buffer an editor has, formatted back to it.
//
// The same formatter the file-based path runs, and the same refusal when
// the text does not parse -- except that an editor gets its text back
// unchanged rather than an error, because a buffer mid-edit is the
// normal case and losing it would be unforgivable.
int RunFormatStdin(const Options& options) {
  std::string text;
  char buffer[4096];
  while (std::size_t read =
             std::fread(buffer, 1, sizeof(buffer), stdin)) {
    text.append(buffer, read);
  }

  Database database;
  const FileId file = database.AddFile("<stdin>", text);
  const TokenizedFile* tokens = database.Tokens(file);
  const ParsedFile* parsed = database.Parse(file);
  if (tokens == nullptr || parsed == nullptr) {
    std::fwrite(text.data(), 1, text.size(), stdout);
    return 0;
  }
  bool syntax_error = false;
  for (const Diagnostic& diagnostic : tokens->diagnostics) {
    syntax_error |= diagnostic.severity == Severity::kError;
  }
  for (const Diagnostic& diagnostic : parsed->diagnostics) {
    syntax_error |= diagnostic.severity == Severity::kError;
  }
  if (syntax_error) {
    std::fwrite(text.data(), 1, text.size(), stdout);
    return 0;
  }
  const std::string formatted =
      Format(database.sources(), database.interner(), tokens->tokens,
             parsed->ast);
  std::fwrite(formatted.data(), 1, formatted.size(), stdout);
  (void)options;
  return 0;
}

// `wolang fix`: applies what the diagnostics suggest.
//
// A diagnostic that knows the change carries it (D191), and this is the
// same edits the editor applies, for a person who does not have one open
// -- or for a whole tree at once. Without `--write` it says what it
// would do and changes nothing.
int RunFix(const Options& options) {
  Database database;
  DiagnosticEngine load_diagnostics;
  std::vector<FileId> files;
  if (!CollectInputs(&database, options, &load_diagnostics, &files)) {
    return 2;
  }

  int changed = 0;
  int offered = 0;
  for (FileId file : files) {
    const std::vector<Diagnostic>* diagnostics = database.Diagnostics(file);
    if (diagnostics == nullptr) {
      continue;
    }
    // Every edit for this file, latest first: applying from the end means
    // an earlier offset is still where it was.
    std::vector<std::pair<FixEdit, std::string>> edits;
    for (const Diagnostic& diagnostic : *diagnostics) {
      for (const Fix& fix : diagnostic.fixes) {
        for (const FixEdit& edit : fix.edits) {
          if (edit.span.is_valid() && edit.span.file == file) {
            edits.emplace_back(edit, diagnostic.code + ": " + fix.message);
          }
        }
      }
    }
    if (edits.empty()) {
      continue;
    }
    std::sort(edits.begin(), edits.end(),
              [](const auto& left, const auto& right) {
                return left.first.span.begin > right.first.span.begin;
              });

    const std::string path(database.sources().Path(file));
    std::string text(database.sources().Text(file));
    for (const auto& held : edits) {
      const FixEdit& edit = held.first;
      if (edit.span.begin > text.size() || edit.span.end > text.size()) {
        continue;
      }
      ++offered;
      const LineColumn at =
          database.sources().Position(file, edit.span.begin);
      std::fprintf(stdout, "%s:%u:%u: %s\n", path.c_str(), at.line,
                   at.column, held.second.c_str());
      if (options.write_in_place) {
        text.replace(edit.span.begin, edit.span.end - edit.span.begin,
                     edit.new_text);
      }
    }
    if (!options.write_in_place) {
      continue;
    }
    std::FILE* out = std::fopen(path.c_str(), "wb");
    if (out == nullptr) {
      std::fprintf(stderr, "wolang: cannot write %s\n", path.c_str());
      return 1;
    }
    std::fwrite(text.data(), 1, text.size(), out);
    std::fclose(out);
    ++changed;
  }

  if (offered == 0) {
    std::fprintf(stdout, "nothing to fix\n");
    return 0;
  }
  if (options.write_in_place) {
    std::fprintf(stdout, "%d file%s changed\n", changed,
                 changed == 1 ? "" : "s");
  } else {
    std::fprintf(stdout, "%d fix%s available; run again with --write\n",
                 offered, offered == 1 ? "" : "es");
  }
  return 0;
}

int RunFormat(const Options& options) {
  if (options.from_stdin) {
    return RunFormatStdin(options);
  }
  Database database;
  DiagnosticEngine load_diagnostics;
  std::vector<FileId> files;
  if (!CollectInputs(&database, options, &load_diagnostics, &files)) {
    return 2;
  }

  RenderOptions render;
  render.color = options.color && StderrIsTerminal();

  int exit_code = 0;
  for (FileId file : files) {
    const TokenizedFile* tokens = database.Tokens(file);
    const ParsedFile* parsed = database.Parse(file);
    if (tokens == nullptr || parsed == nullptr) {
      continue;
    }

    // Only the parse has to have succeeded. Formatting a tree the parser
    // could not finish would silently drop whatever it failed on, but an
    // unresolved name or a type error says nothing about the syntax -- and
    // refusing to format then would make `fmt` useless on a file that is
    // mid-edit, which is when it is wanted most.
    bool syntax_error = false;
    for (const Diagnostic& diagnostic : tokens->diagnostics) {
      syntax_error |= diagnostic.severity == Severity::kError;
    }
    for (const Diagnostic& diagnostic : parsed->diagnostics) {
      syntax_error |= diagnostic.severity == Severity::kError;
    }
    if (syntax_error) {
      std::fprintf(stderr, "wolang: %s does not parse; not formatting\n",
                   std::string(database.sources().Path(file)).c_str());
      std::vector<Diagnostic> reported = tokens->diagnostics;
      reported.insert(reported.end(), parsed->diagnostics.begin(),
                      parsed->diagnostics.end());
      return Report(database.sources(), reported, render);
    }

    const std::string_view original = database.sources().Text(file);
    std::string formatted = Format(database.sources(), database.interner(), tokens->tokens, parsed->ast);
    if (original.find("\r\n") != std::string_view::npos) {
      std::string crlf;
      crlf.reserve(formatted.size() + formatted.size() / 16);
      for (const char c : formatted) {
        if (c == '\n') {
          crlf.push_back('\r');
        }
        crlf.push_back(c);
      }
      formatted = std::move(crlf);
    }

    if (options.check_only) {
      if (formatted != original) {
        std::fprintf(stderr, "%s is not formatted\n",
                     std::string(database.sources().Path(file)).c_str());
        exit_code = 1;
      }
      continue;
    }

    if (options.write_in_place) {
      if (formatted == original) {
        continue;
      }
      const std::string path(database.sources().Path(file));
      std::ofstream stream(path, std::ios::binary | std::ios::trunc);
      if (!stream.is_open()) {
        std::fprintf(stderr, "wolang: cannot write '%s'\n", path.c_str());
        return 2;
      }
      stream.write(formatted.data(),
                   static_cast<std::streamsize>(formatted.size()));
      std::fprintf(stdout, "formatted %s\n", path.c_str());
      continue;
    }

    WriteAll(formatted, stdout);
  }
  return exit_code;
}

// What the scheduler resumes a task through.
//
// The scheduler holds a kind and a frame and knows nothing else; this is
// where that becomes a call into whichever tier is running the program.
// Both go through the one entry point the compiler generates, so neither
// tier needs a scheduler of its own.
struct TaskHost {
  Database* database = nullptr;
  const ir::Module* module = nullptr;
  abi::LayoutStore* layouts = nullptr;
  interp::ForeignCaller* foreign = nullptr;
  codegen::CompiledModule* compiled = nullptr;
  std::span<const ir::Module* const> linked;
  Symbol dispatch;
};

bool ResumeTask(void* context, int32_t kind, void* frame) {
  auto* host = static_cast<TaskHost*>(context);
  std::vector<interp::Value> arguments;
  arguments.reserve(2);
  arguments.push_back(interp::Value::Int(static_cast<uint64_t>(kind),
                                         IntWidth::k32, true));
  arguments.push_back(
      interp::Value::Int(reinterpret_cast<uint64_t>(frame), IntWidth::k64,
                         /*is_signed=*/false));

  if (host->compiled != nullptr &&
      host->compiled->Contains(host->dispatch)) {
    // The hot path: no boxing, no lookup, no allocation.
    bool faulted = false;
    const bool finished =
        host->compiled->ResumeTask(host->dispatch, kind, frame, &faulted);
    if (faulted) {
      std::fprintf(stderr, "wolang: a task faulted\n");
    }
    return finished;
  }

  interp::Outcome outcome;
  {
    outcome = interp::RunNamed(host->database->interner(),
                               host->database->types(), *host->module,
                               host->dispatch, arguments, {}, nullptr,
                               host->foreign, host->layouts, host->linked);
  }
  if (!outcome.ok()) {
    std::fprintf(stderr, "wolang: %s at %s: %s\n",
                 std::string(interp::TrapKindName(outcome.trap)).c_str(),
                 outcome.where.c_str(), outcome.message.c_str());
    // Finished, as far as the scheduler is concerned: a task that faulted
    // is not one to resume.
    return true;
  }
  return outcome.value.kind() == interp::Value::Kind::kBool &&
         outcome.value.boolean();
}

// Every file's IR, for the code generator.
//
// All of them at once: lowering is per file, so a call into another file is
// a declaration with no body, and compiling one file at a time would leave
// every cross-module call to the interpreter. In a package that is most of
// them.
// Opens what the packages said they link against, so that the tiers
// which resolve a symbol through the loader can find it.
//
// `wolang build` passes the same names to the linker; a program that is
// *run* has no link step, so the library has to be brought into the
// process by hand. Failure is quiet on purpose: a package may name a
// library only its ahead-of-time build needs, and a missing symbol is
// reported where it is called rather than here (D129).
void OpenLinkedLibraries(Database* database) {
  for (size_t i = 0; i < database->package_count(); ++i) {
    const Package* package =
        database->GetPackage(PackageId{static_cast<uint32_t>(i)});
    if (package == nullptr) {
      continue;
    }
    for (const std::string& library : package->manifest.link_libraries) {
      // A path or a file name is opened as it is; a bare name gets the
      // platform's decoration.
      const bool spelled =
          library.find('/') != std::string::npos ||
          library.find('\\') != std::string::npos ||
          library.find(".so") != std::string::npos ||
          library.find(".dll") != std::string::npos ||
          library.find(".dylib") != std::string::npos;
      std::vector<std::string> tries;
      if (spelled) {
        tries.push_back(library);
      } else {
#if defined(__APPLE__)
        const std::string decorated = "lib" + library + ".dylib";
#elif defined(_WIN32)
        const std::string decorated = library + ".dll";
#else
        const std::string decorated = "lib" + library + ".so";
#endif
        // The named paths *first*: a bare name goes through the loader's
        // own search, which on macOS finds a shim for `libcrypto` and
        // warns that it is being loaded unsafely. A manifest that says
        // where the library is means it (D150).
        //
        // A relative path is relative to the package, not to wherever
        // the command was run: a manifest describes its own package,
        // and `gfx` saying `../../build/debug/platform` has to mean the
        // same thing from every directory (D155).
        for (const std::string& path : package->manifest.link_paths) {
          if (!path.empty() && !std::filesystem::path(path).is_absolute() && path.front() != '/' &&
              !package->directory.empty()) {
            tries.push_back(package->directory + "/" + path + "/" +
                            decorated);
          }
          tries.push_back(path + "/" + decorated);
        }
        tries.push_back(decorated);
      }
      for (const std::string& candidate : tries) {
        if (runtime::OpenLibrary(candidate)) {
          break;
        }
      }
    }
  }
}

std::vector<const ir::Module*> CollectModules(
    Database* database,
    const std::vector<FileId>& files) {
  std::vector<const ir::Module*> modules;
  for (FileId file : files) {
    const LoweredFile* lowered = database->Lowered(file);
    if (lowered != nullptr) {
      modules.push_back(&lowered->module);
    }
  }
  return modules;
}

std::vector<codegen::FileCode> CollectCode(Database* database,
                                           const std::vector<FileId>& files) {
  std::vector<codegen::FileCode> code;
  for (FileId file : files) {
    const LoweredFile* lowered = database->Lowered(file);
    const CheckedFile* checked = database->Checked(file);
    if (lowered == nullptr || checked == nullptr) {
      continue;
    }
    code.push_back(codegen::FileCode{&checked->resolution, &lowered->module});
  }
  return code;
}

// Runs `main`, through the interpreter. The reference semantics are the
// unoptimized IR; the optimized form is run too and the two results are
// compared, which turns "the passes preserve meaning" into something the
// driver checks on every run rather than something assumed.

// Where a trap happened, as a file and a line when the IR knew.
//
// "at line 12 of app.wo" is what a programmer can act on; "at @main:%27"
// is what the compiler happens to call the instruction (D118).
std::string Where(const SourceManager& sources,
                  const interp::Outcome& outcome) {
  if (!outcome.span.is_valid()) {
    return outcome.where;
  }
  const LineColumn at = sources.Position(outcome.span);
  std::string where(sources.Path(outcome.span.file));
  where.append(":");
  where.append(std::to_string(at.line));
  where.append(":");
  where.append(std::to_string(at.column));
  where.append(" (");
  where.append(outcome.where);
  where.append(")");
  return where;
}

std::string Describe(const interp::Outcome& outcome) {
  if (outcome.ok()) {
    return outcome.value.Describe();
  }
  std::string result(interp::TrapKindName(outcome.trap));
  result.append(" at ");
  result.append(outcome.where);
  return result;
}

// `wolang fetch`: get what the manifest asked for, and write down what
// was got.
int RunFetch(const Options& options) {
  Database database;
  DiagnosticEngine diagnostics;
  const std::string& where = options.inputs.front();
  if (!IsPackageDirectory(where)) {
    std::fprintf(stderr, "wolang: %s is not a package\n", where.c_str());
    return 2;
  }
  const bool complete =
      FetchDependencies(&database, where, &diagnostics);

  RenderOptions render;
  render.color = options.color && StderrIsTerminal();
  const int reported =
      Report(database.sources(), diagnostics.diagnostics(), render);
  if (!complete) {
    return reported != 0 ? reported : 1;
  }

  int remote = 0;
  for (size_t i = 0; i < database.package_count(); ++i) {
    const Package* package =
        database.GetPackage(PackageId{static_cast<uint32_t>(i)});
    if (package == nullptr) {
      continue;
    }
    for (const ManifestDependency& dependency :
         package->manifest.dependencies) {
      remote += dependency.is_remote() ? 1 : 0;
    }
  }
  std::fprintf(stdout, "%d fetched, written to %s\n", remote,
               (std::filesystem::path(where) / "wolang.lock").c_str());
  return reported;
}

// Where the standard library is, as this binary can find it.
//
// In order: what the environment says, a tree the given directory sits
// inside, the one next to the compiler itself, and the tree this
// compiler was built from (D132).
std::string StandardLibrary(const Options& options,
                            const std::filesystem::path& near) {
  std::error_code failed;
  if (const char* said = std::getenv("WOLANG_STD")) {
    return said;
  }
  for (std::filesystem::path up = std::filesystem::absolute(near, failed);
       !up.empty() && up != up.root_path(); up = up.parent_path()) {
    if (std::filesystem::exists(up / "lib" / "std" / "wolang.toml",
                                failed)) {
      return (up / "lib" / "std").string();
    }
  }
  const std::filesystem::path own =
      std::filesystem::weakly_canonical(options.program, failed);
  const std::filesystem::path beside =
      own.parent_path().parent_path() / "lib" / "std";
  if (std::filesystem::exists(beside / "wolang.toml", failed)) {
    return beside.string();
  }
#ifdef WOLANG_LIBRARY_DIRECTORY
  const std::filesystem::path built =
      std::filesystem::path(WOLANG_LIBRARY_DIRECTORY) / "std";
  if (std::filesystem::exists(built / "wolang.toml", failed)) {
    return built.string();
  }
#endif
  return std::string();
}

// What a session's program printed, so that the loop can decide whether
// to show it: a line whose value is nothing prints `<?>`, and showing
// that would be worse than showing nothing (D140).
std::string repl_output;

void CollectRepl(void* user, const char* bytes, uint64_t length) {
  (void)user;
  repl_output.append(bytes, static_cast<size_t>(length));
}

int RunRepl(const Options& options) {
  std::fprintf(stdout,
               "wolang %s -- type an expression, a statement or a "
               "declaration.\n"
               ":help for the commands, :quit to leave.\n",
               std::string(kVersion).c_str());

  const std::string standard =
      StandardLibrary(options, std::filesystem::current_path());
  if (standard.empty()) {
    std::fprintf(stderr,
                 "wolang: cannot find the standard library; set "
                 "WOLANG_STD\n");
  }

  // One session, one vm: each line becomes a *file* of the same package,
  // so a declaration stays declared and a variable keeps what was
  // written into it. A line that does not compile is dropped by
  // rebuilding the session from the ones that did (D140).
  std::vector<std::string> history;
  int serial = 0;

  const auto fresh = [&]() {
    auto held = std::make_unique<wolang::Vm>();
    held->OnOutput(&CollectRepl, nullptr);
    // Replaying what the session already ran must not start its
    // variables again: what they hold is in the runtime's table, and
    // this is a rebuild rather than a new program.
    held->InitializeGlobals(false);
    if (!standard.empty()) {
      held->Load(standard);
    }
    for (size_t i = 0; i < history.size(); ++i) {
      held->LoadSource("session" + std::to_string(i) + ".wo", history[i]);
    }
    held->InitializeGlobals(true);
    return held;
  };
  std::unique_ptr<wolang::Vm> vm = fresh();

  // Whether a line finishes what it started: a session that runs half a
  // `fun` is a session nobody can declare anything in.
  const auto complete = [](const std::string& text) {
    int depth = 0;
    bool in_string = false;
    for (size_t i = 0; i < text.size(); ++i) {
      const char c = text[i];
      if (in_string) {
        if (c == '\\') {
          ++i;
        } else if (c == '"') {
          in_string = false;
        }
        continue;
      }
      if (c == '"') {
        in_string = true;
      } else if (c == '{' || c == '(' || c == '[') {
        ++depth;
      } else if (c == '}' || c == ')' || c == ']') {
        --depth;
      }
    }
    return depth <= 0;
  };

  const auto is_declaration = [](const std::string& line) {
    static const char* kStarts[] = {"fun ",      "var ",       "const ",
                                    "class ",    "struct ",    "enum ",
                                    "ability ",  "component ", "tag ",
                                    "type ",     "extern ",    "offer ",
                                    "machine ",  "system ",    "effect ",
                                    "@",         "extend ",    "need "};
    for (const char* start : kStarts) {
      if (line.rfind(start, 0) == 0) {
        return true;
      }
    }
    return false;
  };

  // `Print` is always in scope in a session: one with no way to show a
  // value is not a session.
  const std::string prelude = "need std.io.{Print}\n";

  std::string pending;
  for (;;) {
    std::fprintf(stdout, "%s", pending.empty() ? ">>> " : "... ");
    std::fflush(stdout);
    std::string line;
    if (!std::getline(std::cin, line)) {
      std::fprintf(stdout, "\n");
      break;
    }
    if (!pending.empty()) {
      pending.push_back('\n');
    }
    pending.append(line);
    if (!complete(pending)) {
      continue;
    }
    std::string whole = pending;
    pending.clear();

    while (!whole.empty() && (whole.front() == ' ' || whole.front() == '\t')) {
      whole.erase(whole.begin());
    }
    while (!whole.empty() &&
           (whole.back() == ' ' || whole.back() == '\t' ||
            whole.back() == '\n')) {
      whole.pop_back();
    }
    if (whole.empty()) {
      continue;
    }

    if (whole == ":quit" || whole == ":q") {
      break;
    }
    if (whole == ":help" || whole == ":h") {
      std::fprintf(stdout,
                   "  <expression>   evaluate and show it\n"
                   "  <statement>    run it\n"
                   "  fun / var / class / need ...   declare it for the "
                   "session\n"
                   "  :list          what the session has declared\n"
                   "  :clear         forget all of it\n"
                   "  :quit          leave\n");
      continue;
    }
    if (whole == ":list") {
      for (const std::string& held : history) {
        std::fputs(held.c_str(), stdout);
      }
      continue;
    }
    if (whole == ":clear") {
      history.clear();
      vm = fresh();
      std::fprintf(stdout, "the session is empty again\n");
      continue;
    }

    const bool declaring = is_declaration(whole);
    const std::string name = "Repl" + std::to_string(serial++);
    const std::string file =
        "session" + std::to_string(history.size()) + ".wo";
    // A declaration is a file of its own; a line to evaluate is a
    // function that shows what it came to.
    const std::string source =
        declaring ? whole + "\n"
                  : prelude + "offer fun " + name +
                        "() uses io, unsafe {\n  Print(\"{" + whole +
                        "}\")\n}\n";

    repl_output.clear();
    if (!vm->LoadSource(file, source)) {
      const std::string errors = vm->errors();
      // The broken file is still in the session's database, so the
      // session is rebuilt from the lines that worked.
      vm = fresh();
      bool ran = false;
      if (!declaring) {
        // Not an expression, perhaps: `x = x + 1` and `Print("hi")` are
        // statements, and a statement in an interpolation does not
        // compile.
        const std::string again = prelude + "offer fun " + name +
                                  "() uses io, unsafe {\n  " + whole +
                                  "\n}\n";
        repl_output.clear();
        if (vm->LoadSource(file, again)) {
          ran = vm->Call(name).has_value();
          if (ran) {
            WriteAll(repl_output, stdout);
          } else {
            std::fputs(vm->errors().c_str(), stderr);
          }
        }
      }
      if (!ran) {
        std::fputs(errors.c_str(), stderr);
        vm = fresh();
      }
      continue;
    }

    if (declaring) {
      history.push_back(source);
      continue;
    }
    if (!vm->Call(name).has_value()) {
      std::fputs(vm->errors().c_str(), stderr);
      vm = fresh();
      continue;
    }
    // `<?>` at the end is a value with nothing in it: the line was a
    // statement, and what it printed itself is what to show.
    const bool nothing =
        repl_output.size() >= 4 &&
        repl_output.compare(repl_output.size() - 4, 4, "<?>\n") == 0;
    std::fputs(nothing ? repl_output.substr(0, repl_output.size() - 4).c_str()
                       : repl_output.c_str(),
               stdout);
  }
  return 0;
}

// Where the program spent its time.
//
// A sampling profiler: a timer fires a few thousand times a second, the
// handler writes down the program counter, and afterwards each sample is
// attributed to the function whose code it landed in. Sampling rather
// than counting, because counting changes what it measures -- and
// because a profile of the compiled tier is the one worth having (D139).
namespace profile {

// What the handler may touch: it runs on a signal, so everything here is
// either atomic or written only by it.
constexpr size_t kMostSamples = 1 << 18;
std::atomic<uint32_t> taken{0};
uint64_t samples[kMostSamples];
std::atomic<bool> running{false};

#if defined(_WIN32)
bool Start(int) {
  return false;
}

void Stop() {
  running.store(false, std::memory_order_relaxed);
}
#else
void OnTick(int, siginfo_t*, void* context) {
  if (!running.load(std::memory_order_relaxed)) {
    return;
  }
  const auto* uc = static_cast<const ucontext_t*>(context);
  uint64_t pc = 0;
#if defined(__APPLE__) && defined(__aarch64__)
  pc = uc->uc_mcontext->__ss.__pc;
#elif defined(__APPLE__) && defined(__x86_64__)
  pc = uc->uc_mcontext->__ss.__rip;
#elif defined(__linux__) && defined(__aarch64__)
  pc = uc->uc_mcontext.pc;
#elif defined(__linux__) && defined(__x86_64__)
  pc = static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RIP]);
#else
  (void)uc;
#endif
  const uint32_t at = taken.fetch_add(1, std::memory_order_relaxed);
  if (at < kMostSamples) {
    samples[at] = pc;
  }
}

// Starts sampling at |hertz|, or does nothing when the platform will not
// give us a timer.
bool Start(int hertz) {
  struct sigaction action = {};
  action.sa_flags = SA_SIGINFO | SA_RESTART;
  action.sa_sigaction = &OnTick;
  sigemptyset(&action.sa_mask);
  if (::sigaction(SIGPROF, &action, nullptr) != 0) {
    return false;
  }
  itimerval timer = {};
  timer.it_interval.tv_usec = 1000000 / (hertz > 0 ? hertz : 1000);
  timer.it_value = timer.it_interval;
  if (::setitimer(ITIMER_PROF, &timer, nullptr) != 0) {
    return false;
  }
  running.store(true, std::memory_order_relaxed);
  return true;
}

void Stop() {
  running.store(false, std::memory_order_relaxed);
  itimerval off = {};
  ::setitimer(ITIMER_PROF, &off, nullptr);
  ::signal(SIGPROF, SIG_IGN);
}
#endif

// What was sampled, by function, most first.
void Report(const codegen::CompiledModule* compiled) {
  const uint32_t count =
      std::min<uint32_t>(taken.load(std::memory_order_relaxed),
                         kMostSamples);
  if (count == 0) {
    std::fprintf(stdout,
                 "no samples: the program finished before the timer "
                 "fired\n");
    return;
  }

  std::vector<std::pair<std::string, uint32_t>> tally;
  uint32_t elsewhere = 0;
  for (uint32_t i = 0; i < count; ++i) {
    std::string name =
        compiled != nullptr ? compiled->FunctionAt(samples[i])
                            : std::string();
    if (name.empty()) {
      // The runtime, the allocator, the kernel: everything that is not
      // this program's compiled code. Worth reporting as one line rather
      // than hiding, because "most of the time is in the allocator" is
      // an answer.
      ++elsewhere;
      continue;
    }
    bool held = false;
    for (auto& [existing, times] : tally) {
      if (existing == name) {
        ++times;
        held = true;
        break;
      }
    }
    if (!held) {
      tally.emplace_back(std::move(name), 1);
    }
  }
  std::sort(tally.begin(), tally.end(),
            [](const auto& left, const auto& right) {
              return left.second > right.second;
            });

  std::fprintf(stdout, "\n%u samples\n", count);
  for (const auto& [name, times] : tally) {
    std::fprintf(stdout, "  %5.1f%%  %6u  %s\n",
                 100.0 * times / count, times, name.c_str());
  }
  if (elsewhere != 0) {
    std::fprintf(stdout, "  %5.1f%%  %6u  <runtime, libraries, kernel>\n",
                 100.0 * elsewhere / count, elsewhere);
  }
}

}  // namespace profile

// The session `wolang dap` is serving, which is where a program's output
// goes instead of standard output: standard output is the editor's pipe.
static debug::DapSession* sm_Session = nullptr;

extern "C" int64_t WriteToEditor(int32_t fd, const char* bytes, uint64_t count) {
  if (sm_Session != nullptr && (fd == 1 || fd == 2)) {
    sm_Session->Output(std::string(bytes, static_cast<size_t>(count)), fd == 2 ? "stderr" : "stdout");
    return static_cast<int64_t>(count);
  }
  return static_cast<int64_t>(std::fwrite(bytes, 1, static_cast<size_t>(count), fd == 2 ? stderr : stdout));
}

// Builds |program| and runs its `main` on the interpreter, unoptimized,
// with |session| told where it is (D216).
int DebugProgram(const Options& options, const std::string& program, debug::DapSession* session, std::string* error) {
  Options debugging = options;
  debugging.inputs = {program};
  Database database;
  DiagnosticEngine load_diagnostics;
  std::vector<FileId> files;
  if (!CollectInputs(&database, debugging, &load_diagnostics, &files)) {
    *error = "wolang: cannot load " + program + "\n";
    return -1;
  }
  OpenLinkedLibraries(&database);

  std::vector<Diagnostic> all = load_diagnostics.diagnostics();
  const LoweredFile* lowered_main = nullptr;
  const CheckedFile* checked_main = nullptr;
  std::vector<const ir::Module*> modules;
  const Symbol main = database.interner().Intern("main");
  for (FileId file : files) {
    if (const std::vector<Diagnostic>* diagnostics = database.Diagnostics(file)) {
      all.insert(all.end(), diagnostics->begin(), diagnostics->end());
    }
    const LoweredFile* lowered = database.Lowered(file);
    if (lowered == nullptr) {
      continue;
    }
    modules.push_back(&lowered->unoptimized);
    if (lowered->unoptimized.Find(main) != nullptr) {
      lowered_main = lowered;
      checked_main = database.Checked(file);
    }
  }
  for (const Diagnostic& diagnostic : all) {
    if (diagnostic.severity == Severity::kError) {
      Report(database.sources(), all, RenderOptions());
      *error = "wolang: " + program + " has errors; `wolang check` lists them\n";
      return -1;
    }
  }
  if (lowered_main == nullptr || checked_main == nullptr) {
    *error = "wolang: no `main` to run in " + program + "\n";
    return -1;
  }

  const std::unique_ptr<interp::ForeignCaller> foreign = codegen::MakeForeignCaller();
  abi::LayoutStore layouts(database.types(), checked_main->resolution);
  database.ShareMembers(&layouts);
  TaskHost host;
  host.database = &database;
  host.module = &lowered_main->unoptimized;
  host.layouts = &layouts;
  host.foreign = foreign.get();
  host.linked = modules;
  host.dispatch = database.interner().Intern("wolang$dispatch");
  runtime::ResetScheduler();
  runtime::InstallResume(&ResumeTask, &host);

  session->SetSources(&database.sources());
  interp::Limits limits;
  limits.steps = UINT64_MAX;
  limits.debugger = session;
  interp::Output output;
  const interp::Outcome outcome = interp::RunNamed(database.interner(), database.types(), lowered_main->unoptimized, main, {}, limits, &output, foreign.get(), &layouts, modules);
  session->SetSources(nullptr);
  if (!output.text.empty()) {
    session->Output(output.text);
  }
  if (!outcome.ok()) {
    if (outcome.message != "stopped by the debugger") {
      session->Output("wolang: " + std::string(interp::TrapKindName(outcome.trap)) + " at " + Where(database.sources(), outcome) + ": " + outcome.message + "\n", "stderr");
    }
    return 1;
  }
  return 0;
}

// `wolang dap`: the Debug Adapter Protocol over standard input and
// output, for an editor to start (D216).
int RunDap(const Options& options) {
#ifdef _WIN32
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  debug::DapSession session([](const std::string& body) { debug::WriteFramed(stdout, body); }, [&options](const lsp::Json& arguments, debug::DapSession* debugged, std::string* error) { return DebugProgram(options, arguments["program"].AsString(), debugged, error); });
  sm_Session = &session;
  runtime::RegisterSymbol("write", reinterpret_cast<void*>(&WriteToEditor));
  while (!session.finished()) {
    const std::optional<std::string> body = debug::ReadFramed(stdin);
    if (!body.has_value()) {
      break;
    }
    session.Handle(*body);
  }
  sm_Session = nullptr;
  return 0;
}

int RunProgram(const Options& options) {
  Database database;
  DiagnosticEngine load_diagnostics;
  std::vector<FileId> files;
  if (!CollectInputs(&database, options, &load_diagnostics, &files)) {
    return 2;
  }

  RenderOptions render;
  render.color = options.color && StderrIsTerminal();
  OpenLinkedLibraries(&database);

  // A journal, when one was asked for: everything the machine answers --
  // the clock, standard input, the process id -- written down on the way
  // out and handed back on the way in (D142). Opened before anything
  // runs, because a global's initializer may already ask the clock.
  if (!options.record.empty() &&
      runtime::JournalRecord(options.record.c_str()) != 0) {
    std::fprintf(stderr, "wolang: cannot write the journal %s\n",
                 options.record.c_str());
    return 2;
  }
  if (!options.replay.empty() &&
      runtime::JournalReplay(options.replay.c_str()) != 0) {
    std::fprintf(stderr, "wolang: cannot read the journal %s\n",
                 options.replay.c_str());
    return 2;
  }

  std::vector<Diagnostic> all = load_diagnostics.diagnostics();
  const LoweredFile* program = nullptr;
  const CheckedFile* program_file = nullptr;
  for (FileId file : files) {
    if (const std::vector<Diagnostic>* diagnostics =
            database.Diagnostics(file)) {
      all.insert(all.end(), diagnostics->begin(), diagnostics->end());
    }
    const LoweredFile* lowered = database.Lowered(file);
    if (lowered == nullptr) {
      continue;
    }
    for (const std::string& problem : lowered->problems) {
      std::fprintf(stderr, "wolang: internal: %s\n", problem.c_str());
    }
    if (lowered->module.Find(database.interner().Intern("main")) != nullptr) {
      program = lowered;
      program_file = database.Checked(file);
    }
  }

  const int reported = Report(database.sources(), all, render);
  if (reported != 0) {
    return reported;
  }
  if (program == nullptr) {
    std::fprintf(stderr, "wolang: no `main` to run\n");
    return 2;
  }

  const Symbol main = database.interner().Intern("main");

  // A program runs **once**.
  //
  // Wave 7 ran it twice -- once on the unoptimized IR and once on the
  // optimized one -- and compared, which was worth it when the only thing a
  // program could do was fill a sink the driver owned. Now that a program
  // can call `write`, running it twice writes twice: a side effect is not
  // something to repeat for the compiler's benefit. The comparison moved to
  // where it belongs, which is `wolang test`: every `@Test` runs in both
  // tiers and has to agree.
  const std::unique_ptr<interp::ForeignCaller> foreign =
      codegen::MakeForeignCaller();

  std::unique_ptr<codegen::CompiledModule> compiled;
  if (!options.interpret && codegen::IsSupportedTarget() &&
      program_file != nullptr) {
    const std::vector<codegen::FileCode> code = CollectCode(&database, files);
    compiled = codegen::Compile(database.interner(), database.types(), code);
  }

  // The scheduler needs a way back into the program before `main` runs:
  // `main` may spawn, and running what it spawned is a call into here.
  abi::LayoutStore program_layouts(database.types(),
                                   program_file->resolution);
  database.ShareMembers(&program_layouts);
  const std::vector<const ir::Module*> modules =
      CollectModules(&database, files);
  TaskHost host;
  host.database = &database;
  host.module = &program->module;
  host.layouts = &program_layouts;
  host.foreign = foreign.get();
  host.compiled = compiled.get();
  host.linked = modules;
  host.dispatch = database.interner().Intern("wolang$dispatch");
  runtime::ResetScheduler();
  runtime::InstallResume(&ResumeTask, &host);

  interp::Output output;
  interp::Outcome outcome;
  const bool profiling = options.profile && profile::Start(options.hertz);
  if (options.profile && !profiling) {
    std::fprintf(stderr,
                 "wolang: this system will not give us a profiling "
                 "timer\n");
  }
  if (compiled != nullptr && compiled->Contains(main)) {
    outcome = compiled->Run(main, {});
  } else {
    // The interpreter needs the layout to read a value it only has the
    // address of, which a task frame is.
    outcome = interp::RunNamed(database.interner(), database.types(),
                               program->module, main, {}, {}, &output,
                               foreign.get(), &program_layouts, modules);
  }

  if (profiling) {
    profile::Stop();
    profile::Report(compiled.get());
  }
  runtime::JournalClose();
  WriteAll(output.text, stdout);
  if (!outcome.ok()) {
    std::fprintf(stderr, "wolang: %s at %s: %s\n",
                 std::string(interp::TrapKindName(outcome.trap)).c_str(),
                 Where(database.sources(), outcome).c_str(),
                 outcome.message.c_str());
    return 1;
  }
  if (outcome.value.kind() != interp::Value::Kind::kVoid) {
    std::fprintf(stdout, "%s\n", outcome.value.Describe().c_str());
  }
  return 0;
}

// Runs every `@Test` function. A test passes when it returns without
// trapping and does not return `false`; anything else is a failure, with the
// trap named.
int RunTests(const Options& options) {
  Database database;
  DiagnosticEngine load_diagnostics;
  std::vector<FileId> files;
  if (!CollectInputs(&database, options, &load_diagnostics, &files)) {
    return 2;
  }

  RenderOptions render;
  render.color = options.color && StderrIsTerminal();
  // A test is a program: a package that declares a library to link
  // needs it loaded here too, or its own tests are the one place its
  // bindings cannot be reached (D155).
  OpenLinkedLibraries(&database);
  std::vector<Diagnostic> all = load_diagnostics.diagnostics();
  for (FileId file : files) {
    if (const std::vector<Diagnostic>* diagnostics =
            database.Diagnostics(file)) {
      all.insert(all.end(), diagnostics->begin(), diagnostics->end());
    }
  }
  const int reported = Report(database.sources(), all, render);
  if (reported != 0) {
    return reported;
  }

  // Every test is also a differential check between the tiers: the same
  // function, compiled and interpreted, has to reach the same answer. Tests
  // are the one place a program is known to terminate, which is what makes
  // running it twice safe.
  std::unique_ptr<codegen::CompiledModule> compiled;
  const std::unique_ptr<interp::ForeignCaller> foreign =
      codegen::MakeForeignCaller();
  if (codegen::IsSupportedTarget()) {
    const std::vector<codegen::FileCode> code = CollectCode(&database, files);
    compiled = codegen::Compile(database.interner(), database.types(), code);
  }

  const std::vector<const ir::Module*> modules =
      CollectModules(&database, files);

  // The tests of the packages that were *asked for*, not of everything
  // they depend on: `wolang test .` in a new package should run its own
  // test and not the standard library's seventy-three. `--all` is how to
  // ask for the rest (D132).
  std::vector<std::string> wanted;
  for (const std::string& input : options.inputs) {
    std::error_code failed_path;
    wanted.push_back(
        std::filesystem::weakly_canonical(input, failed_path).string());
  }
  const auto asked_for = [&](FileId file) {
    if (options.test_all || wanted.empty()) {
      return true;
    }
    const std::string where(database.sources().Path(file));
    for (const std::string& root : wanted) {
      if (!root.empty() && where.rfind(root, 0) == 0) {
        return true;
      }
    }
    return false;
  };

  int ran = 0;
  int failed = 0;
  for (FileId file : files) {
    const LoweredFile* lowered = database.Lowered(file);
    if (lowered == nullptr || !asked_for(file)) {
      continue;
    }
    for (Symbol name : lowered->tests) {
      if (!options.test_filter.empty() && database.interner().Text(name).find(options.test_filter) == std::string_view::npos) {
        continue;
      }
      ++ran;
      // Run through the unoptimized IR: the reference semantics are what a
      // test is testing.
      const CheckedFile* checked = database.Checked(file);
      // Bound to something that outlives the layout store: a conditional
      // whose other arm is a temporary makes the whole expression a value,
      // and the store would hold a reference to a copy that is already
      // gone. Which is what it did -- harmlessly until member tables
      // started being built on demand, and then not harmlessly at all.
      static const FileResolution kNoResolution;
      const FileResolution& resolution =
          checked != nullptr ? checked->resolution : kNoResolution;
      abi::LayoutStore layouts(database.types(), resolution);
      database.ShareMembers(&layouts);
      const interp::Outcome outcome = interp::RunNamed(
          database.interner(), database.types(), lowered->unoptimized, name,
          {}, {}, nullptr, foreign.get(), &layouts, modules);
      const bool returned_false =
          outcome.ok() &&
          outcome.value.kind() == interp::Value::Kind::kBool &&
          !outcome.value.boolean();

      if (compiled != nullptr && compiled->Contains(name)) {
        const interp::Outcome machine = compiled->Run(name, {});
        if (machine.trap != outcome.trap ||
            machine.where != outcome.where ||
            (outcome.ok() && machine.value != outcome.value)) {
          ++failed;
          std::fprintf(stdout,
                       "[ failed ] %s: the tiers disagree\n"
                       "           interpreter: %s\n"
                       "           machine code: %s\n",
                       std::string(database.interner().Text(name)).c_str(),
                       Describe(outcome).c_str(), Describe(machine).c_str());
          continue;
        }
      }

      if (outcome.ok() && !returned_false) {
        std::fprintf(stdout, "[     ok ] %s%s\n",
                     std::string(database.interner().Text(name)).c_str(),
                     compiled != nullptr && compiled->Contains(name)
                         ? " (both tiers)"
                         : "");
        continue;
      }
      ++failed;
      if (returned_false) {
        std::fprintf(stdout, "[ failed ] %s: returned false\n",
                     std::string(database.interner().Text(name)).c_str());
      } else {
        std::fprintf(stdout, "[ failed ] %s: %s at %s: %s\n",
                     std::string(database.interner().Text(name)).c_str(),
                     std::string(interp::TrapKindName(outcome.trap)).c_str(),
                     outcome.where.c_str(), outcome.message.c_str());
      }
    }
    std::fflush(stdout);
  }

  std::fprintf(stdout, "\n%d test%s run, %d failed\n", ran,
               ran == 1 ? "" : "s", failed);
  return failed == 0 ? 0 : 1;
}

// Prints what the dev tier made of each function: how much code, and for
// the ones it left alone, why. A tier that compiles what it can has to be
// able to say what it did not.
int RunAsm(const Options& options) {
  Database database;
  DiagnosticEngine load_diagnostics;
  std::vector<FileId> files;
  if (!CollectInputs(&database, options, &load_diagnostics, &files)) {
    return 2;
  }

  RenderOptions render;
  render.color = options.color && StderrIsTerminal();
  std::vector<Diagnostic> all = load_diagnostics.diagnostics();
  for (FileId file : files) {
    if (const std::vector<Diagnostic>* diagnostics =
            database.Diagnostics(file)) {
      all.insert(all.end(), diagnostics->begin(), diagnostics->end());
    }
  }
  if (const int reported = Report(database.sources(), all, render);
      reported != 0) {
    return reported;
  }

  if (!codegen::IsSupportedTarget()) {
    std::fprintf(stderr, "wolang: no code generator for this target\n");
    return 2;
  }

  const std::vector<codegen::FileCode> code = CollectCode(&database, files);
  const std::unique_ptr<codegen::CompiledModule> compiled =
      codegen::Compile(database.interner(), database.types(), code);
  std::fprintf(stdout,
               "// %u compiled, %u left to the interpreter, %zu bytes\n",
               compiled->compiled_count(), compiled->skipped_count(),
               compiled->size());

  for (FileId file : files) {
    const LoweredFile* lowered = database.Lowered(file);
    if (lowered == nullptr) {
      continue;
    }
    std::fprintf(stdout, "// %s\n",
                 std::string(database.sources().Path(file)).c_str());
    for (const ir::Function& function : lowered->module.functions) {
      const std::string name(database.interner().Text(function.name));
      if (compiled->Contains(function.name)) {
        uint32_t bytes = 0;
        for (const std::pair<std::string, uint32_t>& entry :
             compiled->Listing()) {
          if (entry.first == name) {
            bytes = entry.second;
          }
        }
        std::fprintf(stdout, "@%s: %u bytes\n", name.c_str(), bytes);
        if (options.raw) {
          // Where this function's bytes begin: the listing is in
          // emission order, so the ones before it say how far in it is.
          uint64_t at = compiled->code_begin();
          for (const std::pair<std::string, uint32_t>& entry :
               compiled->Listing()) {
            if (entry.first == name) {
              break;
            }
            at += entry.second;
          }
          const auto* emitted = reinterpret_cast<const uint8_t*>(at);
          for (uint32_t i = 0; i < bytes; ++i) {
            std::fprintf(stdout, "%02x%s", emitted[i],
                         (i + 1) % 16 == 0 || i + 1 == bytes ? "\n" : " ");
          }
        }
        continue;
      }
      // A foreign declaration is not "left to the interpreter": it has no
      // body on either side.
      std::fprintf(stdout, "@%s: %s%s\n", name.c_str(),
                   function.is_foreign() ? "" : "interpreted, ",
                   std::string(compiled->SkipReason(function.name)).c_str());
    }
  }
  return 0;
}

// Runs every function marked `@Bench` through the dev tier and times it.
//
// Only the compiled tier: the interpreter is thousands of times slower and
// has a step budget, so timing it would measure the budget. Whether the two
// tiers agree is what `wolang test` is for.
int RunBench(const Options& options) {
  Database database;
  DiagnosticEngine load_diagnostics;
  std::vector<FileId> files;
  if (!CollectInputs(&database, options, &load_diagnostics, &files)) {
    return 2;
  }

  RenderOptions render;
  render.color = options.color && StderrIsTerminal();
  std::vector<Diagnostic> all = load_diagnostics.diagnostics();
  for (FileId file : files) {
    if (const std::vector<Diagnostic>* diagnostics =
            database.Diagnostics(file)) {
      all.insert(all.end(), diagnostics->begin(), diagnostics->end());
    }
  }
  if (const int reported = Report(database.sources(), all, render);
      reported != 0) {
    return reported;
  }
  if (!codegen::IsSupportedTarget()) {
    std::fprintf(stderr, "wolang: no code generator for this target\n");
    return 2;
  }

  const std::vector<codegen::FileCode> code = CollectCode(&database, files);
  const std::unique_ptr<codegen::CompiledModule> compiled =
      codegen::Compile(database.interner(), database.types(), code);

  int ran = 0;
  for (FileId file : files) {
    const LoweredFile* lowered = database.Lowered(file);
    const CheckedFile* checked = database.Checked(file);
    if (lowered == nullptr || checked == nullptr ||
        lowered->benches.empty()) {
      continue;
    }

    // A benchmark may spawn, so the scheduler needs its way back in.
    abi::LayoutStore layouts(database.types(), checked->resolution);
    database.ShareMembers(&layouts);
    TaskHost host;
    host.database = &database;
    host.module = &lowered->module;
    host.layouts = &layouts;
    host.compiled = compiled.get();
    host.dispatch = database.interner().Intern("wolang$dispatch");
    runtime::InstallResume(&ResumeTask, &host);

    for (Symbol name : lowered->benches) {
      const std::string text(database.interner().Text(name));
      ++ran;
      if (!compiled->Contains(name)) {
        std::fprintf(stdout, "@%-24s interpreted, %s\n", text.c_str(),
                     std::string(compiled->SkipReason(name)).c_str());
        continue;
      }
      // Once to warm the caches, then the best of several runs. The best
      // rather than the mean: the thing being measured is how fast the
      // code can run, and everything noise adds is time it did not spend
      // on that.
      constexpr int kRuns = 5;
      runtime::ResetScheduler();
      compiled->Run(name, {});
      double ms = 0;
      interp::Outcome outcome;
      for (int run = 0; run < kRuns; ++run) {
        // Each run starts with nothing scheduled: a benchmark that spawns
        // would otherwise measure the leftovers of the last one.
        runtime::ResetScheduler();
        const auto start = std::chrono::steady_clock::now();
        outcome = compiled->Run(name, {});
        const auto finish = std::chrono::steady_clock::now();
        const double took =
            std::chrono::duration<double, std::milli>(finish - start)
                .count();
        ms = run == 0 ? took : std::min(ms, took);
      }
      if (!outcome.ok()) {
        std::fprintf(stdout, "@%-24s %s at %s\n", text.c_str(),
                     std::string(interp::TrapKindName(outcome.trap)).c_str(),
                     outcome.where.c_str());
        continue;
      }
      std::fprintf(stdout, "@%-24s %10.3f ms   result %s\n", text.c_str(),
                   ms, outcome.value.Describe().c_str());
    }
  }
  if (ran == 0) {
    std::fprintf(stderr, "wolang: nothing marked `@Bench`\n");
    return 2;
  }
  return 0;
}

int RunModules(const Options& options) {
  if (options.inputs.size() != 1) {
    std::fprintf(stderr, "wolang: modules needs exactly one directory\n");
    return 2;
  }

  Database database;
  DiagnosticEngine diagnostics;
  if (!LoadPackage(&database, options.inputs.front(), &diagnostics)) {
    RenderOptions render;
    render.color = options.color && StderrIsTerminal();
    return Report(database.sources(), diagnostics.diagnostics(), render);
  }

  for (size_t i = 0; i < database.package_count(); ++i) {
    const Package* package = database.GetPackage(PackageId{
        static_cast<uint32_t>(i)});
    std::fprintf(stdout, "package %s %s\n", package->name.c_str(),
                 package->manifest.version.empty()
                     ? "(no version)"
                     : package->manifest.version.c_str());
  }

  for (const Module& module : database.modules()) {
    std::fprintf(stdout, "module %s (%zu file%s)\n", module.name.c_str(),
                 module.files.size(), module.files.size() == 1 ? "" : "s");
    for (FileId file : module.files) {
      std::fprintf(stdout, "    %s\n",
                   std::string(database.sources().Path(file)).c_str());
    }
  }

  RenderOptions render;
  render.color = options.color && StderrIsTerminal();
  return Report(database.sources(), diagnostics.diagnostics(), render);
}

// Generated programs through every tier, compared.
//
// The same check every test in `CodegenTest` makes by hand, with the
// programs written by a generator instead: the interpreter on the IR as
// lowering left it is the reference, the interpreter on the optimized IR
// says whether the passes kept the meaning, and the machine code says
// whether the back end agrees with both. A seed names one program, so a
// disagreement can be looked at again.

// Builds |source| into a binary, runs it, and says what it computed in the
// same words the other tiers use.
//
// A generated program has a `Probe` and no `main`, so a wrapper is added
// that calls it and reports the answer. What comes back is either a value,
// a `trap: ...` -- the binary prints one and exits, since there is nobody
// to report an outcome to -- or a `<...>` saying the comparison itself
// could not be made (D98).
std::string BuildAndRun(const std::string& source, uint64_t seed) {
  std::string wrapped = source;
  wrapped.append(
      "\nextern \"C\" fun wolang_report(value: s64)\n"
      "\nfun main() uses unsafe {\n  wolang_report(Probe() as s64)\n}\n");

  Database database;
  const FileId file = database.AddFile("<fuzz>", wrapped);
  const LoweredFile* lowered = database.Lowered(file);
  const CheckedFile* checked = database.Checked(file);
  if (lowered == nullptr || checked == nullptr) {
    return "<the wrapped program did not lower>";
  }
  if (const std::vector<Diagnostic>* diagnostics =
          database.Diagnostics(file)) {
    for (const Diagnostic& diagnostic : *diagnostics) {
      if (diagnostic.severity == Severity::kError) {
        return "<the wrapped program was rejected: " +
               std::string(diagnostic.code) + ">";
      }
    }
  }

  const std::string stem = codegen::BinaryName((std::filesystem::temp_directory_path() / ("wolang-fuzz-" + std::to_string(ProcessId()) + "-" + std::to_string(seed))).string());
  const codegen::FileCode files[1] = {
      codegen::FileCode{&checked->resolution, &lowered->module}};
  codegen::AotOptions settings;
  settings.object_path = stem + ".o";
  const codegen::AotResult built = codegen::EmitObject(
      database.interner(), database.types(), files, settings);
  if (!built.error.empty()) {
    return "<the backend failed: " + built.error + ">";
  }
  if (!built.skipped.empty()) {
    return "<left out of the binary: " + built.skipped.front() + ">";
  }
#if defined(_WIN32)
  const std::string link = codegen::LinkCommand(settings.object_path, stem) + " > NUL 2>&1";
#else
  const std::string link = codegen::LinkCommand(settings.object_path, stem) + " 2>/dev/null";
#endif
  const int linked = std::system(link.c_str());
  std::remove(settings.object_path.c_str());
  if (linked != 0) {
    return "<the linker failed>";
  }

  std::string said;
  FILE* running = WOLANG_POPEN(("\"" + stem + "\" 2>&1").c_str(), "r");
  if (running != nullptr) {
    char buffer[256];
    while (std::fgets(buffer, sizeof(buffer), running) != nullptr) {
      said.append(buffer);
    }
    WOLANG_PCLOSE(running);
  }
  std::remove(stem.c_str());

  // "value 42" is what it computed; "wolang: out of bounds: ..." is a
  // trap, and the words after the colon are the kind.
  if (said.rfind("value ", 0) == 0) {
    std::string number = said.substr(6);
    while (!number.empty() &&
           (number.back() == '\n' || number.back() == ' ')) {
      number.pop_back();
    }
    return number;
  }
  if (said.rfind("wolang: ", 0) == 0) {
    const size_t end = said.find_first_of(":\n", 8);
    return "trap: " + said.substr(8, end == std::string::npos ? end
                                                              : end - 8);
  }
  return "<the binary said nothing>";
}

int RunFuzz(const Options& options) {
  if (options.fuzz_aot && !codegen::HasAotBackend()) {
    // A silent skip would report success for a comparison never made.
    std::fprintf(stderr,
                 "wolang: --aot needs an ahead-of-time backend and this "
                 "build has none; configure with LLVM to get one\n");
    return 2;
  }
  const uint64_t first = options.fuzz_seed;
  const uint64_t count = options.inputs.empty() ? options.fuzz_count : 1;
  uint64_t checked = 0;
  uint64_t failed = 0;

  for (uint64_t seed = first; seed < first + count; ++seed) {
    fuzz::Options generated;
    generated.seed = seed;
    generated.depth = options.fuzz_depth;
    generated.statements = options.fuzz_statements;
    generated.functions = options.fuzz_statements <= 2 ? 1 : 3;
    std::string source = fuzz::Generate(generated);
    if (!options.inputs.empty()) {
      // A program to compare instead of a generated one: what a
      // disagreement is cut down to by hand or by script (D231).
      std::ifstream in(options.inputs.front(), std::ios::binary);
      std::ostringstream text;
      text << in.rdbuf();
      source = text.str();
    }
    if (options.show_source) {
      std::fprintf(stdout, "// seed %llu\n%s\n",
                   static_cast<unsigned long long>(seed), source.c_str());
    }

    Database database;
    const FileId file = database.AddFile("<fuzz>", source);
    bool rejected = false;
    if (const std::vector<Diagnostic>* diagnostics =
            database.Diagnostics(file)) {
      for (const Diagnostic& diagnostic : *diagnostics) {
        if (diagnostic.severity == Severity::kError) {
          std::fprintf(stdout,
                       "[ failed ] seed %llu: the checker rejected the "
                       "program: %s %s\n",
                       static_cast<unsigned long long>(seed),
                       std::string(diagnostic.code).c_str(),
                       diagnostic.message.c_str());
          rejected = true;
          break;
        }
      }
    }
    if (rejected) {
      ++failed;
      continue;
    }

    const LoweredFile* lowered = database.Lowered(file);
    const CheckedFile* checked_file = database.Checked(file);
    if (lowered == nullptr || checked_file == nullptr) {
      continue;
    }
    if (!lowered->problems.empty()) {
      std::fprintf(stdout, "[ failed ] seed %llu: %s\n",
                   static_cast<unsigned long long>(seed),
                   lowered->problems.front().c_str());
      ++failed;
      continue;
    }

    const Symbol probe = database.interner().Intern("Probe");
    abi::LayoutStore layouts(database.types(), checked_file->resolution);
    database.ShareMembers(&layouts);
    const std::unique_ptr<interp::ForeignCaller> foreign =
        codegen::MakeForeignCaller();
    const interp::Outcome reference = interp::RunNamed(
        database.interner(), database.types(), lowered->unoptimized, probe,
        {}, {}, nullptr, foreign.get(), &layouts);
    // A program the reference tier could not finish says nothing about the
    // other two, and the compiled tier has no step limit to stop it with.
    if (!reference.ok() && reference.trap == ir::TrapKind::kStepLimit) {
      continue;
    }
    const interp::Outcome optimized = interp::RunNamed(
        database.interner(), database.types(), lowered->module, probe, {}, {},
        nullptr, foreign.get(), &layouts);
    ++checked;

    const auto describe = [](const interp::Outcome& outcome) {
      // The kind and the words: two tiers that stop for the same reason
      // with different messages disagree about what happened (D231).
      return outcome.ok() ? outcome.value.Describe()
                          : "trap: " + std::string(
                                           ir::TrapKindName(outcome.trap)) +
                                " (" + outcome.message + ")";
    };
    if (describe(reference) != describe(optimized)) {
      std::fprintf(stdout,
                   "[ failed ] seed %llu: the passes changed the meaning: "
                   "%s then %s\n",
                   static_cast<unsigned long long>(seed),
                   describe(reference).c_str(),
                   describe(optimized).c_str());
      ++failed;
      continue;
    }

    if (!codegen::IsSupportedTarget()) {
      continue;
    }
    std::unique_ptr<codegen::CompiledModule> compiled =
        codegen::Compile(database.interner(), database.types(),
                         checked_file->resolution, lowered->module);
    if (compiled == nullptr || !compiled->Contains(probe)) {
      continue;
    }
    const interp::Outcome machine = compiled->Run(probe, {});
    if (describe(machine) != describe(reference)) {
      std::fprintf(stdout,
                   "[ failed ] seed %llu: the code generator disagrees: "
                   "interpreter said %s, machine code said %s\n",
                   static_cast<unsigned long long>(seed),
                   describe(reference).c_str(), describe(machine).c_str());
      ++failed;
      continue;
    }

    if (options.fuzz_aot && codegen::HasAotBackend()) {
      const std::string said = BuildAndRun(source, seed);
      if (said.rfind("<", 0) == 0) {
        std::fprintf(stdout, "[ failed ] seed %llu: %s\n",
                     static_cast<unsigned long long>(seed), said.c_str());
        ++failed;
        continue;
      }
      // The binary prints its trap in its own words (and cannot say the
      // length an index missed), so only the kind is compared here.
      std::string expected = describe(reference);
      if (const size_t cut = expected.find(" ("); cut != std::string::npos) {
        expected.resize(cut);
      }
      if (said != expected) {
        std::fprintf(stdout,
                     "[ failed ] seed %llu: the binary disagrees: "
                     "interpreter said %s, the binary said %s\n",
                     static_cast<unsigned long long>(seed),
                     describe(reference).c_str(), said.c_str());
        ++failed;
      }
    }
  }

  std::fprintf(stdout, "%llu program%s checked, %llu disagreed\n",
               static_cast<unsigned long long>(checked),
               checked == 1 ? "" : "s",
               static_cast<unsigned long long>(failed));
  return failed == 0 ? 0 : 1;
}

// What every system does and when it can run.
//
// The schedule is a property of the declarations, so it can be printed
// without running anything: each stage is a set of systems that may run at
// the same time, because nothing in it writes what another reads.
int RunSystems(const Options& options) {
  if (options.inputs.size() != 1) {
    std::fprintf(stderr, "wolang: systems needs exactly one directory\n");
    return 2;
  }

  Database database;
  DiagnosticEngine diagnostics;
  std::vector<FileId> files;
  if (!CollectInputs(&database, options, &diagnostics, &files)) {
    RenderOptions render;
    render.color = options.color && StderrIsTerminal();
    return Report(database.sources(), diagnostics.diagnostics(), render);
  }

  RenderOptions render;
  render.color = options.color && StderrIsTerminal();
  std::vector<Diagnostic> all = diagnostics.diagnostics();
  for (FileId file : files) {
    if (const std::vector<Diagnostic>* reported =
            database.Diagnostics(file)) {
      all.insert(all.end(), reported->begin(), reported->end());
    }
  }

  for (size_t i = 0; i < database.modules().size(); ++i) {
    const ModuleId id{static_cast<uint32_t>(i)};
    const AccessGraph* graph = database.Systems(id);
    if (graph == nullptr || graph->systems.empty()) {
      continue;
    }
    const Module* module = database.GetModule(id);
    std::fprintf(stdout, "module %s\n",
                 module == nullptr ? "?" : module->name.c_str());
    for (const SystemAccess& system : graph->systems) {
      std::string line;
      for (const ComponentAccess& access : system.components) {
        line.append(line.empty() ? "" : ", ");
        line.append(access.access == Access::kWrite ? "mut " : "");
        line.append(database.interner().Text(access.component));
      }
      std::fprintf(stdout, "    system %s(%s)\n",
                   std::string(database.interner().Text(system.name)).c_str(),
                   line.c_str());
    }
    for (size_t stage = 0; stage < graph->stages.size(); ++stage) {
      std::string line;
      for (Symbol name : graph->stages[stage]) {
        line.append(line.empty() ? "" : ", ");
        line.append(database.interner().Text(name));
      }
      std::fprintf(stdout, "    stage %zu: %s\n", stage + 1, line.c_str());
    }
  }

  return Report(database.sources(), all, render);
}


// Compiles the program into a binary.
//
// The third tier: the same IR the dev tier runs, through LLVM, into an
// object file -- and then through the system's linker, with the runtime
// archive, into something that needs no compiler to run (D98).
// `wolang new game`: a package that builds, runs and has a test.
//
// What it writes is what a package is: a manifest, a source file, and a
// test next to the code it tests. Not a framework and not a directory
// tree -- the first thing a person does to a new package is read it, and
// four files they can read beats twenty they skip (D132).
int RunNew(const Options& options) {
  const std::string directory = options.inputs.front();
  std::error_code failed;
  if (std::filesystem::exists(directory, failed)) {
    std::fprintf(stderr, "wolang: %s already exists\n", directory.c_str());
    return 2;
  }
  // The package's name is the directory's last part, which is what a
  // person meant by naming the directory.
  std::string name =
      std::filesystem::path(directory).filename().string();
  if (name.empty()) {
    name = "package";
  }
  // A name has to be an identifier: a package called `my-game` would not
  // be nameable in a `need`.
  for (char& held : name) {
    if (!((held >= 'a' && held <= 'z') || (held >= 'A' && held <= 'Z') ||
          (held >= '0' && held <= '9') || held == '_')) {
      held = '_';
    }
  }

  std::filesystem::create_directories(directory + "/src", failed);
  if (failed) {
    std::fprintf(stderr, "wolang: cannot make %s: %s\n", directory.c_str(),
                 failed.message().c_str());
    return 2;
  }

  // Where the standard library is, as this binary can find it. In order:
  // what the environment says, a tree the new package sits inside, the
  // one next to the compiler itself, and the tree this compiler was
  // built from. A relative path when the package is inside the tree --
  // that is what makes the compiler's own examples movable -- and an
  // absolute one otherwise (D132).
  const std::filesystem::path here =
      std::filesystem::absolute(directory, failed);
  std::string standard = StandardLibrary(options, here);
  if (standard.empty()) {
    standard = "../lib/std";
  } else if (std::filesystem::exists(
                 std::filesystem::path(standard) / "wolang.toml", failed)) {
    // A relative path when the package is inside the tree -- that is what
    // makes the compiler's own examples movable -- and an absolute one
    // otherwise (D132).
    const std::filesystem::path relative =
        std::filesystem::relative(standard, here, failed);
    if (!failed && relative.string().rfind("..", 0) == 0 &&
        relative.string().find("../..") != 0) {
      standard = relative.string();
    }
  }

  const auto write = [&](const std::string& path,
                         const std::string& contents) {
    std::ofstream out(directory + "/" + path, std::ios::binary);
    if (!out) {
      return false;
    }
    out << contents;
    return true;
  };

  std::string manifest = "[package]\nname = \"" + name +
                         "\"\nversion = \"0.1.0\"\nsource = \"src\"\n"
                         "\n[dependencies]\nstd = { path = \"" +
                         standard + "\" }\n";
  if (options.new_library) {
    manifest.append("\n# A library's own tests run with `wolang test .`\n");
  }

  std::string source;
  if (options.new_library) {
    source =
        "// " + name + ": what this package offers.\n"
        "//\n"
        "// `offer` is what makes a declaration visible outside this\n"
        "// module; everything else is private to it.\n"
        "\n"
        "/// Adds two numbers. The documentation comment is what\n"
        "/// `wolang doc` writes and what an editor shows on hover.\n"
        "offer fun Add(left: int, right: int) -> int {\n"
        "  return left + right\n"
        "}\n"
        "\n"
        "// ------------------------------------------------------------- "
        "tests ---\n"
        "//\n"
        "// Next to the code they test, and run in every tier: `wolang "
        "test`\n"
        "// runs each one interpreted and compiled and compares the two.\n"
        "\n"
        "@Test\n"
        "fun AddsTwoNumbers() -> bool {\n"
        "  return Add(2, 2) == 4 && Add(-1, 1) == 0\n"
        "}\n";
  } else {
    source =
        "// " + name + "\n"
        "//\n"
        "// `wolang run .` runs this; `wolang test .` runs the test at\n"
        "// the bottom; `wolang build . -o " + name +
        "` makes a binary that\n"
        "// needs no compiler.\n"
        "\n"
        "need std.io.{Print}\n"
        "\n"
        "fun Greeting(name: text) -> text uses unsafe {\n"
        "  return \"hello, {name}\"\n"
        "}\n"
        "\n"
        "fun main() uses io, unsafe {\n"
        "  Print(Greeting(\"world\"))\n"
        "}\n"
        "\n"
        "// ------------------------------------------------------------- "
        "tests ---\n"
        "\n"
        "@Test\n"
        "fun GreetsByName() -> bool uses unsafe {\n"
        "  return Greeting(\"wolang\") == \"hello, wolang\"\n"
        "}\n";
  }

  const std::string ignore =
      "# What a build leaves behind.\n"
      "/target/\n"
      "/build/\n"
      ".wolang-cache/\n";

  const std::string readme =
      "# " + name + "\n"
      "\n"
      "```sh\n" +
      (options.new_library
           ? "wolang test .      # run the tests, in every tier\n"
             "wolang check .     # types, effects, borrows\n"
             "wolang doc . -o docs/api\n"
           : "wolang run .       # run it\n"
             "wolang test .      # run the tests, in every tier\n"
             "wolang build . -o " + name + "   # a binary\n") +
      "```\n";

  if (!write("wolang.toml", manifest) || !write("src/main.wo", source) ||
      !write(".gitignore", ignore) || !write("README.md", readme)) {
    std::fprintf(stderr, "wolang: cannot write into %s\n",
                 directory.c_str());
    return 2;
  }

  std::fprintf(stdout,
               "%s: a %s called `%s`\n"
               "  %s/wolang.toml\n"
               "  %s/src/main.wo\n"
               "\n"
               "  cd %s && wolang %s .\n",
               directory.c_str(),
               options.new_library ? "library" : "program", name.c_str(),
               directory.c_str(), directory.c_str(), directory.c_str(),
               options.new_library ? "test" : "run");
  return 0;
}

// `wolang bind sdl.h`: C declarations as Wolang.
//
// The system's preprocessor expands the header first, because a header is
// C and C has a preprocessor; what comes out of this is declarations for
// a person to edit rather than a binding pretending to be complete
// (D130).
// Writes |text| to |path|, or says why it could not.
static bool WriteText(const std::string& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    std::fprintf(stderr, "wolang: cannot write %s\n", path.c_str());
    return false;
  }
  out << text;
  return true;
}

// `wolang bind --reflection *.reflection.json -o mowo.wo --glue mowo.cpp`:
// a Wolang package's declarations for an engine's reflected classes, and
// the C++ the engine compiles to answer them (D218).
int RunBindReflection(const Options& options) {
  std::vector<std::string> documents;
  for (const std::string& input : options.inputs) {
    std::ifstream file(input, std::ios::binary);
    if (!file) {
      std::fprintf(stderr, "wolang: cannot read %s\n", input.c_str());
      return 2;
    }
    documents.emplace_back(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  }

  bind::ReflectionOptions settings;
  settings.module = options.bind_module;
  settings.effect = options.bind_effect;
  const bind::ReflectionResult result = bind::GenerateFromReflection(documents, settings);
  if (options.output.empty()) {
    WriteAll(result.source, stdout);
  }
  else if (!WriteText(options.output, result.source)) {
    return 2;
  }
  if (!options.bind_glue.empty() && !WriteText(options.bind_glue, result.glue)) {
    return 2;
  }

  std::fprintf(stderr, "wolang: %d classes, %d functions, %d properties\n", result.classes, result.functions, result.properties);
  for (const std::string& skipped : result.skipped) {
    std::fprintf(stderr, "  skipped %s\n", skipped.c_str());
  }
  return 0;
}

int RunBind(const Options& options) {
  if (options.bind_reflection) {
    return RunBindReflection(options);
  }
  const std::string& path = options.inputs.front();
  bind::Options settings;
  settings.header = path;
  settings.prefix = options.bind_prefix;

  std::string text = bind::Preprocess(path, options.bind_includes);
  bool expanded = !text.empty();
  if (!expanded) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
      std::fprintf(stderr, "wolang: cannot read %s\n", path.c_str());
      return 2;
    }
    text.assign(std::istreambuf_iterator<char>(file),
                std::istreambuf_iterator<char>());
  }

  const bind::Result result = bind::Generate(text, settings);
  if (!options.output.empty()) {
    std::ofstream out(options.output, std::ios::binary);
    if (!out) {
      std::fprintf(stderr, "wolang: cannot write %s\n",
                   options.output.c_str());
      return 2;
    }
    out << result.source;
  } else {
    WriteAll(result.source, stdout);
  }

  std::fprintf(stderr,
               "wolang: %d functions, %d constants, %d aliases%s\n",
               result.functions, result.constants, result.aliases,
               expanded ? "" : " (read without the preprocessor)");
  for (const std::string& skipped : result.skipped) {
    std::fprintf(stderr, "  skipped %s\n", skipped.c_str());
  }
  return 0;
}

int RunBuild(const Options& options) {
  if (!codegen::HasAotBackend()) {
    std::fprintf(stderr,
                 "wolang: this build has no ahead-of-time backend; "
                 "configure with LLVM to get one\n");
    return 2;
  }
  Database database;
  DiagnosticEngine load_diagnostics;
  std::vector<FileId> files;
  if (!CollectInputs(&database, options, &load_diagnostics, &files)) {
    return 2;
  }
  RenderOptions render;
  render.color = options.color && StderrIsTerminal();
  std::vector<Diagnostic> all = load_diagnostics.diagnostics();
  for (FileId file : files) {
    if (const std::vector<Diagnostic>* diagnostics =
            database.Diagnostics(file)) {
      all.insert(all.end(), diagnostics->begin(), diagnostics->end());
    }
  }
  if (const int reported = Report(database.sources(), all, render);
      reported != 0) {
    return reported;
  }

  const std::vector<codegen::FileCode> code = CollectCode(&database, files);
  if (code.empty()) {
    std::fprintf(stderr, "wolang: nothing to build\n");
    return 2;
  }

  std::string binary = options.output.empty() ? "a.out" : options.output;
  const std::string object =
      options.emit_object ? binary : binary + ".o";
  if (!options.emit_object && (options.target.empty() || options.target == codegen::HostTriple())) {
    binary = codegen::BinaryName(binary);
  }

  codegen::AotOptions settings;
  settings.object_path = object;
  settings.optimization = options.optimization;
  settings.triple = options.target;
  // Line information unless the build asked for none: stepping through
  // Wolang in a debugger is worth more than a smaller object, and
  // `--no-debug` is there for when it is not (D118).
  if (!options.no_debug) {
    settings.sources = &database.sources();
  }
  // A library for a host to link: the files the inputs named export what
  // they offer, and their dependencies stay inside (D219).
  if (!options.library.empty()) {
    settings.library = options.library;
    for (FileId file : files) {
      const std::string where(database.sources().Path(file));
      for (const std::string& input : options.inputs) {
        std::error_code failed;
        const std::string root = std::filesystem::weakly_canonical(input, failed).string();
        const std::string path = std::filesystem::weakly_canonical(where, failed).string();
        if (!root.empty() && path.rfind(root, 0) == 0) {
          settings.exported.push_back(file.index);
        }
      }
    }
  }
  const codegen::AotResult result = codegen::EmitObject(
      database.interner(), database.types(), code, settings);
  if (!result.error.empty()) {
    std::fprintf(stderr, "wolang: %s\n", result.error.c_str());
    return 1;
  }
  // Said rather than discovered at run time: a binary missing a body is
  // incomplete, and which bodies is what makes it worth fixing.
  for (const std::string& left : result.skipped) {
    std::fprintf(stderr, "wolang: left out of the binary: %s\n",
                 left.c_str());
  }
  if (options.emit_object) {
    std::fprintf(stdout, "%s\n", object.c_str());
    return 0;
  }

  // A binary for another machine needs that machine's runtime and that
  // machine's linker, and this build has neither: what it has is the
  // object, which is the part only this compiler can produce. Said
  // plainly rather than handed to a linker that would fail with
  // something about architectures (D160).
  if (!options.target.empty() &&
      options.target != codegen::HostTriple()) {
    std::fprintf(stderr,
                 "wolang: wrote %s for %s\n"
                 "wolang: linking it needs that machine's runtime and "
                 "linker; build the runtime there, or link with\n"
                 "wolang:   clang --target=%s %s "
                 "<libwolang_runtime.a for that machine> -o <binary>\n",
                 object.c_str(), options.target.c_str(),
                 options.target.c_str(), object.c_str());
    return 0;
  }

  const std::string_view archive = codegen::RuntimeArchive();
  if (archive.empty()) {
    std::fprintf(stderr,
                 "wolang: this build did not record where the runtime "
                 "archive is\n");
    return 1;
  }
  // The system's linker, through its C driver: it knows where the C++
  // runtime and the platform's libraries are, and teaching this one that
  // would be a worse copy of it.
  std::string command = codegen::LinkCommand(object, binary);
  // What the packages said they need, from their `[link]` tables: a
  // package binding a C library is the one that knows which library, so
  // the manifest carries it and the link line is assembled here (D129).
  for (size_t i = 0; i < database.package_count(); ++i) {
    const Package* held = database.GetPackage(PackageId{
        static_cast<uint32_t>(i)});
    if (held == nullptr) {
      continue;
    }
    const Package& package = *held;
    for (const std::string& path : package.manifest.link_paths) {
      command.append(codegen::LinkPathFlag(path));
    }
    for (const std::string& library : package.manifest.link_libraries) {
      command.append(codegen::LinkLibraryFlag(library));
    }
    for (const std::string& framework : package.manifest.link_frameworks) {
      command.append(" -framework ");
      command.append(framework);
    }
  }
  const int linked = std::system(command.c_str());
  if (linked != 0) {
    std::remove(object.c_str());
    std::fprintf(stderr, "wolang: the linker failed\n");
    return 1;
  }
  // On this platform the line table stays in the object file and the
  // binary holds a map into it, so a debugger needs either the object or
  // a `.dSYM` beside the binary. The second one is what survives the
  // object being deleted (D118).
#if defined(__APPLE__)
  if (!options.no_debug) {
    std::string bundle = "dsymutil '";
    bundle.append(binary);
    bundle.append("' 2>/dev/null");
    const int collected = std::system(bundle.c_str());
    (void)collected;
  }
#endif
  std::remove(object.c_str());
  std::fprintf(stdout, "%s\n", binary.c_str());
  return 0;
}

// `wolang debug <package>`: build it with line information and start a
// debugger on it.
//
// Not a debugger of its own: the binary carries DWARF (D118), so lldb
// and gdb already know where every line is. What this does is the part a
// person should not have to remember -- build with the right flags, then
// hand the binary over with the source root set (D118).
int RunDebug(const Options& options) {
  Options building = options;
  building.command = "build";
  building.optimization = 0;
  if (building.output.empty()) {
    const std::filesystem::path where(options.inputs.front());
    std::error_code failed;
    const std::filesystem::path canonical =
        std::filesystem::weakly_canonical(where, failed);
    building.output =
        (std::filesystem::temp_directory_path() /
         ("wolang-debug-" + canonical.filename().string()))
            .string();
  }
  if (const int built = RunBuild(building); built != 0) {
    return built;
  }

  // Which debugger: whatever is there. lldb first on macOS, gdb first
  // elsewhere, and a clear message when neither is.
  const char* first = nullptr;
#if defined(__APPLE__)
  const char* candidates[] = {"lldb", "gdb"};
#else
  const char* candidates[] = {"gdb", "lldb"};
#endif
  for (const char* candidate : candidates) {
    std::string probe = "command -v ";
    probe.append(candidate);
    probe.append(" >/dev/null 2>&1");
    if (std::system(probe.c_str()) == 0) {
      first = candidate;
      break;
    }
  }
  if (first == nullptr) {
    std::fprintf(stderr,
                 "wolang: no lldb or gdb on the path; the binary is at "
                 "%s\n",
                 building.output.c_str());
    return 1;
  }

  std::fprintf(stdout,
               "%s %s\n"
               "  `b main.wo:12` puts a breakpoint on a line; `run` "
               "starts it.\n",
               first, building.output.c_str());
  std::string command(first);
  command.append(" '");
  command.append(building.output);
  command.append("'");
  for (size_t i = 1; i < options.inputs.size(); ++i) {
    command.append(" ");
    command.append(options.inputs[i]);
  }
  return std::system(command.c_str()) == 0 ? 0 : 1;
}



// ------------------------------------------------------------- `doc` ---

// The `///` lines of a declaration, with the slashes taken off.
std::string DocText(const SourceManager& sources, SourceSpan span) {
  if (!span.is_valid()) {
    return {};
  }
  std::string text;
  const std::string_view whole = sources.SpanText(span);
  size_t at = 0;
  while (at <= whole.size()) {
    const size_t stop = whole.find('\n', at);
    const std::string_view line =
        whole.substr(at, stop == std::string_view::npos ? stop : stop - at);
    at = stop == std::string_view::npos ? whole.size() + 1 : stop + 1;
    std::string_view trimmed = line;
    while (!trimmed.empty() &&
           (trimmed.front() == ' ' || trimmed.front() == '\t')) {
      trimmed.remove_prefix(1);
    }
    if (trimmed.rfind("///", 0) == 0) {
      trimmed.remove_prefix(3);
    }
    if (!trimmed.empty() && trimmed.front() == ' ') {
      trimmed.remove_prefix(1);
    }
    text.append(trimmed);
    text.push_back('\n');
  }
  return text;
}

// What a declaration looks like, as the program wrote it: everything from
// its name up to the body.
//
// Taken from the source rather than rebuilt from the syntax tree,
// because the program's own spelling is the documentation -- a default
// value, a `where` clause and the order of the effects all say something
// (D116).
std::string Signature(const SourceManager& sources,
                      const Ast& ast,
                      const Item& item) {
  SourceSpan span = item.span;
  if (!span.is_valid()) {
    return {};
  }
  uint32_t end = span.end;
  if (item.body.is_valid() && ast.Contains(item.body)) {
    const SourceSpan body = ast.Get(item.body).span;
    if (body.is_valid() && body.begin > span.begin) {
      end = body.begin;
    }
  } else if (!ast.Get(item.members).empty()) {
    // A type: up to where its first member is.
    const ItemId first = ast.Get(item.members).front();
    if (ast.Contains(first) && ast.Get(first).span.is_valid()) {
      end = ast.Get(first).span.begin;
    }
  }
  uint32_t begin = span.begin;
  // The doc comment is part of the declaration's span; the signature
  // starts after it.
  if (item.doc.is_valid() && item.doc.end > begin && item.doc.end <= end) {
    begin = item.doc.end;
  }
  const std::string_view text =
      sources.SpanText(SourceSpan{span.file, begin, end});
  std::string signature(text);
  // One line, and nothing hanging off the end: a signature that spans
  // lines keeps its line breaks, and a trailing `{` or `,` is noise.
  while (!signature.empty() &&
         (signature.back() == ' ' || signature.back() == '\n' ||
          signature.back() == '\t' || signature.back() == '{' ||
          signature.back() == '\r')) {
    signature.pop_back();
  }
  size_t start = 0;
  while (start < signature.size() &&
         (signature[start] == ' ' || signature[start] == '\n' ||
          signature[start] == '\t')) {
    ++start;
  }
  return signature.substr(start);
}

const char* KindWord(ItemKind kind) {
  switch (kind) {
    case ItemKind::kFunction:
      return "function";
    case ItemKind::kStruct:
      return "struct";
    case ItemKind::kClass:
      return "class";
    case ItemKind::kAbility:
      return "ability";
    case ItemKind::kEnum:
      return "enum";
    case ItemKind::kComponent:
      return "component";
    case ItemKind::kBinding:
      return "constant";
    case ItemKind::kTypeAlias:
      return "type";
    case ItemKind::kSystem:
      return "system";
    case ItemKind::kSignal:
      return "signal";
    case ItemKind::kErrorSet:
      return "error";
    case ItemKind::kMachine:
      return "machine";
    default:
      return "declaration";
  }
}

// Writes what one module offers.
void DocumentModule(const Database& database,
                    const Module& module,
                    std::string* out) {
  out->append("\n## ");
  out->append(module.name);
  out->append("\n");

  for (const FileId file : module.files) {
    const ParsedFile* parsed =
        const_cast<Database&>(database).Parse(file);
    if (parsed == nullptr) {
      continue;
    }
    const Ast& ast = parsed->ast;
    for (const ItemId id : ast.Get(ast.root())) {
      if (!ast.Contains(id)) {
        continue;
      }
      const Item& item = ast.Get(id);
      // What another module can name, which is what documentation is
      // about: a private helper is an implementation detail by
      // definition.
      if (!HasFlag(item.flags, ItemFlags::kOffer)) {
        continue;
      }
      out->append("\n### ");
      out->append(KindWord(item.kind));
      out->append(" `");
      out->append(database.interner().Text(item.name));
      out->append("`\n\n```wolang\n");
      out->append(Signature(database.sources(), ast, item));
      out->append("\n```\n");
      const std::string doc = DocText(database.sources(), item.doc);
      if (!doc.empty()) {
        out->append("\n");
        out->append(doc);
      }

      // The members a type offers: a method with no `priv` is part of
      // the surface, because a type's members are offered with it.
      for (const ItemId member_id : ast.Get(item.members)) {
        if (!ast.Contains(member_id)) {
          continue;
        }
        const Item& member = ast.Get(member_id);
        if (HasFlag(member.flags, ItemFlags::kPriv) ||
            !member.name.is_valid()) {
          continue;
        }
        out->append("\n#### ");
        out->append(member.kind == ItemKind::kBinding ? "field" : "method");
        out->append(" `");
        out->append(database.interner().Text(member.name));
        out->append("`\n\n```wolang\n");
        out->append(Signature(database.sources(), ast, member));
        out->append("\n```\n");
        const std::string member_doc =
            DocText(database.sources(), member.doc);
        if (!member_doc.empty()) {
          out->append("\n");
          out->append(member_doc);
        }
      }
    }
  }
}

// `wolang doc`: what a package offers, as Markdown.
//
// Markdown because it reads as text and renders where code lives, and
// from the *source* spans because the program's own spelling of a
// signature is the documentation (D116).
int RunDoc(const Options& options) {
  if (options.inputs.size() != 1) {
    std::fprintf(stderr, "wolang: doc needs exactly one directory\n");
    return 2;
  }
  Database database;
  DiagnosticEngine diagnostics;
  if (!LoadPackage(&database, options.inputs.front(), &diagnostics)) {
    RenderOptions render;
    render.color = options.color && StderrIsTerminal();
    return Report(database.sources(), diagnostics.diagnostics(), render);
  }

  // The package the directory names, not its dependencies: a package's
  // documentation is what *it* offers, and `std` has its own page.
  PackageId wanted;
  for (size_t i = 0; i < database.package_count(); ++i) {
    const PackageId id{static_cast<uint32_t>(i)};
    const Package* package = database.GetPackage(id);
    if (package == nullptr) {
      continue;
    }
    if (package->directory.find(options.inputs.front()) !=
            std::string::npos ||
        options.inputs.front().find(package->directory) !=
            std::string::npos) {
      wanted = id;
    }
  }

  std::string out;
  if (const Package* package = database.GetPackage(wanted);
      package != nullptr) {
    out.append("# ");
    out.append(package->name);
    if (!package->manifest.version.empty()) {
      out.append(" ");
      out.append(package->manifest.version);
    }
    out.append("\n");
  }

  for (const Module& module : database.modules()) {
    if (wanted.is_valid() && module.package != wanted) {
      continue;
    }
    DocumentModule(database, module, &out);
  }

  if (options.output.empty()) {
    std::fwrite(out.data(), 1, out.size(), stdout);
    return 0;
  }
  std::FILE* file = std::fopen(options.output.c_str(), "wb");
  if (file == nullptr) {
    std::fprintf(stderr, "wolang: cannot write %s\n",
                 options.output.c_str());
    return 1;
  }
  std::fwrite(out.data(), 1, out.size(), file);
  std::fclose(file);
  std::fprintf(stdout, "%s\n", options.output.c_str());
  return 0;
}

int Run(int argc, char** argv) {
  Options options;
  options.program = argc > 0 ? argv[0] : "wolang";
  // Everything after `--` belongs to the program being run, not to the
  // driver: `wolang run app -- --port 9000` passes two arguments on
  // (D115).
  int mine = argc;
  for (int i = 1; i < argc; ++i) {
    if (std::string_view(argv[i]) == "--") {
      mine = i;
      runtime::wolang_args_set(argc - i - 1, argv + i + 1);
      break;
    }
  }
  for (int i = 1; i < mine; ++i) {
    const std::string_view argument = argv[i];
    if (argument == "--aot") {
      options.fuzz_aot = true;
      continue;
    }
    if (argument == "--raw") {
      options.raw = true;
      continue;
    }
    if (argument == "--emit-object") {
      options.emit_object = true;
      continue;
    }
    if (argument == "--library" && i + 1 < argc) {
      options.library = argv[++i];
      options.emit_object = true;
      continue;
    }
    if (argument == "--target" && i + 1 < argc) {
      options.target = argv[++i];
      continue;
    }
    if (argument.rfind("--target=", 0) == 0) {
      options.target = std::string(argument.substr(9));
      continue;
    }
    if (argument == "--no-debug") {
      options.no_debug = true;
      continue;
    }
    if (argument == "-o" && i + 1 < argc) {
      options.output = argv[++i];
      continue;
    }
    if (argument.size() == 3 && argument.rfind("-O", 0) == 0 &&
        argument[2] >= '0' && argument[2] <= '3') {
      options.optimization = argument[2] - '0';
      continue;
    }
    if (argument == "--prefix" && i + 1 < argc) {
      options.bind_prefix = argv[++i];
    } else if (argument == "--reflection") {
      options.bind_reflection = true;
    } else if (argument == "--glue" && i + 1 < argc) {
      options.bind_glue = argv[++i];
    } else if (argument == "--module" && i + 1 < argc) {
      options.bind_module = argv[++i];
    } else if (argument == "--effect" && i + 1 < argc) {
      options.bind_effect = argv[++i];
    } else if (argument == "-I" && i + 1 < argc) {
      options.bind_includes.push_back(argv[++i]);
    } else if (argument.rfind("-I", 0) == 0 && argument.size() > 2) {
      options.bind_includes.push_back(std::string(argument.substr(2)));
    } else if (argument == "--stdin") {
      options.from_stdin = true;
    } else if (argument == "--unoptimized") {
      options.unoptimized = true;
    } else if (argument == "--source") {
      options.show_source = true;
    } else if (argument == "--filter" && i + 1 < argc) {
      options.test_filter = argv[++i];
    } else if (argument == "--seed" && i + 1 < argc) {
      options.fuzz_seed = std::strtoull(argv[++i], nullptr, 10);
    } else if (argument == "--depth" && i + 1 < argc) {
      options.fuzz_depth = std::atoi(argv[++i]);
    } else if (argument == "--statements" && i + 1 < argc) {
      options.fuzz_statements = std::atoi(argv[++i]);
    } else if (argument == "--count" && i + 1 < argc) {
      options.fuzz_count = std::strtoull(argv[++i], nullptr, 10);
    } else if (argument == "--no-color") {
      options.color = false;
    } else if (argument == "--trivia") {
      options.show_trivia = true;
    } else if (argument == "--spans") {
      options.show_spans = true;
    } else if (argument == "--check") {
      options.check_only = true;
    } else if (argument == "--write") {
      options.write_in_place = true;
    } else if (argument == "--no-cache") {
      options.no_cache = true;
    } else if (argument == "--interpret") {
      options.interpret = true;
    } else if (argument == "--version" || argument == "-V" ||
               argument == "--help" || argument == "-h") {
      // Asked for before a command, which is where a person types it.
      if (options.command.empty()) {
        options.command = std::string(argument);
      }
    } else if (argument == "--lib") {
      options.new_library = true;
    } else if (argument == "--all") {
      options.test_all = true;
    } else if (argument == "--record" && i + 1 < argc) {
      options.record = argv[++i];
    } else if (argument == "--replay" && i + 1 < argc) {
      options.replay = argv[++i];
    } else if (argument == "--profile") {
      options.profile = true;
    } else if (argument == "--hertz" && i + 1 < argc) {
      options.hertz = static_cast<int>(std::strtol(argv[++i], nullptr, 10));
    } else if (argument.size() > 1 && argument[0] == '-') {
      std::fprintf(stderr, "wolang: unknown option %.*s\n",
                   static_cast<int>(argument.size()), argument.data());
      return 2;
    } else if (options.command.empty()) {
      options.command = std::string(argument);
    } else {
      options.inputs.push_back(std::string(argument));
    }
  }

  if (options.command == "--version" || options.command == "-V" ||
      options.command == "version") {
    std::fprintf(stdout, "wolang %s\n", std::string(kVersion).c_str());
    return 0;
  }
  if (options.command == "--help" || options.command == "-h") {
    PrintUsage();
    return 0;
  }
  if (options.command.empty() || options.command == "help") {
    PrintUsage();
    return options.command.empty() ? 2 : 0;
  }
  if (options.command == "version") {
    std::fprintf(stdout, "wolang %s\n", std::string(kVersion).c_str());
    return 0;
  }

  const bool needs_inputs =
      (options.command == "fmt" && !options.from_stdin) ||
      options.command == "check" || options.command == "fix" ||
      options.command == "tokens" ||
      options.command == "ast" ||
      options.command == "ir" || options.command == "modules" ||
      options.command == "systems" ||
      options.command == "run" || options.command == "test" ||
      options.command == "asm" || options.command == "bench" ||
      options.command == "build" || options.command == "doc" ||
      options.command == "bind" || options.command == "new" ||
      options.command == "fetch" || options.command == "debug";
  if (needs_inputs && options.inputs.empty()) {
    std::fprintf(stderr, "wolang: %s needs at least one path\n",
                 options.command.c_str());
    return 2;
  }

  if (options.command == "check") {
    return RunCheck(options);
  }
  if (options.command == "tokens" || options.command == "ast" ||
      options.command == "ir") {
    return RunSyntaxCommand(options);
  }
  if (options.command == "fix") {
    return RunFix(options);
  }
  if (options.command == "fmt") {
    return RunFormat(options);
  }
  if (options.command == "run") {
    return RunProgram(options);
  }
  if (options.command == "build") {
    return RunBuild(options);
  }
  if (options.command == "test") {
    return RunTests(options);
  }
  if (options.command == "asm") {
    return RunAsm(options);
  }
  if (options.command == "bench") {
    return RunBench(options);
  }
  if (options.command == "modules") {
    return RunModules(options);
  }
  if (options.command == "doc") {
    return RunDoc(options);
  }
  if (options.command == "bind") {
    return RunBind(options);
  }
  if (options.command == "new") {
    return RunNew(options);
  }
  if (options.command == "repl") {
    return RunRepl(options);
  }
  if (options.command == "fetch") {
    return RunFetch(options);
  }
  if (options.command == "debug") {
    return RunDebug(options);
  }
  if (options.command == "dap") {
    return RunDap(options);
  }
  if (options.command == "lsp") {
    // The editor's end of the compiler: the same queries `check` and
    // `fmt` use, over a pipe (D117).
    return lsp::Serve();
  }
  if (options.command == "systems") {
    return RunSystems(options);
  }
  if (options.command == "fuzz") {
    return RunFuzz(options);
  }

  std::fprintf(stderr, "wolang: unknown command '%s'\n",
               options.command.c_str());
  PrintUsage();
  return 2;
}

}  // namespace
}  // namespace wolang

int main(int argc, char** argv) {
  return wolang::Run(argc, argv);
}
