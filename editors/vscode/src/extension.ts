// The Wolang extension is a language client and nothing else.
//
// Everything a person sees -- diagnostics, hover, go to definition,
// symbols, formatting, completion -- comes from `wolang lsp`, which is
// the compiler itself answering. An extension that reimplemented any of
// it would be a second compiler with different answers.

import * as vscode from 'vscode';
import {
  LanguageClient,
  LanguageClientOptions,
  ServerOptions,
  TransportKind,
} from 'vscode-languageclient/node';

let client: LanguageClient | undefined;

export function activate(context: vscode.ExtensionContext): void {
  const configured = vscode.workspace
    .getConfiguration('wolang')
    .get<string>('path', 'wolang');

  const server: ServerOptions = {
    run: { command: configured, args: ['lsp'], transport: TransportKind.stdio },
    debug: {
      command: configured,
      args: ['lsp'],
      transport: TransportKind.stdio,
    },
  };

  const options: LanguageClientOptions = {
    documentSelector: [{ scheme: 'file', language: 'wolang' }],
    synchronize: {
      // A manifest changes what a package is, which changes every answer.
      fileEvents: vscode.workspace.createFileSystemWatcher('**/wolang.toml'),
    },
  };

  client = new LanguageClient('wolang', 'Wolang', server, options);
  client.start().catch((error: unknown) => {
    vscode.window.showErrorMessage(
      `wolang: cannot start \`${configured} lsp\`: ${String(error)}`,
    );
  });

  // The four things a person does to a package, as commands rather than
  // as a terminal they have to remember the flags for.
  for (const command of ['check', 'test', 'build', 'run']) {
    context.subscriptions.push(
      vscode.commands.registerCommand(`wolang.${command}`, () => {
        const folder = vscode.workspace.workspaceFolders?.[0];
        if (folder === undefined) {
          return;
        }
        const task = new vscode.Task(
          { type: 'wolang', command },
          folder,
          `wolang ${command}`,
          'wolang',
          new vscode.ShellExecution(configured, [command, folder.uri.fsPath]),
          '$wolang',
        );
        vscode.tasks.executeTask(task);
      }),
    );
  }
}

export function deactivate(): Thenable<void> | undefined {
  return client?.stop();
}
