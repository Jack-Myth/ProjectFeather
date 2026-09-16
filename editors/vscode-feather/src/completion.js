'use strict';

const KEYWORDS = [
  ['def', 'Declare a top-level function'],
  ['var', 'Declare a variable'],
  ['export', 'Export a top-level declaration'],
  ['return', 'Return from a function'],
  ['if', 'Conditional statement'],
  ['else', 'Alternative branch'],
  ['while', 'Loop while a condition is true'],
  ['null', 'Null value'],
  ['true', 'Boolean true'],
  ['false', 'Boolean false']
];

const BUILTINS = [
  ['import', 'Import a host or Feather module'],
  ['object', 'Create a new object']
];

const SNIPPETS = [
  ['def', 'def ${1:name}(${2}) {\n\t${0}\n}', 'Function declaration'],
  ['if', 'if (${1:condition}) {\n\t${0}\n}', 'If statement'],
  ['while', 'while (${1:condition}) {\n\t${0}\n}', 'While loop'],
  ['var', 'var ${1:name} = ${0:null};', 'Variable declaration'],
  ['import', 'import("${1:module}")', 'Import a module']
];

function maskCommentsAndStrings(source) {
  let output = '';
  let string = false;
  let comment = false;
  let escaped = false;
  for (let index = 0; index < source.length; ++index) {
    const character = source[index];
    if (comment) {
      if (character === '\n') { comment = false; output += '\n'; }
      else output += ' ';
      continue;
    }
    if (string) {
      if (character === '\n') { string = false; escaped = false; output += '\n'; continue; }
      if (!escaped && character === '"') string = false;
      escaped = !escaped && character === '\\';
      if (character !== '\\') escaped = false;
      output += ' ';
      continue;
    }
    if (character === '"') { string = true; output += ' '; continue; }
    if (character === '/' && source[index + 1] === '/') {
      comment = true;
      output += ' ';
      continue;
    }
    output += character;
  }
  return output;
}

function documentSymbols(source) {
  const clean = maskCommentsAndStrings(source);
  const symbols = [];
  const seen = new Set();
  const add = (name, kind, detail) => {
    if (!seen.has(name)) { seen.add(name); symbols.push({ name, kind, detail }); }
  };
  for (const match of clean.matchAll(/\bvar\s+([A-Za-z_][A-Za-z0-9_]*)/g))
    add(match[1], 'variable', 'Feather variable');
  for (const match of clean.matchAll(/\bdef\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(([^)]*)\)/g)) {
    add(match[1], 'function', 'Feather function');
    for (const parameter of match[2].split(',')) {
      const name = /^\s*([A-Za-z_][A-Za-z0-9_]*)/.exec(parameter);
      if (name) add(name[1], 'variable', 'Feather parameter');
    }
  }
  return symbols;
}

function completionModel(source) {
  return {
    keywords: KEYWORDS.map(([name, detail]) => ({ name, detail })),
    builtins: BUILTINS.map(([name, detail]) => ({ name, detail })),
    snippets: SNIPPETS.map(([name, insertText, detail]) => ({ name, insertText, detail })),
    symbols: documentSymbols(source)
  };
}

module.exports = { completionModel, documentSymbols, maskCommentsAndStrings };
