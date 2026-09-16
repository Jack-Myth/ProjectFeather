'use strict';

const path = require('node:path');
const { spawn } = require('node:child_process');
const { FramedConnection, connect, findAvailablePort } = require('./transport');

class FeatherDebugSession {
  constructor(dependencies = {}) {
    this.spawn = dependencies.spawn || spawn;
    this.connect = dependencies.connect || connect;
    this.findAvailablePort = dependencies.findAvailablePort || findAvailablePort;
    this.sink = () => {};
    this.outputSequence = 1;
    this.targetSequence = 1;
    this.nextReference = 1;
    this.pending = new Map();
    this.references = new Map();
    this.sourceBreakpoints = new Map();
    this.primarySource = '';
    this.connection = undefined;
    this.child = undefined;
    this.ownsChild = false;
    this.disposed = false;
  }

  setMessageSink(sink) { this.sink = sink; }

  emit(message) {
    message.seq = this.outputSequence++;
    this.sink(message);
  }

  event(event, body = {}) { this.emit({ type: 'event', event, body }); }

  respond(request, body = {}) {
    this.emit({ type: 'response', request_seq: request.seq, command: request.command, success: true, body });
  }

  fail(request, error) {
    this.emit({ type: 'response', request_seq: request.seq, command: request.command,
      success: false, message: error instanceof Error ? error.message : String(error) });
  }

  handleMessage(message) {
    if (!message || message.type !== 'request') return;
    Promise.resolve(this.dispatch(message)).catch(error => this.fail(message, error));
  }

  async dispatch(request) {
    const args = request.arguments || {};
    switch (request.command) {
      case 'initialize':
        this.respond(request, {
          supportsConfigurationDoneRequest: true,
          supportsTerminateRequest: false,
          supportsExceptionFilterOptions: false,
          supportsEvaluateForHovers: true,
          supportsConditionalBreakpoints: true,
          supportsSetVariable: true,
          exceptionBreakpointFilters: [
            { filter: 'error', label: 'Error results', default: false,
              description: 'Break when an operation or function returns an Error value.' }
          ]
        });
        return;
      case 'launch':
        await this.startLaunch(args);
        await this.targetRequest('Debugger.enable');
        this.respond(request);
        this.event('initialized');
        return;
      case 'attach':
        await this.startAttach(args);
        await this.targetRequest('Debugger.enable');
        this.respond(request);
        this.event('initialized');
        return;
      case 'configurationDone':
        await this.targetRequest('Runtime.runIfWaitingForDebugger');
        this.respond(request);
        return;
      case 'disconnect':
        await this.disconnect();
        this.respond(request);
        return;
      case 'threads':
        this.respond(request, { threads: [{ id: 1, name: 'Feather VM' }] });
        return;
      case 'setBreakpoints':
        await this.setBreakpoints(request, args);
        return;
      case 'setExceptionBreakpoints':
        await this.targetRequest('Debugger.setPauseOnErrors', { enabled: (args.filters || []).includes('error') });
        this.respond(request);
        return;
      case 'stackTrace':
        await this.stackTrace(request);
        return;
      case 'scopes':
        this.scopes(request, args);
        return;
      case 'variables':
        await this.variables(request, args);
        return;
      case 'evaluate':
        await this.evaluate(request, args);
        return;
      case 'setVariable':
        await this.setVariable(request, args);
        return;
      case 'continue':
        await this.executionRequest(request, 'Debugger.resume', { allThreadsContinued: true });
        return;
      case 'next':
        await this.executionRequest(request, 'Debugger.stepOver');
        return;
      case 'stepIn':
        await this.executionRequest(request, 'Debugger.stepInto');
        return;
      case 'stepOut':
        await this.executionRequest(request, 'Debugger.stepOut');
        return;
      case 'pause':
        await this.executionRequest(request, 'Debugger.pause');
        return;
      default:
        throw new Error(`unsupported DAP request: ${request.command}`);
    }
  }

