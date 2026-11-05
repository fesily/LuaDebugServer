'use strict';

// Integration tests for the native lua-debug adapter, driven through
// Microsoft's @vscode/debugadapter-testsupport (the same DebugClient that
// vscode-mock-debug uses). It speaks real DAP over the adapter's stdio and
// tolerates no adapter-specific shortcuts, so it complements the stdlib Python
// harness in test/e2e (which can address the backend socket and instrument
// timings more directly).

const path = require('path');
const fs = require('fs');
const os = require('os');
const assert = require('assert');
const { DebugClient } = require('@vscode/debugadapter-testsupport');

const ROOT = path.resolve(__dirname, '..', '..');
const ADAPTER = path.join(ROOT, 'publish', 'bin',
  process.platform === 'win32' ? 'lua-debug.exe' : 'lua-debug');
const FIXTURE = path.join(ROOT, 'test', 'e2e', 'fixtures', 'baseline.lua');

// The runtime root that matches the host; overridable for other machines.
const HOST_PLATFORM =
  process.platform === 'win32' ? 'win32' : process.platform === 'darwin' ? 'darwin' : 'linux';
const HOST_ARCH = process.arch === 'arm64' ? 'arm64' : 'x64';
const RUNTIME_ROOT = process.env.LUA_DEBUG_RUNTIME_ROOT
  || path.join(ROOT, 'publish', 'runtime', `${HOST_PLATFORM}-${HOST_ARCH}`);

const VERSIONS = (process.env.LUA_DEBUG_VERSIONS || 'lua51,lua52,lua53,lua54,lua55,luajit')
  .split(',')
  .map((v) => v.trim())
  .filter(Boolean);

const LUA_EXE_NAME = process.platform === 'win32' ? 'lua.exe' : 'lua';

function luaExe(version) {
  return path.join(RUNTIME_ROOT, version, LUA_EXE_NAME);
}

function traceFile(name) {
  return path.join(os.tmpdir(), `lua-debug-node-${process.pid}-${name}.txt`);
}

function launchArgs(version, extra) {
  return Object.assign(
    {
      type: 'lua',
      request: 'launch',
      name: 'node-integration',
      luaVersion: version,
      luaexe: luaExe(version),
      program: FIXTURE,
      cwd: path.dirname(FIXTURE),
      console: 'internalConsole',
      // The adapter stops on entry by default; these cases want the breakpoint.
      stopOnEntry: false,
      env: { LUA_DEBUG_E2E_TRACE: traceFile(version) },
    },
    extra || {},
  );
}

function readTrace(version) {
  const file = traceFile(version);
  if (!fs.existsSync(file)) {
    return [];
  }
  return fs.readFileSync(file, 'utf8').split('\n').filter((line) => line.length > 0);
}

function createClient() {
  // DebugClient spawns `<runtime> [<executable>]`; the adapter is a standalone
  // executable, so the second program name is deliberately empty and the
  // adapter sees no positional argument (i.e. speaks DAP over stdio).
  const dc = new DebugClient(ADAPTER, '', 'lua', undefined, !!process.env.LUA_DEBUG_SHOW_ADAPTER_STDERR);
  // Once the debuggee ends, the adapter exits; a trailing `disconnect` write
  // then fails with EPIPE. That is a property of the session being over, not a
  // test failure, so the transport errors are swallowed here.
  const swallow = () => {};
  const start = dc.start.bind(dc);
  dc.start = (...args) => start(...args).then(() => {
    dc._adapterProcess.stdin.on('error', swallow);
    dc._adapterProcess.stdout.on('error', swallow);
    return dc;
  });
  return dc;
}

// Graceful teardown that cannot hang: the adapter exits by itself once the
// debuggee ends, so a late `disconnect` may never be answered.
async function stopClient(dc) {
  let timer;
  const timeout = new Promise((resolve) => {
    timer = setTimeout(resolve, 1000);
    if (typeof timer.unref === 'function') {
      timer.unref();
    }
  });
  try {
    await Promise.race([dc.stop(), timeout]);
  } catch (e) {
    // Session already over.
  } finally {
    clearTimeout(timer);
  }
  dc.stopAdapter();
}

