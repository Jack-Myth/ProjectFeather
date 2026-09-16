'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { completionModel, documentSymbols, maskCommentsAndStrings } = require('../src/completion');

test('completion model contains Feather keywords, builtins, and snippets', () => {
  const model = completionModel('');
  assert.ok(model.keywords.some(item => item.name === 'export'));
  assert.ok(model.builtins.some(item => item.name === 'import'));
  assert.ok(model.snippets.some(item => item.name === 'def' && item.insertText.includes('${1:name}')));
});

test('document symbols include variables, functions, and parameters once', () => {
  const symbols = documentSymbols(`
    export var base = 2;
    def sum(value, fallback = null) { var result = value; return result; }
    var base = 3;
  `);
  assert.deepEqual(symbols.map(symbol => symbol.name), ['base', 'result', 'sum', 'value', 'fallback']);
  assert.equal(symbols.find(symbol => symbol.name === 'sum').kind, 'function');
});

test('declarations in comments and strings are ignored', () => {
  const source = '// var hidden = 1;\nvar visible = "羽毛 def fake()"; // def hiddenToo()';
  assert.deepEqual(documentSymbols(source).map(symbol => symbol.name), ['visible']);
  const masked = maskCommentsAndStrings(source);
  assert.equal(masked.split('\n')[0].trim(), '');
});
