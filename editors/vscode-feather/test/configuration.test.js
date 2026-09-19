'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const path = require('node:path');
const { finalizeConfiguration } = require('../src/configuration');

test('finalizes program after VS Code substitutes workspaceFolder', () => {
  const workspace = path.resolve('workspace');
  const program = path.join(workspace, 'HelloWorld.fe');
  const config = { request: 'launch', program };

  finalizeConfiguration(workspace, config, () => false, 'win32');

  assert.equal(config.program, program);
  assert.equal(config.runtimeExecutable, 'feather');
});

test('resolves an ordinary relative program against its workspace', () => {
  const workspace = path.resolve('workspace');
  const config = { request: 'launch', program: 'src/main.fe' };

  finalizeConfiguration(workspace, config, () => false, 'win32');

  assert.equal(config.program, path.resolve(workspace, 'src/main.fe'));
});

test('uses the workspace build runtime when it exists', () => {
  const workspace = path.resolve('workspace');
  const expected = path.join(workspace, 'build', 'debug', 'feather.exe');
  const config = { request: 'launch', program: path.join(workspace, 'main.fe') };

  finalizeConfiguration(workspace, config, candidate => candidate === expected, 'win32');

  assert.equal(config.runtimeExecutable, expected);
});
