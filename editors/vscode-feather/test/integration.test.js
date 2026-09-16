'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { FeatherDebugSession } = require('../src/session');

const repository = path.resolve(__dirname, '..', '..', '..');
const runtime = path.join(repository, 'build', process.platform === 'win32' ? 'feather.exe' : 'feather');
const program = path.join(repository, 'tests', 'fixtures', 'cli-ok.fe');

test('launch drives a real breakpoint, inspection, resume, and termination',
  { skip: !fs.existsSync(runtime), timeout: 10000 }, async () => {
    const output = [];
    const waiters = new Map();
    const waitForEvent = name => new Promise(resolve => waiters.set(name, resolve));
    const session = new FeatherDebugSession();
    session.setMessageSink(message => {
      output.push(message);
      if (message.type === 'event' && waiters.has(message.event)) {
        waiters.get(message.event)(message);
        waiters.delete(message.event);
      }
    });
    await session.dispatch({ seq: 1, type: 'request', command: 'launch', arguments: {
      program, runtimeExecutable: runtime, connectTimeout: 5000
    }});
    await session.dispatch({ seq: 2, type: 'request', command: 'setBreakpoints', arguments: {
      source: { path: program }, breakpoints: [{ line: 2, condition: 'start == 3' }]
    }});
    const stopped = waitForEvent('stopped');
    await session.dispatch({ seq: 3, type: 'request', command: 'configurationDone', arguments: {} });
    assert.equal((await stopped).body.reason, 'breakpoint');
    await session.dispatch({ seq: 4, type: 'request', command: 'stackTrace', arguments: { threadId: 1 } });
    const stack = output.find(message => message.type === 'response' && message.request_seq === 4);
    assert.equal(stack.body.stackFrames[0].source.path, program);
    await session.dispatch({ seq: 9, type: 'request', command: 'evaluate', arguments: {
      frameId: stack.body.stackFrames[0].id, expression: 'start + 2', context: 'watch'
    }});
    assert.equal(output.find(message => message.request_seq === 9).body.result, '5');
    await session.dispatch({ seq: 5, type: 'request', command: 'scopes', arguments: {
      frameId: stack.body.stackFrames[0].id
    }});
    const scopes = output.find(message => message.type === 'response' && message.request_seq === 5);
    await session.dispatch({ seq: 6, type: 'request', command: 'variables', arguments: {
      variablesReference: scopes.body.scopes[0].variablesReference
    }});
    assert.ok(output.some(message => message.type === 'response' && message.request_seq === 6));
    await session.dispatch({ seq: 10, type: 'request', command: 'setVariable', arguments: {
      variablesReference: scopes.body.scopes[2].variablesReference,
      name: 'start', value: '4'
    }});
    assert.equal(output.find(message => message.request_seq === 10).body.value, '4');
    const finished = waitForEvent('terminated');
    await session.dispatch({ seq: 7, type: 'request', command: 'continue', arguments: { threadId: 1 } });
    await finished;
    assert.ok(output.some(message => message.type === 'event' && message.event === 'initialized'));
    assert.ok(output.some(message => message.type === 'event' && message.event === 'terminated'));
    await session.dispatch({ seq: 8, type: 'request', command: 'disconnect', arguments: {} });
    session.dispose();
  });
