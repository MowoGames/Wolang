// The Wolang extension is a language client and nothing else.
//
// Everything a person sees -- diagnostics, hover, go to definition,
// references, symbols, formatting, completion -- comes from `wolang
// lsp`, which is the compiler itself answering. An extension that
// reimplemented any of it would be a second compiler with different
// answers.
//
// Plain JavaScript with no dependencies, rather than TypeScript over
// `vscode-languageclient`: an extension with a dependency has to be
// packaged by a machine with npm on it, and this one is packaged by
// `python3 tools/package/make_vsix.py` anywhere (D175). What that costs
// is the two hundred lines below -- a framed JSON-RPC client and the
// translation between what the protocol says and what the editor's API
// wants.

const vscode = require('vscode');
const { spawn } = require('child_process');
const path = require('path');

// ------------------------------------------------------------ the wire ---

class Client {
  constructor(command, output) {
    this.command = command;
    this.output = output;
    this.process = null;
    this.buffer = Buffer.alloc(0);
    this.nextId = 1;
    this.pending = new Map();
    this.onDiagnostics = () => {};
    this.ready = false;
  }

  start(folder) {
    this.process = spawn(this.command, ['lsp'], {
      cwd: folder,
      stdio: ['pipe', 'pipe', 'pipe'],
    });
    this.process.on('error', (error) => {
      vscode.window.showErrorMessage(
        `wolang: cannot start \`${this.command} lsp\`: ${error.message}`);
    });
    this.process.stderr.on('data', (chunk) => {
      this.output.append(chunk.toString());
    });
    this.process.stdout.on('data', (chunk) => {
      this.buffer = Buffer.concat([this.buffer, chunk]);
      this.drain();
    });
    this.process.on('exit', (code) => {
      this.ready = false;
      if (code !== 0 && code !== null) {
        this.output.appendLine(`wolang lsp exited with ${code}`);
      }
    });

    return this.request('initialize', {
      processId: process.pid,
      rootUri: folder ? vscode.Uri.file(folder).toString() : null,
      capabilities: {
        textDocument: {
          synchronization: { dynamicRegistration: false },
          publishDiagnostics: {},
          hover: { contentFormat: ['markdown', 'plaintext'] },
          completion: { completionItem: { snippetSupport: false } },
          definition: {},
          references: {},
          documentSymbol: { hierarchicalDocumentSymbolSupport: false },
          formatting: {},
        },
      },
    }).then(() => {
      this.notify('initialized', {});
      this.ready = true;
    });
  }

  // One message out: a header with the length, a blank line, the body.
  send(message) {
    if (this.process === null || this.process.stdin.destroyed) {
      return;
    }
    const body = JSON.stringify(message);
    const length = Buffer.byteLength(body, 'utf8');
    this.process.stdin.write(`Content-Length: ${length}\r\n\r\n${body}`);
  }

  request(method, params) {
    const id = this.nextId++;
    const waiting = new Promise((resolve, reject) => {
      this.pending.set(id, { resolve, reject });
      // A server that never answers must not leave a request that never
      // settles: the editor would wait for it forever.
      setTimeout(() => {
        if (this.pending.has(id)) {
          this.pending.delete(id);
          resolve(null);
        }
      }, 10000);
    });
    this.send({ jsonrpc: '2.0', id, method, params });
    return waiting;
  }

  notify(method, params) {
    this.send({ jsonrpc: '2.0', method, params });
  }

  // Everything in the buffer that is a whole message.
  drain() {
    for (;;) {
      const header = this.buffer.indexOf('\r\n\r\n');
      if (header < 0) {
        return;
      }
      const head = this.buffer.slice(0, header).toString('ascii');
      const match = /Content-Length: *(\d+)/i.exec(head);
      if (match === null) {
        this.buffer = this.buffer.slice(header + 4);
        continue;
      }
      const length = parseInt(match[1], 10);
      const start = header + 4;
      if (this.buffer.length < start + length) {
        return;
      }
      const body = this.buffer.slice(start, start + length).toString('utf8');
      this.buffer = this.buffer.slice(start + length);
      let message = null;
      try {
        message = JSON.parse(body);
      } catch (error) {
        this.output.appendLine(`wolang: unreadable message: ${body}`);
        continue;
      }
      this.handle(message);
    }
  }

