// Copyright 2026 The Wolang Authors. All rights reserved.

#ifndef WOLANG_LSP_SERVER_H_
#define WOLANG_LSP_SERVER_H_

namespace wolang {
namespace lsp {

// A language server, over stdin and stdout.
//
// What an editor needs from a compiler is what the compiler already
// knows: where the errors are, what a name means, what type an expression
// has, and how the file should be formatted. All four come from the same
// queries `wolang check` and `wolang fmt` use, so the server is a
// protocol over them rather than a second front end (D117).
//
// Runs until the client says to stop. Returns the exit code.
int Serve();

}  // namespace lsp
}  // namespace wolang

#endif  // WOLANG_LSP_SERVER_H_
