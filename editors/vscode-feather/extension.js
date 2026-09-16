'use strict';

const fs = require('node:fs');
const vscode = require('vscode');
const { FeatherDebugSession } = require('./src/session');
const { completionModel } = require('./src/completion');
const { finalizeConfiguration } = require('./src/configuration');

class InlineAdapter {
  constructor(session) {
    this.session = session;
    this.messages = new vscode.EventEmitter();
    this.onDidSendMessage = this.messages.event;
    session.setMessageSink(message => this.messages.fire(message));
  }

  handleMessage(message) {
    this.session.handleMessage(message);
  }

  dispose() {
    this.session.dispose();
    this.messages.dispose();
  }
}

class ConfigurationProvider {
  resolveDebugConfiguration(folder, config) {
    const editor = vscode.window.activeTextEditor;
    if (!config.type && editor && editor.document.languageId === 'feather') {
      config.type = 'feather';
      config.name = 'Debug Feather program';
      config.request = 'launch';
      config.program = '${file}';
    }
    if (config.request === 'launch' && !config.program) {
      vscode.window.showErrorMessage('A Feather launch configuration requires "program".');
      return undefined;
    }
    if (config.request === 'attach' && !config.port) {
      vscode.window.showErrorMessage('A Feather attach configuration requires "port".');
      return undefined;
    }
    return config;
  }

  resolveDebugConfigurationWithSubstitutedVariables(folder, config) {
    return finalizeConfiguration(folder && folder.uri.fsPath, config, fs.existsSync);
  }
}

class AdapterFactory {
  createDebugAdapterDescriptor() {
    return new vscode.DebugAdapterInlineImplementation(new InlineAdapter(new FeatherDebugSession()));
  }
}

function completionItems(document) {
  const model = completionModel(document.getText());
  const items = [];
  for (const entry of model.keywords) {
    const item = new vscode.CompletionItem(entry.name, vscode.CompletionItemKind.Keyword);
    item.detail = entry.detail;
    items.push(item);
  }
  for (const entry of model.builtins) {
    const item = new vscode.CompletionItem(entry.name, vscode.CompletionItemKind.Function);
    item.detail = entry.detail;
    items.push(item);
  }
  for (const entry of model.snippets) {
    const item = new vscode.CompletionItem(entry.name, vscode.CompletionItemKind.Snippet);
    item.detail = entry.detail;
    item.insertText = new vscode.SnippetString(entry.insertText);
    item.sortText = `0-${entry.name}`;
    items.push(item);
  }
  for (const entry of model.symbols) {
    const kind = entry.kind === 'function'
      ? vscode.CompletionItemKind.Function : vscode.CompletionItemKind.Variable;
    const item = new vscode.CompletionItem(entry.name, kind);
    item.detail = entry.detail;
    items.push(item);
  }
  return items;
}

function activate(context) {
  context.subscriptions.push(
    vscode.debug.registerDebugConfigurationProvider('feather', new ConfigurationProvider()),
    vscode.debug.registerDebugAdapterDescriptorFactory('feather', new AdapterFactory()),
    vscode.languages.registerCompletionItemProvider('feather', {
      provideCompletionItems(document) { return completionItems(document); }
    })
  );
}

function deactivate() {}

module.exports = { activate, deactivate, completionItems };
