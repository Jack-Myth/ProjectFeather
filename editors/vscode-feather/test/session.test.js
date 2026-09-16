'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { FeatherDebugSession, pathKey } = require('../src/session');

function request(seq, command, args = {}) {
  return { seq, type: 'request', command, arguments: args };
}

test('initialize advertises Feather Error breakpoints', async () => {
  const output = [];
  const session = new FeatherDebugSession();
  session.setMessageSink(message => output.push(message));
  await session.dispatch(request(1, 'initialize', { supportsRunInTerminalRequest: true }));
  assert.equal(output[0].success, true);
  assert.equal(output[0].body.exceptionBreakpointFilters[0].filter, 'error');
  assert.equal(session.supportsRunInTerminalRequest, true);
});

test('launch uses the integrated terminal when the client supports it', async () => {
  const output = [];
  const socket = { on() {}, write() {}, destroy() {} };
  const session = new FeatherDebugSession({
    findAvailablePort: async () => 4711,
    connect: async () => socket
  });
  session.supportsRunInTerminalRequest = true;
  session.setMessageSink(message => {
    output.push(message);
    if (message.type === 'request' && message.command === 'runInTerminal') {
      queueMicrotask(() => session.handleMessage({
        seq: 90, type: 'response', request_seq: message.seq,
        command: message.command, success: true, body: { processId: 1234 }
      }));
    }
  });

  await session.startLaunch({ program: 'main.fe', runtimeExecutable: 'feather' });

  const launch = output.find(message => message.command === 'runInTerminal');
  assert.equal(launch.arguments.kind, 'integrated');
  assert.deepEqual(launch.arguments.args.slice(0, 5),
    ['feather', 'debug', '--listen', '127.0.0.1:4711', '--wait-debugger']);
  assert.equal(session.terminalProcessId, 1234);
  session.terminalProcessId = undefined;
  session.dispose();
});

test('root breakpoints and Error filter map to target requests', async () => {
  const output = [];
  const calls = [];
  const session = new FeatherDebugSession();
  session.primarySource = require('node:path').resolve('main.fe');
  session.setMessageSink(message => output.push(message));
  session.targetRequest = async (method, params) => {
    calls.push({ method, params });
    return method === 'Debugger.setBreakpoint' ? { breakpointId: 7 } : {};
  };
  await session.dispatch(request(1, 'setBreakpoints', {
    source: { path: session.primarySource }, breakpoints: [{ line: 4, condition: 'value == 2' }]
  }));
  await session.dispatch(request(2, 'setExceptionBreakpoints', { filters: ['error'] }));
  assert.deepEqual(calls[0], {
    method: 'Debugger.setBreakpoint', params: {
      moduleId: '', line: 4, condition: 'value == 2'
    }
  });
  assert.deepEqual(calls[1], {
    method: 'Debugger.setPauseOnErrors', params: { enabled: true }
  });
  assert.equal(output[0].body.breakpoints[0].verified, false);
});

test('Windows root source matching ignores path casing', () => {
  const session = new FeatherDebugSession({ platform: 'win32' });
  session.primarySource = 'C:\\Users\\JackMyth\\Projects\\MyFeather\\HelloWorld.fe';

  assert.equal(session.moduleId('c:\\users\\jackmyth\\projects\\myfeather\\HELLOWORLD.FE'), '');
  assert.equal(
    pathKey('C:\\Users\\JackMyth\\Projects\\MyFeather\\HelloWorld.fe', 'win32'),
    pathKey('c:\\users\\jackmyth\\projects\\myfeather\\HELLOWORLD.FE', 'win32')
  );
});

test('evaluate and setVariable map through stop-scoped references', async () => {
  const output = [];
  const calls = [];
  const session = new FeatherDebugSession();
  session.setMessageSink(message => output.push(message));
  session.targetRequest = async (method, params) => {
    calls.push({ method, params });
    return { type: 'number', description: '5', value: 5 };
  };
  await session.dispatch(request(1, 'scopes', { frameId: 3 }));
  await session.dispatch(request(2, 'evaluate', { frameId: 3, expression: 'x + 1' }));
  await session.dispatch(request(3, 'setVariable', {
    variablesReference: 1, name: 'x', value: '5'
  }));
  assert.deepEqual(calls[0], {
    method: 'Debugger.evaluate', params: { frameId: 3, expression: 'x + 1' }
  });
  assert.deepEqual(calls[1], {
    method: 'Debugger.setVariable', params: {
      frameId: 3, name: 'x', expression: '5', scope: 'locals'
    }
  });
  assert.equal(output.find(message => message.request_seq === 2).body.result, '5');
  assert.equal(output.find(message => message.request_seq === 3).body.value, '5');
});

test('Error pause becomes a DAP exception stop', () => {
  const output = [];
  const session = new FeatherDebugSession();
  session.setMessageSink(message => output.push(message));
  session.handleTargetMessage({ method: 'Debugger.paused', params: {
    reason: 'error', error: { description: 'Error: invalid operands' }
  }});
  assert.equal(output[0].event, 'stopped');
  assert.equal(output[0].body.reason, 'exception');
  assert.equal(output[0].body.text, 'Error: invalid operands');
});