  handle(message) {
    if (message.id !== undefined && this.pending.has(message.id)) {
      const waiting = this.pending.get(message.id);
      this.pending.delete(message.id);
      if (message.error) {
        this.output.appendLine(`wolang: ${JSON.stringify(message.error)}`);
        waiting.resolve(null);
      } else {
        waiting.resolve(message.result);
      }
      return;
    }
    if (message.method === 'textDocument/publishDiagnostics') {
      this.onDiagnostics(message.params);
    }
  }

  stop() {
    if (this.process !== null) {
      this.send({ jsonrpc: '2.0', id: this.nextId++, method: 'shutdown' });
      this.notify('exit', null);
      this.process.kill();
      this.process = null;
    }
  }
}

// ------------------------------------------------- protocol to editor ---

function toRange(range) {
  return new vscode.Range(
    new vscode.Position(range.start.line, range.start.character),
    new vscode.Position(range.end.line, range.end.character));
}

function toSeverity(severity) {
  switch (severity) {
    case 1: return vscode.DiagnosticSeverity.Error;
    case 2: return vscode.DiagnosticSeverity.Warning;
    case 3: return vscode.DiagnosticSeverity.Information;
    default: return vscode.DiagnosticSeverity.Hint;
  }
}

function toLocation(one) {
  return new vscode.Location(vscode.Uri.parse(one.uri), toRange(one.range));
}

function position(document, where) {
  return { line: where.line, character: where.character };
}

function identify(document) {
  return { uri: document.uri.toString() };
}

// ---------------------------------------------------------- activation ---

let client = null;

