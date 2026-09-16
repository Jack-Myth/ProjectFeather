'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { EventEmitter } = require('node:events');
const { FramedConnection } = require('../src/transport');

class FakeSocket extends EventEmitter {
  constructor() { super(); this.writes = []; }
  write(data) { this.writes.push(Buffer.from(data)); }
  destroy() { this.emit('close'); }
}

test('framed connection parses split and consecutive messages', () => {
  const socket = new FakeSocket();
  const messages = [];
  new FramedConnection(socket, message => messages.push(message), error => assert.ifError(error));
  const first = Buffer.from('{"id":1}');
  const second = Buffer.from('{"method":"paused"}');
  const bytes = Buffer.concat([
    Buffer.from(`Content-Length: ${first.length}\r\n\r\n`), first,
    Buffer.from(`Content-Length: ${second.length}\r\n\r\n`), second
  ]);
  socket.emit('data', bytes.subarray(0, 11));
  socket.emit('data', bytes.subarray(11));
  assert.deepEqual(messages, [{ id: 1 }, { method: 'paused' }]);
});

test('framed connection sends UTF-8 byte length', () => {
  const socket = new FakeSocket();
  const connection = new FramedConnection(socket, () => {}, error => assert.ifError(error));
  connection.send({ text: '羽毛' });
  const output = Buffer.concat(socket.writes).toString('utf8');
  assert.match(output, /^Content-Length: 17\r\n\r\n/);
  assert.match(output, /羽毛"}$/);
});
