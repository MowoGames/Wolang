// Copyright 2026 The Wolang Authors. All rights reserved.
//
// The wolang command line driver. Only the commands that the current wave
// supports are implemented; see docs/roadmap.md for what each wave adds.

#include <cstdio>
#include <cstdlib>
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
};

bool StderrIsTerminal() {
#if defined(_WIN32)
  return _isatty(_fileno(stderr)) != 0;
#else
  return isatty(fileno(stderr)) != 0;
#endif
}

void PrintUsage() {
  std::fprintf(stdout,
               "wolang %s - the Wolang compiler driver\n"
               "\n"
               "usage: wolang <command> [options] [files...]\n"
               "\n"
               "commands:\n"
               "  check <files>   report diagnostics for the given files\n"
               "  tokens <files>  print the token stream of the given files\n"
               "  ast <files>     print the syntax tree of the given files\n"
               "  version         print the version and exit\n"
               "  help            print this message\n"
               "\n"
               "options:\n"
               "  --no-color      never emit ANSI colors\n"
               "  --trivia        include comments in `tokens` output\n"
               "  --spans         annotate `ast` output with positions\n",
               std::string(kVersion).c_str());
}

// Renders everything collected in |diagnostics| and returns the exit code.
int ReportAndSummarize(const SourceManager& sources,
                       const DiagnosticEngine& diagnostics,
                       const RenderOptions& options) {
  if (!diagnostics.diagnostics().empty()) {
    const std::string text =
        RenderDiagnostics(sources, diagnostics.diagnostics(), options);
    std::fputs(text.c_str(), stderr);
  }

  const std::string summary = RenderSummary(diagnostics, options);
  if (!summary.empty()) {
    std::fprintf(stderr, "\n%s\n", summary.c_str());
  }
  return diagnostics.has_errors() ? 1 : 0;
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
      case TokenKind::kIntLiteral: {
        const NumberLiteral& number = stream.NumberOf(token);
        detail = std::to_string(number.integer_value);
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
      case TokenKind::kFloatLiteral: {
        const NumberLiteral& number = stream.NumberOf(token);
        detail = std::to_string(number.float_value);
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

int RunFrontendCommand(const Options& options) {
  SourceManager sources;
  StringInterner interner;
  DiagnosticEngine diagnostics;

  RenderOptions render;
  render.color = options.color && StderrIsTerminal();

  AstPrintOptions print;
  print.include_spans = options.show_spans;

  for (const std::string& input : options.inputs) {
    std::string error;
    const FileId file = sources.LoadFile(input, &error);
    if (!file.is_valid()) {
      std::fprintf(stderr, "wolang: %s\n", error.c_str());
      return 2;
    }

    const TokenStream stream = Lex(sources, file, &interner, &diagnostics);
    if (options.command == "tokens") {
      PrintTokens(sources, interner, stream, options.show_trivia);
      continue;
    }

    const Ast ast = Parse(sources, stream, &interner, &diagnostics);
    if (options.command == "ast") {
      const std::string text = PrintAst(sources, interner, ast, print);
      std::fputs(text.c_str(), stdout);
    }
  }

  return ReportAndSummarize(sources, diagnostics, render);
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
  if (options.command == "check" || options.command == "tokens" ||
      options.command == "ast") {
    if (options.inputs.empty()) {
      std::fprintf(stderr, "wolang: %s needs at least one input file\n",
                   options.command.c_str());
      return 2;
    }
    return RunFrontendCommand(options);
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