  async startLaunch(args) {
    if (!args.program) throw new Error('launch requires program');
    const host = args.host || '127.0.0.1';
    const port = args.port || await this.findAvailablePort(host);
    const runtime = args.runtimeExecutable || 'feather';
    const runtimeArgs = Array.isArray(args.runtimeArgs) ? args.runtimeArgs : [];
    this.primarySource = path.resolve(args.program);
    this.ownsChild = true;
    this.child = this.spawn(runtime,
      [...runtimeArgs, 'debug', '--listen', `${host}:${port}`, '--wait-debugger', this.primarySource],
      { cwd: path.dirname(this.primarySource), windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'] });
    this.child.stdout.on('data', data => this.output('stdout', data));
    this.child.stderr.on('data', data => this.output('stderr', data));
    const childFailed = new Promise((_, reject) => this.child.once('error', reject));
    this.child.once('error', error => this.output('stderr', `${error.message}\n`));
    this.child.once('exit', code => {
      if (!this.disposed && this.connection)
        this.output('console', `Feather process exited with code ${code}.\n`);
    });
    try {
      const socket = await Promise.race([
        this.connect(host, port, args.connectTimeout || 5000, true), childFailed
      ]);
      this.attachSocket(socket);
    } catch (error) {
      if (this.child && this.child.exitCode === null) this.child.kill();
      throw error;
    }
  }

  async startAttach(args) {
    if (!args.port) throw new Error('attach requires port');
    this.primarySource = args.program ? path.resolve(args.program) : '';
    const socket = await this.connect(args.host || '127.0.0.1', args.port,
      args.connectTimeout || 5000, true);
    this.attachSocket(socket);
  }

  attachSocket(socket) {
    this.connection = new FramedConnection(socket,
      message => this.handleTargetMessage(message),
      error => this.handleConnectionClosed(error));
  }

  output(category, data) {
    this.event('output', { category, output: String(data) });
  }

  targetRequest(method, params = {}) {
    if (!this.connection) return Promise.reject(new Error('not connected to a Feather debug target'));
    const id = this.targetSequence++;
    return new Promise((resolve, reject) => {
      this.pending.set(id, { resolve, reject });
      try { this.connection.send({ id, method, params }); }
      catch (error) { this.pending.delete(id); reject(error); }
    });
  }

  handleTargetMessage(message) {
    if (Number.isSafeInteger(message.id)) {
      const pending = this.pending.get(message.id);
      if (!pending) return;
      this.pending.delete(message.id);
      if (message.error) pending.reject(new Error(message.error.message || 'Feather target rejected the request'));
      else pending.resolve(message.result || {});
      return;
    }
    const params = message.params || {};
    switch (message.method) {
      case 'Debugger.paused': {
        this.resetReferences();
        const reason = params.reason === 'error' ? 'exception' :
          params.reason === 'pause' ? 'pause' :
          params.reason === 'breakpoint' ? 'breakpoint' : 'step';
        const body = { reason, threadId: 1, allThreadsStopped: true };
        if (params.error && params.error.description) body.text = params.error.description;
        if (params.conditionError) body.description = params.conditionError;
        this.event('stopped', body);
        break;
      }
      case 'Debugger.resumed':
        this.resetReferences();
        this.event('continued', { threadId: 1, allThreadsContinued: true });
        break;
      case 'Debugger.executionFinished':
        this.resetReferences();
        this.event('terminated');
        break;
      case 'Debugger.breakpointResolved':
        this.event('breakpoint', { reason: 'changed', breakpoint: {
          id: params.breakpointId, verified: true, line: params.line, column: params.column
        }});
        break;
      case 'Debugger.protocolError':
        this.output('stderr', `${params.message || 'Feather debug protocol error'}\n`);
        break;
    }
  }

  handleConnectionClosed(error) {
    this.connection = undefined;
    for (const pending of this.pending.values()) pending.reject(error || new Error('Feather target disconnected'));
    this.pending.clear();
    if (error && !this.disposed) this.output('stderr', `${error.message}\n`);
  }

  async setBreakpoints(request, args) {
    const sourcePath = args.source && args.source.path;
    if (!sourcePath) throw new Error('setBreakpoints requires source.path');
    const key = path.normalize(sourcePath);
    const old = this.sourceBreakpoints.get(key) || [];
    for (const id of old) await this.targetRequest('Debugger.removeBreakpoint', { breakpointId: id });
    const ids = [];
    const breakpoints = [];
    for (const input of args.breakpoints || []) {
      const params = { moduleId: this.moduleId(sourcePath), line: input.line };
      if (input.column !== undefined) params.column = input.column;
      if (input.condition) params.condition = input.condition;
      const result = await this.targetRequest('Debugger.setBreakpoint', params);
      ids.push(result.breakpointId);
      breakpoints.push({ id: result.breakpointId, verified: false });
    }
    this.sourceBreakpoints.set(key, ids);
    this.respond(request, { breakpoints });
  }

  moduleId(sourcePath) {
    return this.primarySource && path.normalize(path.resolve(sourcePath)) === path.normalize(this.primarySource)
      ? '' : sourcePath;
  }

  sourcePath(moduleId) { return moduleId === '' ? this.primarySource : moduleId; }

  async stackTrace(request) {
    const result = await this.targetRequest('Debugger.getStackTrace');
    const stackFrames = (result.frames || []).map(frame => {
      const output = { id: frame.frameId, name: frame.functionName, line: frame.line || 1, column: frame.column || 1 };
      const source = this.sourcePath(frame.moduleId || '');
      if (source) output.source = { name: path.basename(source), path: source };
      return output;
    });
    this.respond(request, { stackFrames, totalFrames: stackFrames.length });
  }

  scopes(request, args) {
    const scopes = ['locals', 'stack', 'globals'].map(scope => ({
      name: scope[0].toUpperCase() + scope.slice(1),
      variablesReference: this.makeReference({ kind: 'scope', frameId: args.frameId, scope }),
      expensive: scope === 'globals'
    }));
    this.respond(request, { scopes });
  }

  async variables(request, args) {
    const reference = this.references.get(args.variablesReference);
    if (!reference) throw new Error('variablesReference is no longer valid');
    let values;
    if (reference.kind === 'scope') {
      const result = await this.targetRequest('Debugger.getVariables',
        { frameId: reference.frameId, scope: reference.scope });
      values = result.variables || [];
    } else {
      const result = await this.targetRequest('Debugger.getProperties', { objectId: reference.objectId });
      values = result.properties || [];
    }
    this.respond(request, { variables: values.map(value => this.dapValue(value, reference.frameId)) });
  }

  async evaluate(request, args) {
    if (args.frameId === undefined) throw new Error('evaluate requires a paused frame');
    const result = await this.targetRequest('Debugger.evaluate', {
      frameId: args.frameId, expression: args.expression
    });
    const value = this.dapValue(result, args.frameId);
    value.result = value.value;
    delete value.value;
    this.respond(request, value);
  }

  async setVariable(request, args) {
    const reference = this.references.get(args.variablesReference);
    if (!reference) throw new Error('variablesReference is no longer valid');
    const params = { frameId: reference.frameId, name: args.name, expression: args.value };
    if (reference.kind === 'scope') params.scope = reference.scope;
    else params.objectId = reference.objectId;
    const result = await this.targetRequest('Debugger.setVariable', params);
    this.respond(request, this.dapValue(result, reference.frameId));
  }

  dapValue(value, frameId = 0) {
    const result = {
      value: value.description,
      type: value.type,
      variablesReference: value.objectId === undefined ? 0 :
        this.makeReference({ kind: 'object', objectId: value.objectId, frameId })
    };
    if (value.name !== undefined) result.name = value.name;
    return result;
  }

  makeReference(reference) {
    const id = this.nextReference++;
    this.references.set(id, reference);
    return id;
  }

  resetReferences() {
    this.references.clear();
    this.nextReference = 1;
  }

  async executionRequest(request, method, body = {}) {
    await this.targetRequest(method);
    this.respond(request, body);
  }

  async disconnect() {
    if (this.connection) {
      try { await this.targetRequest('Debugger.disable'); } catch (_) {}
      this.connection.close();
      this.connection = undefined;
    }
    if (this.ownsChild && this.child && this.child.exitCode === null) this.child.kill();
  }

  dispose() {
    if (this.disposed) return;
    this.disposed = true;
    if (this.connection) this.connection.close();
    if (this.ownsChild && this.child && this.child.exitCode === null) this.child.kill();
    for (const pending of this.pending.values()) pending.reject(new Error('debug session disposed'));
    this.pending.clear();
  }
}

module.exports = { FeatherDebugSession };