// LuaJIT can report the same line breakpoint again right after a continue, so
// termination is reached by continuing until the session ends.
async function continueUntilTerminated(dc, threadId, maxStops = 10) {
  for (let i = 0; i < maxStops; i++) {
    await dc.continueRequest({ threadId });
    const event = await Promise.race([
      dc.waitForEvent('terminated').then((e) => ({ type: 'terminated', e })),
      dc.waitForEvent('exited').then((e) => ({ type: 'exited', e })),
      dc.waitForEvent('stopped').then((e) => ({ type: 'stopped', e })),
    ]);
    if (event.type !== 'stopped') {
      return;
    }
  }
  assert.fail(`debuggee did not terminate after ${maxStops} continues`);
}

// The adapter verifies a source breakpoint only once the chunk is loaded, which
// happens after configurationDone, so the client-visible sequence is:
// launch -> initialized -> setBreakpoints (maybe "Wait verify") -> configurationDone
// -> breakpoint event with verified=true -> stopped. DebugClient#hitBreakpoint
// insists on an immediately verified breakpoint and cannot express that, so the
// sequence is spelled out here.
async function stopOnBreakpoint(dc, args, location) {
  const verified = [];
  dc.on('breakpoint', (event) => {
    if (event.body && event.body.breakpoint && event.body.breakpoint.verified) {
      verified.push(event.body.breakpoint);
    }
  });

  await dc.initializeRequest();
  await launchAndWaitInitialized(dc, args);
  const response = await dc.setBreakpointsRequest({
    source: { path: location.path },
    breakpoints: [{ line: location.line, column: location.column }],
  });
  const immediate = response.body.breakpoints[0].verified;
  // The stop can be emitted as soon as `configurationDone` is processed, so the
  // waiter must be armed before the request is sent: DebugClient records events
  // only while a listener is attached.
  const stopped = dc.assertStoppedLocation('breakpoint', location);
  await dc.configurationDoneRequest();
  await stopped;
  assert.ok(immediate || verified.length > 0,
    'a breakpoint must end up verified, either in the response or via a breakpoint event');
}

// The `initialized` event is emitted as soon as the backend attaches, which can
// be before the launch response resolves; the listener has to be armed first or
// the event is missed.
async function launchAndWaitInitialized(dc, args) {
  const initialized = dc.waitForEvent('initialized');
  await dc.launchRequest(args);
  await initialized;
}

function variablesByName(body) {
  const map = new Map();
  for (const variable of body.variables) {
    map.set(variable.name, variable);
  }
  return map;
}

