// Copyright 2026 The Wolang Authors. All rights reserved.
//
// The wolang command line driver. Only the commands that the current wave
// supports are implemented; see docs/roadmap.md for what each wave adds.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#include "wolang/base/diagnostic.h"
#include "wolang/base/diagnostic_renderer.h"
#include "wolang/base/source_manager.h"
#include "wolang/base/string_interner.h"
#include "wolang/compiler/cache.h"
#include "wolang/compiler/database.h"
#include "wolang/codegen/aot.h"
#include "wolang/codegen/compiler.h"
#include "wolang/abi/layout.h"
#include "wolang/codegen/foreign.h"
#include "wolang/fuzz/generator.h"
#include "wolang/runtime/scheduler.h"
#include "wolang/compiler/workspace.h"
#include "wolang/format/formatter.h"
#include "wolang/interp/interpreter.h"
#include "wolang/ir/printer.h"
#include "wolang/syntax/ast.h"
#include "wolang/syntax/ast_printer.h"
#include "wolang/syntax/lexer.h"
#include "wolang/syntax/parser.h"

namespace wolang {
namespace {

constexpr std::string_view kVersion = "0.1.0";

struct Options {
  std::string command;
  std::vector<std::string> inputs;
  bool color = true;
  // `run` only: stay on the interpreter even where the dev tier could take
  // the program. Useful for telling a code generator bug from a program
  // bug, which is otherwise the hardest thing to tell apart.
  bool interpret = false;
  bool show_trivia = false;
  bool show_spans = false;
  // `fuzz` only: which program to start from, how many, and whether to
  // print what was generated -- which is the first thing wanted when one
  // of them disagrees.
  uint64_t fuzz_seed = 1;
  uint64_t fuzz_count = 100;
  bool show_source = false;
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
  // file, and how hard the back end works.
  std::string output;
  bool emit_object = false;
  int optimization = 2;
};

bool StderrIsTerminal() {
#if defined(_WIN32)
  return _isatty(_fileno(stderr)) != 0;
#else
  return isatty(fileno(stderr)) != 0;
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
      "  systems <dir>   print what systems access and when they run\n"
      "  fuzz            generate programs and compare the tiers\n"
      "  version         print the version and exit\n"
      "  help            print this message\n"
      "\n"
      "options:\n"
      "  --seed <n>      the first seed `fuzz` generates from\n"
      "  --count <n>     how many programs `fuzz` generates\n"
      "  --source        print the programs `fuzz` generated\n"
      "  --unoptimized   `ir`: print what lowering produced, before passes\n"
      "  -o <path>       `build`: where the binary goes\n"
      "  --emit-object   `build`: stop at the object file\n"
      "  -O<n>           `build`: how hard the back end optimizes, 0 to 3\n"
      "  --no-color      never emit ANSI colors\n"
      "  --trivia        include comments in `tokens` output\n"
      "  --spans         annotate `ast` output with positions\n"
      "  --check         `fmt` only: fail if a file is not formatted\n"
      "  --write         `fmt` only: rewrite the file in place\n"
      "  --no-cache      ignore and do not write the on-disk cache\n"
      "  --interpret     `run` only: do not use the compiled tier\n",
      std::string(kVersion).c_str());
}

int Report(const SourceManager& sources,
           const std::vector<Diagnostic>& diagnostics,
           const RenderOptions& options) {
  if (!diagnostics.empty()) {
    const std::string text = RenderDiagnostics(sources, diagnostics, options);
    std::fputs(text.c_str(), stderr);
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

    std::string error;
    const FileId file = database->LoadFile(input, &error);
    if (!file.is_valid()) {
      std::fprintf(stderr, "wolang: %s\n", error.c_str());
      return false;
    }
    files->push_back(file);
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
        std::fputs(text.c_str(), stdout);
      }
    }

    const std::vector<Diagnostic>* diagnostics = database.Diagnostics(file);
    if (diagnostics != nullptr) {
      all.insert(all.end(), diagnostics->begin(), diagnostics->end());
    }
  }
  return Report(database.sources(), all, render);
}

int RunFormat(const Options& options) {
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

    const std::string formatted =
        Format(database.sources(), database.interner(), tokens->tokens,
               parsed->ast);
    const std::string_view original = database.sources().Text(file);

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

    std::fputs(formatted.c_str(), stdout);
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
std::string Describe(const interp::Outcome& outcome) {
  if (outcome.ok()) {
    return outcome.value.Describe();
  }
  std::string result(interp::TrapKindName(outcome.trap));
  result.append(" at ");
  result.append(outcome.where);
  return result;
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
  if (compiled != nullptr && compiled->Contains(main)) {
    outcome = compiled->Run(main, {});
  } else {
    // The interpreter needs the layout to read a value it only has the
    // address of, which a task frame is.
    outcome = interp::RunNamed(database.interner(), database.types(),
                               program->module, main, {}, {}, &output,
                               foreign.get(), &program_layouts, modules);
  }

  std::fputs(output.text.c_str(), stdout);
  if (!outcome.ok()) {
    std::fprintf(stderr, "wolang: %s at %s: %s\n",
                 std::string(interp::TrapKindName(outcome.trap)).c_str(),
                 outcome.where.c_str(), outcome.message.c_str());
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

  int ran = 0;
  int failed = 0;
  for (FileId file : files) {
    const LoweredFile* lowered = database.Lowered(file);
    if (lowered == nullptr) {
      continue;
    }
    for (Symbol name : lowered->tests) {
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
int RunFuzz(const Options& options) {
  const uint64_t first = options.fuzz_seed;
  const uint64_t count = options.fuzz_count;
  uint64_t checked = 0;
  uint64_t failed = 0;

  for (uint64_t seed = first; seed < first + count; ++seed) {
    fuzz::Options generated;
    generated.seed = seed;
    generated.depth = options.fuzz_depth;
    generated.statements = options.fuzz_statements;
    generated.functions = options.fuzz_statements <= 2 ? 1 : 3;
    const std::string source = fuzz::Generate(generated);
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
      return outcome.ok() ? outcome.value.Describe()
                          : "trap: " + std::string(
                                           ir::TrapKindName(outcome.trap));
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

  codegen::AotOptions settings;
  settings.object_path = object;
  settings.optimization = options.optimization;
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
  std::string command = "c++ -o '";
  command.append(binary);
  command.append("' '");
  command.append(object);
  command.append("' '");
  command.append(archive);
  command.append("' -lpthread");
  const int linked = std::system(command.c_str());
  std::remove(object.c_str());
  if (linked != 0) {
    std::fprintf(stderr, "wolang: the linker failed\n");
    return 1;
  }
  std::fprintf(stdout, "%s\n", binary.c_str());
  return 0;
}

int Run(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument = argv[i];
    if (argument == "--emit-object") {
      options.emit_object = true;
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
    if (argument == "--unoptimized") {
      options.unoptimized = true;
    } else if (argument == "--source") {
      options.show_source = true;
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

  if (options.command.empty() || options.command == "help") {
    PrintUsage();
    return options.command.empty() ? 2 : 0;
  }
  if (options.command == "version") {
    std::fprintf(stdout, "wolang %s\n", std::string(kVersion).c_str());
    return 0;
  }

  const bool needs_inputs =
      options.command == "check" || options.command == "tokens" ||
      options.command == "ast" || options.command == "fmt" ||
      options.command == "ir" || options.command == "modules" ||
      options.command == "systems" ||
      options.command == "run" || options.command == "test" ||
      options.command == "asm" || options.command == "bench" ||
      options.command == "build";
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
