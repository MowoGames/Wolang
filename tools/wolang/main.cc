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
#include "wolang/codegen/compiler.h"
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
  bool show_trivia = false;
  bool show_spans = false;
  // `fmt` only: verify instead of printing, or rewrite the file in place.
  bool check_only = false;
  bool write_in_place = false;
  // Skip the on-disk cache entirely, which is how a suspected cache problem
  // is ruled out.
  bool no_cache = false;
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
      "  bench <paths>   run and time every function marked `@Bench`\n"
      "  test <paths>    run every function marked `@Test`\n"
      "  modules <dir>   print the module graph of a package\n"
      "  version         print the version and exit\n"
      "  help            print this message\n"
      "\n"
      "options:\n"
      "  --no-color      never emit ANSI colors\n"
      "  --trivia        include comments in `tokens` output\n"
      "  --spans         annotate `ast` output with positions\n"
      "  --check         `fmt` only: fail if a file is not formatted\n"
      "  --write         `fmt` only: rewrite the file in place\n"
      "  --no-cache      ignore and do not write the on-disk cache\n",
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
      for (const Module& module : database->modules()) {
        for (FileId file : module.files) {
          files->push_back(file);
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
        std::fputs(ir::PrintModule(database.interner(), database.types(),
                                   lowered->module)
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
  interp::Output reference_output;
  interp::Output optimized_output;
  const interp::Outcome reference = interp::RunNamed(
      database.interner(), database.types(), program->unoptimized, main, {},
      {}, &reference_output);
  const interp::Outcome optimized = interp::RunNamed(
      database.interner(), database.types(), program->module, main, {}, {},
      &optimized_output);

  // What the program printed is part of what it does, so the two tiers have
  // to agree about that too.
  if (reference.trap != optimized.trap ||
      reference_output.text != optimized_output.text ||
      (reference.ok() && reference.value != optimized.value)) {
    // A difference here is a miscompile, which is the most serious kind of
    // bug this compiler can have.
    std::fprintf(stderr,
                 "wolang: internal: the passes changed the meaning of the "
                 "program\n  before: %s\n  after:  %s\n",
                 Describe(reference).c_str(), Describe(optimized).c_str());
    return 70;
  }

  std::fputs(reference_output.text.c_str(), stdout);
  if (!reference.ok()) {
    std::fprintf(stderr, "wolang: %s at %s: %s\n",
                 std::string(interp::TrapKindName(reference.trap)).c_str(),
                 reference.where.c_str(), reference.message.c_str());
    return 1;
  }
  if (reference.value.kind() != interp::Value::Kind::kVoid) {
    std::fprintf(stdout, "%s\n", reference.value.Describe().c_str());
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

  int ran = 0;
  int failed = 0;
  for (FileId file : files) {
    const LoweredFile* lowered = database.Lowered(file);
    if (lowered == nullptr) {
      continue;
    }
    // Every test is also a differential check between the tiers: the same
    // function, compiled and interpreted, has to reach the same answer.
    // Tests are the one place a program is known to terminate, which is
    // what makes running it twice safe.
    std::unique_ptr<codegen::CompiledModule> compiled;
    const CheckedFile* checked = database.Checked(file);
    if (checked != nullptr && codegen::IsSupportedTarget()) {
      compiled = codegen::Compile(database.interner(), database.types(),
                                  checked->resolution, lowered->module);
    }

    for (Symbol name : lowered->tests) {
      ++ran;
      // Run through the unoptimized IR: the reference semantics are what a
      // test is testing.
      const interp::Outcome outcome = interp::RunNamed(
          database.interner(), database.types(), lowered->unoptimized, name);
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

  for (FileId file : files) {
    const LoweredFile* lowered = database.Lowered(file);
    const CheckedFile* checked = database.Checked(file);
    if (lowered == nullptr || checked == nullptr) {
      continue;
    }
    const std::unique_ptr<codegen::CompiledModule> compiled =
        codegen::Compile(database.interner(), database.types(),
                         checked->resolution, lowered->module);
    std::fprintf(stdout, "// %s: %u compiled, %u left to the interpreter, "
                         "%zu bytes\n",
                 std::string(database.sources().Path(file)).c_str(),
                 compiled->compiled_count(), compiled->skipped_count(),
                 compiled->size());
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
      std::fprintf(stdout, "@%s: interpreted, %s\n", name.c_str(),
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

  int ran = 0;
  for (FileId file : files) {
    const LoweredFile* lowered = database.Lowered(file);
    const CheckedFile* checked = database.Checked(file);
    if (lowered == nullptr || checked == nullptr ||
        lowered->benches.empty()) {
      continue;
    }
    const std::unique_ptr<codegen::CompiledModule> compiled =
        codegen::Compile(database.interner(), database.types(),
                         checked->resolution, lowered->module);

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
      compiled->Run(name, {});
      double ms = 0;
      interp::Outcome outcome;
      for (int run = 0; run < kRuns; ++run) {
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

int Run(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument = argv[i];
    if (argument == "--no-color") {
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
      options.command == "run" || options.command == "test" ||
      options.command == "asm" || options.command == "bench";
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