VERSIONS.forEach((version) => {
  describe(`dap integration [${version}]`, function () {
    this.timeout(20000);

    let dc;
    beforeEach(() => {
      dc = createClient();
      return dc.start();
    });
    afterEach(() => stopClient(dc));

    it('reports the capabilities the client depends on', async () => {
      const response = await dc.initializeRequest();
      assert.strictEqual(response.success, true);
      const caps = response.body;
      assert.strictEqual(caps.supportsConfigurationDoneRequest, true);
      assert.strictEqual(caps.supportsFunctionBreakpoints, true);
      assert.strictEqual(caps.supportsConditionalBreakpoints, true);
      assert.strictEqual(caps.supportsHitConditionalBreakpoints, true);
      assert.strictEqual(caps.supportsSetVariable, true);
      assert.strictEqual(caps.supportsRestartFrame, true);
      assert.strictEqual(caps.supportsReadMemoryRequest, true);
      assert.strictEqual(caps.supportsDisassembleRequest, true);
      assert.ok(Array.isArray(caps.exceptionBreakpointFilters));
      assert.ok(caps.exceptionBreakpointFilters.length >= 5);
    });

    it('stops on a source breakpoint and exposes real locals', async () => {
      const args = launchArgs(version);
      await stopOnBreakpoint(dc, args, { path: FIXTURE, line: 16 });

      const threads = await dc.threadsRequest();
      assert.ok(threads.body.threads.length >= 1, 'threads must be reported');
      const threadId = threads.body.threads[0].id;

      const stack = await dc.stackTraceRequest({ threadId });
      const top = stack.body.stackFrames[0];
      assert.strictEqual(top.line, 16);
      assert.strictEqual(path.resolve(top.source.path), FIXTURE);

      const scopes = await dc.scopesRequest({ frameId: top.id });
      assert.ok(scopes.body.scopes.length >= 1, 'a frame must expose scopes');
      const locals = await dc.variablesRequest({
        variablesReference: scopes.body.scopes[0].variablesReference,
      });
      const byName = variablesByName(locals.body);
      assert.ok(byName.has('x'), `locals should contain x: ${[...byName.keys()]}`);
      assert.strictEqual(Number(byName.get('x').value), 1);
    });

    it('steps in, over and out with the expected lines', async () => {
      const args = launchArgs(version);
      await stopOnBreakpoint(dc, args, { path: FIXTURE, line: 16 });

      const threads = await dc.threadsRequest();
      const threadId = threads.body.threads[0].id;

      await dc.stepInRequest({ threadId });
      await dc.assertStoppedLocation('step', { path: FIXTURE, line: 11 });

      let stack = await dc.stackTraceRequest({ threadId });
      const inner = stack.body.stackFrames[0];
      assert.strictEqual(inner.name, 'add');
      let scopes = await dc.scopesRequest({ frameId: inner.id });
      let vars = await dc.variablesRequest({
        variablesReference: scopes.body.scopes[0].variablesReference,
      });
      let byName = variablesByName(vars.body);
      assert.strictEqual(Number(byName.get('a').value), 1);
      assert.strictEqual(Number(byName.get('b').value), 2);

      await dc.nextRequest({ threadId });
      await dc.assertStoppedLocation('step', { path: FIXTURE, line: 12 });

      await dc.stepOutRequest({ threadId });
      await dc.waitForEvent('stopped');
      stack = await dc.stackTraceRequest({ threadId });
      assert.strictEqual(stack.body.stackFrames.length, 1, 'stepOut returns to the main chunk');
    });

    it('evaluates and writes back a variable', async () => {
      const args = launchArgs(version);
      await stopOnBreakpoint(dc, args, { path: FIXTURE, line: 19 });

      const threads = await dc.threadsRequest();
      const threadId = threads.body.threads[0].id;
      const stack = await dc.stackTraceRequest({ threadId });
      const frameId = stack.body.stackFrames[0].id;

      const z = await dc.evaluateRequest({ expression: 'z', frameId });
      assert.strictEqual(Number(z.body.result), 6);
      const doubled = await dc.evaluateRequest({ expression: 'z * 2', frameId });
      assert.strictEqual(Number(doubled.body.result), 12);

      const scopes = await dc.scopesRequest({ frameId });
      const ref = scopes.body.scopes[0].variablesReference;
      await dc.variablesRequest({ variablesReference: ref });  // makes `z` writable
      const written = await dc.setVariableRequest({
        variablesReference: ref, name: 'z', value: '42',
      });
      assert.strictEqual(Number(written.body.value), 42);

      await continueUntilTerminated(dc, threadId);
      const trace = readTrace(version);
      assert.deepStrictEqual(trace.slice(0, 3), ['start', 'y=3', 'z=42'],
        `the target must observe the write: ${JSON.stringify(trace)}`);
    });

    it('honours conditional breakpoints through the DAP surface', async () => {
      const args = launchArgs(version, { program: path.join(ROOT, 'test', 'e2e', 'fixtures', 'full.lua') });
      await dc.initializeRequest();
      await launchAndWaitInitialized(dc, args);
      await dc.setBreakpointsRequest({
        source: { path: args.program },
        breakpoints: [{ line: 12, condition: 'i == 3' }],
      });
      await dc.configurationDoneRequest();
      await dc.waitForEvent('stopped');
      const threads = await dc.threadsRequest();
      const threadId = threads.body.threads[0].id;
      const stack = await dc.stackTraceRequest({ threadId });
      const frameId = stack.body.stackFrames[0].id;
      const i = await dc.evaluateRequest({ expression: 'i', frameId });
      assert.strictEqual(Number(i.body.result), 3);
      await dc.continueRequest({ threadId }).catch(() => {});
    });
  });
});

describe('dap integration [default stopOnEntry]', function () {
  this.timeout(20000);
  let dc;
  beforeEach(() => {
    dc = createClient();
    return dc.start();
  });
  afterEach(() => stopClient(dc));

  it('stops on entry when the client does not override it', async () => {
    const version = VERSIONS[0];
    const args = launchArgs(version);
    delete args.stopOnEntry;
    await dc.initializeRequest();
    await launchAndWaitInitialized(dc, args);
    await dc.setBreakpointsRequest({ source: { path: FIXTURE }, breakpoints: [{ line: 16 }] });
    await dc.configurationDoneRequest();
    const stopped = await dc.waitForEvent('stopped');
    assert.strictEqual(stopped.body.reason, 'entry');
    const threadId = stopped.body.threadId;
    await dc.continueRequest({ threadId });
    await dc.assertStoppedLocation('breakpoint', { path: FIXTURE, line: 16 });
  });
});