function activate(context) {
  const settings = vscode.workspace.getConfiguration('wolang');
  const command = settings.get('path', 'wolang');
  const output = vscode.window.createOutputChannel('Wolang');
  const diagnostics = vscode.languages.createDiagnosticCollection('wolang');
  context.subscriptions.push(output, diagnostics);

  const folders = vscode.workspace.workspaceFolders;
  const root = folders && folders.length > 0
    ? folders[0].uri.fsPath
    : undefined;

  client = new Client(command, output);
  client.onDiagnostics = (params) => {
    const uri = vscode.Uri.parse(params.uri);
    diagnostics.set(uri, (params.diagnostics || []).map((one) => {
      const held = new vscode.Diagnostic(
        toRange(one.range), one.message, toSeverity(one.severity));
      if (one.code !== undefined) {
        held.code = one.code;
      }
      held.source = 'wolang';
      return held;
    }));
  };
  client.start(root).catch(() => {});

  const selector = { scheme: 'file', language: 'wolang' };

  // What the editor has, the server is told about: a server that cannot
  // see the unsaved buffer answers about the file on disk.
  const opened = (document) => {
    if (document.languageId !== 'wolang') {
      return;
    }
    client.notify('textDocument/didOpen', {
      textDocument: {
        uri: document.uri.toString(),
        languageId: 'wolang',
        version: document.version,
        text: document.getText(),
      },
    });
  };
  vscode.workspace.textDocuments.forEach(opened);
  context.subscriptions.push(
    vscode.workspace.onDidOpenTextDocument(opened),
    vscode.workspace.onDidChangeTextDocument((event) => {
      if (event.document.languageId !== 'wolang') {
        return;
      }
      client.notify('textDocument/didChange', {
        textDocument: {
          uri: event.document.uri.toString(),
          version: event.document.version,
        },
        contentChanges: [{ text: event.document.getText() }],
      });
    }),
    vscode.workspace.onDidSaveTextDocument((document) => {
      if (document.languageId === 'wolang') {
        client.notify('textDocument/didSave', {
          textDocument: identify(document),
        });
      }
    }),
    vscode.workspace.onDidCloseTextDocument((document) => {
      if (document.languageId === 'wolang') {
        client.notify('textDocument/didClose', {
          textDocument: identify(document),
        });
        diagnostics.delete(document.uri);
      }
    }));

  context.subscriptions.push(
    vscode.languages.registerHoverProvider(selector, {
      async provideHover(document, where) {
        const answer = await client.request('textDocument/hover', {
          textDocument: identify(document),
          position: position(document, where),
        });
        if (!answer || !answer.contents) {
          return null;
        }
        const contents = answer.contents;
        const text = typeof contents === 'string'
          ? contents
          : (contents.value || '');
        if (text.length === 0) {
          return null;
        }
        return new vscode.Hover(new vscode.MarkdownString(text));
      },
    }),

    vscode.languages.registerDefinitionProvider(selector, {
      async provideDefinition(document, where) {
        const answer = await client.request('textDocument/definition', {
          textDocument: identify(document),
          position: position(document, where),
        });
        if (!answer) {
          return null;
        }
        return Array.isArray(answer)
          ? answer.map(toLocation)
          : toLocation(answer);
      },
    }),

    vscode.languages.registerReferenceProvider(selector, {
      async provideReferences(document, where) {
        const answer = await client.request('textDocument/references', {
          textDocument: identify(document),
          position: position(document, where),
          context: { includeDeclaration: true },
        });
        return Array.isArray(answer) ? answer.map(toLocation) : [];
      },
    }),

    vscode.languages.registerDocumentSymbolProvider(selector, {
      async provideDocumentSymbols(document) {
        const answer = await client.request('textDocument/documentSymbol', {
          textDocument: identify(document),
        });
        if (!Array.isArray(answer)) {
          return [];
        }
        return answer.map((one) => new vscode.SymbolInformation(
          one.name,
          (one.kind || 1) - 1,
          one.containerName || '',
          toLocation(one.location)));
      },
    }),

    vscode.languages.registerCompletionItemProvider(selector, {
      async provideCompletionItems(document, where) {
        const answer = await client.request('textDocument/completion', {
          textDocument: identify(document),
          position: position(document, where),
        });
        const items = Array.isArray(answer)
          ? answer
          : (answer && answer.items) || [];
        return items.map((one) => {
          const item = new vscode.CompletionItem(one.label);
          if (one.detail) {
            item.detail = one.detail;
          }
          if (one.documentation) {
            item.documentation = typeof one.documentation === 'string'
              ? one.documentation
              : one.documentation.value;
          }
          if (one.kind) {
            item.kind = one.kind - 1;
          }
          return item;
        });
      },
    }, '.', ':'),

    vscode.languages.registerDocumentFormattingEditProvider(selector, {
      async provideDocumentFormattingEdits(document) {
        const answer = await client.request('textDocument/formatting', {
          textDocument: identify(document),
          options: { tabSize: 2, insertSpaces: true },
        });
        if (!Array.isArray(answer)) {
          return [];
        }
        return answer.map((edit) => vscode.TextEdit.replace(
          toRange(edit.range), edit.newText));
      },
    }));

  // The four things a person does to a package, as commands rather than
  // as a terminal they have to remember the flags for.
  for (const verb of ['check', 'test', 'build', 'run']) {
    context.subscriptions.push(
      vscode.commands.registerCommand(`wolang.${verb}`, () => {
        const folder = vscode.workspace.workspaceFolders &&
          vscode.workspace.workspaceFolders[0];
        if (!folder) {
          return;
        }
        const task = new vscode.Task(
          { type: 'wolang', command: verb },
          folder,
          `wolang ${verb}`,
          'wolang',
          new vscode.ShellExecution(command, [verb, '.']),
          '$wolang');
        vscode.tasks.executeTask(task);
      }));
  }

  // The debugger is the compiler too: `wolang dap` runs a program on the
  // interpreter and stops where it is told, and a host that embeds the
  // language opens a port with `wolang_vm_debug_listen` to attach to.
  context.subscriptions.push(
    vscode.debug.registerDebugAdapterDescriptorFactory('wolang', {
      createDebugAdapterDescriptor(session) {
        if (session.configuration.request === 'attach') {
          return new vscode.DebugAdapterServer(
            session.configuration.port || 4711,
            session.configuration.host || '127.0.0.1');
        }
        return new vscode.DebugAdapterExecutable(command, ['dap']);
      },
    }));

  context.subscriptions.push(
    vscode.commands.registerCommand('wolang.restart', () => {
      client.stop();
      client = new Client(command, output);
      client.start(root).catch(() => {});
    }));
}

function deactivate() {
  if (client !== null) {
    client.stop();
    client = null;
  }
}

module.exports = { activate, deactivate };
