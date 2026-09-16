'use strict';

const net = require('node:net');

const MAX_MESSAGE_BYTES = 1024 * 1024;
const MAX_HEADER_BYTES = 8192;

class FramedConnection {
  constructor(socket, onMessage, onClose) {
    this.socket = socket;
    this.onMessage = onMessage;
    this.onClose = onClose;
    this.buffer = Buffer.alloc(0);
    this.expected = undefined;
    this.closed = false;
    socket.on('data', chunk => this.accept(chunk));
    socket.on('error', error => this.finish(error));
    socket.on('close', () => this.finish());
  }

  send(message) {
    if (this.closed) throw new Error('debug connection is closed');
    const body = Buffer.from(JSON.stringify(message), 'utf8');
    if (body.length > MAX_MESSAGE_BYTES) throw new Error('debug message exceeds 1 MiB');
    this.socket.write(`Content-Length: ${body.length}\r\n\r\n`);
    this.socket.write(body);
  }

  accept(chunk) {
    if (this.closed) return;
    this.buffer = Buffer.concat([this.buffer, chunk]);
    try {
      while (true) {
        if (this.expected === undefined) {
          const end = this.buffer.indexOf('\r\n\r\n');
          if (end < 0) {
            if (this.buffer.length > MAX_HEADER_BYTES) throw new Error('debug message header exceeds 8 KiB');
            return;
          }
          const header = this.buffer.subarray(0, end).toString('ascii');
          this.buffer = this.buffer.subarray(end + 4);
          const lengths = header.split('\r\n')
            .map(line => /^content-length:\s*(\d+)\s*$/i.exec(line))
            .filter(Boolean);
          if (lengths.length !== 1) throw new Error('invalid Content-Length header');
          this.expected = Number(lengths[0][1]);
          if (!Number.isSafeInteger(this.expected) || this.expected > MAX_MESSAGE_BYTES)
            throw new Error('debug message exceeds 1 MiB');
        }
        if (this.buffer.length < this.expected) return;
        const body = this.buffer.subarray(0, this.expected).toString('utf8');
        this.buffer = this.buffer.subarray(this.expected);
        this.expected = undefined;
        this.onMessage(JSON.parse(body));
      }
    } catch (error) {
      this.finish(error);
      this.socket.destroy();
    }
  }

  finish(error) {
    if (this.closed) return;
    this.closed = true;
    this.onClose(error);
  }

  close() {
    if (!this.closed) this.socket.destroy();
    this.finish();
  }
}

function connect(host, port, timeout = 5000, retry = false) {
  return new Promise((resolve, reject) => {
    const started = Date.now();
    let timer;
    let stopped = false;
    const fail = error => {
      if (stopped) return;
      if (retry && Date.now() - started < timeout) {
        timer = setTimeout(attempt, 50);
        return;
      }
      stopped = true;
      reject(error);
    };
    const attempt = () => {
      const socket = net.createConnection({ host, port });
      socket.setNoDelay(true);
      socket.setTimeout(Math.max(100, timeout - (Date.now() - started)), () =>
        socket.destroy(new Error(`timed out connecting to ${host}:${port}`)));
      socket.once('connect', () => {
        if (stopped) return socket.destroy();
        stopped = true;
        clearTimeout(timer);
        socket.setTimeout(0);
        socket.removeAllListeners('error');
        resolve(socket);
      });
      socket.once('error', fail);
    };
    attempt();
  });
}

function findAvailablePort(host) {
  return new Promise((resolve, reject) => {
    const server = net.createServer();
    server.unref();
    server.once('error', reject);
    server.listen(0, host, () => {
      const address = server.address();
      server.close(error => error ? reject(error) : resolve(address.port));
    });
  });
}

module.exports = { FramedConnection, connect, findAvailablePort, MAX_MESSAGE_BYTES };
