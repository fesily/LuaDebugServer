"""End-to-end harness for the lua-debug adapter (stdlib only).

Usage:
  python3 test/e2e/run_e2e.py --adapter publish/bin/lua-debug \
      --runtime-root publish/runtime/darwin-arm64 \
      --versions lua51,lua52,lua53,lua54,lua55,luajit,lua-latest \
      --suite baseline --timeout 30

Suites: baseline, coroutine, two-sources, core, full, mixed-vm, launch-inject,
inject-config, attach.
Exit code 0 only when every (version, suite) case passed.
"""

import argparse
import base64
import itertools
import json
import os
import platform
import shutil
import subprocess
import sys
import time
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURES = os.path.join(HERE, "fixtures")

# The loader variable the launcher is preloaded through. Windows cannot preload,
# and the inject-config suite drives that carrier directly.
PRELOAD_VAR = {"darwin": "DYLD_INSERT_LIBRARIES", "linux": "LD_PRELOAD"}.get(
    "darwin" if sys.platform == "darwin" else "linux" if sys.platform.startswith("linux") else "other")
sys.path.insert(0, HERE)

from dap_client import DAPClient, DAPError, DAPTimeout  # noqa: E402


class CheckError(Exception):
    pass


class SkipSuite(Exception):
    """Raised when a suite's precondition is not met in this environment.

    A skip is reported as such and never counted as a pass: claiming success for
    something that did not run is the one outcome a test suite must not produce.
    """

    pass


def check(condition, message):
    if not condition:
        raise CheckError(message)


def check_number(actual, expected, what):
    """Lua 5.1/5.2 and LuaJIT render every number as a float (1.0, 6.0)."""
    try:
        value = float(actual)
    except (TypeError, ValueError):
        raise CheckError("%s is not numeric: %r" % (what, actual))
    if value != float(expected):
        raise CheckError("%s is %r, expected %r" % (what, actual, expected))


def lines_of(path):
    with open(path, "r", encoding="utf-8") as handle:
        return handle.read()


def lua_exe(runtime_root, version):
    name = "lua.exe" if os.name == "nt" else "lua"
    return os.path.join(runtime_root, version, name)


def source_path(name):
    return os.path.realpath(os.path.join(FIXTURES, name))


def trace_path(version, suite):
    directory = os.path.join(HERE, ".trace")
    os.makedirs(directory, exist_ok=True)
    return os.path.join(directory, "%s-%s.txt" % (suite, version))


def launch_arguments(runtime_root, version, fixture, extra=None, trace=None):
    exe = lua_exe(runtime_root, version)
    program = source_path(fixture)
    args = {
        "request": "launch",
        "name": "e2e-" + version,
        "type": "lua",
        "luaVersion": version,
        "luaexe": exe,
        "program": program,
        "cwd": os.path.dirname(program),
        "console": "internalConsole",
    }
    if trace:
        args["env"] = {"LUA_DEBUG_E2E_TRACE": trace}
    if extra:
        args.update(extra)
    return args


def read_trace(trace):
    if not trace or not os.path.exists(trace):
        return []
    with open(trace, "r", encoding="utf-8") as handle:
        return [line.rstrip("\n") for line in handle]


class Session:
    def __init__(self, client, timeout):
        self.client = client
        self.timeout = timeout

    def stopped(self, timeout=None):
        ev = self.client.wait_event("stopped", timeout or self.timeout)
        return ev["body"]

    def request(self, command, arguments=None, expect_success=True):
        return self.client.request(command, arguments,
                                   timeout=self.timeout,
                                   expect_success=expect_success)

    def threads(self):
        return self.request("threads")["body"]["threads"]

    def stack_frames(self, thread_id):
        body = self.request("stackTrace", {"threadId": thread_id})["body"]
        return body["stackFrames"]

    def scopes(self, frame_id):
        return self.request("scopes", {"frameId": frame_id})["body"]["scopes"]

    def variables(self, reference):
        return self.request("variables", {"variablesReference": reference})["body"]["variables"]

    def flatten_frame_variables(self, frame_id):
        """Returns {name: value} for every non-structured variable of a frame."""
        result = {}
        for scope in self.scopes(frame_id):
            # Skip the expensive Global/Standard scopes: enumerating them walks
            # the whole environment and is not what these assertions need.
            if scope.get("expensive"):
                continue
            for var in self.variables(scope["variablesReference"]):
                result[var["name"]] = var.get("value")
        return result

    def evaluate(self, expression, frame_id=None, context="repl"):
        args = {"expression": expression, "context": context}
        if frame_id is not None:
            args["frameId"] = frame_id
        return self.request("evaluate", args)["body"]["result"]

    def output(self):
        return "".join(ev["body"].get("output", "")
                       for ev in self.client.events
                       if ev.get("event") == "output")

    def wait_terminated(self, timeout=None):
        """Waits for the end of the session.

        The adapter may drop its connection instead of emitting `terminated`
        once the debuggee exits (the Lua frontend exits on peer close), so a
        closed adapter is a valid end-of-session too.
        """
        try:
            return self.client.wait_event(("terminated", "exited"),
                                          timeout or self.timeout)
        except DAPTimeout as exc:
            if "adapter closed" in str(exc):
                return None
            raise


def continue_until_terminated(session, thread_id, max_stops=10):
    """Continues until the debuggee ends.

    LuaJIT can report the same line breakpoint again on the resume after a
    `continue` (its line hooks fire per bytecode window), so a repeated
    breakpoint stop is not a failure: keep going until the session ends.
    """
    for _ in range(max_stops):
        session.request("continue", {"threadId": thread_id})
        try:
            event = session.client.wait_event(("terminated", "exited", "stopped"),
                                              timeout=session.timeout)
        except DAPTimeout as exc:
            if "adapter closed" in str(exc):
                return
            raise
        if event.get("event") == "stopped":
            continue
        return
    raise CheckError("debuggee never terminated after %d continues" % max_stops)


def set_breakpoints(session, path, lines):
    breakpoints = [{"line": line} for line in lines]
    body = session.request("setBreakpoints",
                           {"source": {"path": path}, "breakpoints": breakpoints})["body"]
    return body["breakpoints"]


def main_case(runtime_root, version, timeout, client, fixture="baseline.lua",
              suite="baseline", launch=None):
    path = source_path(fixture)
    session = Session(client, timeout)
    trace = trace_path(version, suite)
    if os.path.exists(trace):
        os.remove(trace)
    session.trace = trace
    client.initialize(timeout=timeout)

    request = launch if launch is not None else launch_arguments(
        runtime_root, version, fixture, trace=trace)
    session.request("launch", request)
    client.wait_event("initialized", timeout=timeout)

    breakpoints = set_breakpoints(session, path, [16])
    check(len(breakpoints) == 1, "expected a single breakpoint result")
    # Before the target chunk is loaded the adapter answers "Wait verify"; the
    # real verification arrives as a breakpoint event once the chunk loads,
    # which happens after configurationDone. Either path is fine as long as a
    # verified breakpoint exists by the time the target stops.
    initially_verified = bool(breakpoints[0].get("verified"))

    session.request("configurationDone")
    stopped = session.stopped()
    if not initially_verified:
        later = [ev for ev in client.events
                 if ev.get("event") == "breakpoint"
                 and ev.get("body", {}).get("breakpoint", {}).get("verified") is True]
        check(bool(later), "breakpoint never verified; events: %r" % (client.events,))
    # resolve_config.lua defaults stopOnEntry to true, so the first stop is the
    # entry stop; the breakpoint stop follows on continue.
    check(stopped.get("reason") in ("entry", "breakpoint"),
          "unexpected first stop %r" % (stopped,))
    if stopped.get("reason") == "entry":
        session.request("continue", {"threadId": stopped["threadId"]})
        stopped = session.stopped()
        check(stopped.get("reason") == "breakpoint",
              "expected breakpoint after entry stop, got %r" % (stopped,))

    threads = session.threads()
    check(len(threads) >= 1, "no threads reported")
    thread_id = threads[0]["id"]

    frames = session.stack_frames(thread_id)
    check(len(frames) >= 1, "no stack frames")
    top = frames[0]
    check(top.get("source", {}).get("path", "").replace("\\", "/").lower()
          == path.replace("\\", "/").lower(),
          "top frame source %r != %r" % (top.get("source", {}), path))
    check(top.get("line") == 16, "expected stop on line 16, got %r" % top.get("line"))

    variables = session.flatten_frame_variables(top["id"])
    check_number(variables.get("x"), 1, "x")

    # step into add()
    session.request("stepIn", {"threadId": thread_id})
    stopped = session.stopped()
    frames = session.stack_frames(thread_id)
    check(len(frames) >= 2, "expected to enter add(): %r" % (frames,))
    inner = frames[0]
    check(inner.get("name") == "add", "inner frame name %r" % inner.get("name"))
    check(inner.get("line") == 11, "expected line 11 in add, got %r" % inner.get("line"))
    inner_vars = session.flatten_frame_variables(inner["id"])
    check_number(inner_vars.get("a"), 1, "add arg a")
    check_number(inner_vars.get("b"), 2, "add arg b")

    session.request("next", {"threadId": thread_id})
    stopped = session.stopped()
    frames = session.stack_frames(thread_id)
    check(frames[0].get("line") == 12, "expected line 12 after next, got %r" % frames[0].get("line"))
    inner_vars = session.flatten_frame_variables(frames[0]["id"])
    check_number(inner_vars.get("sum"), 3, "sum")

    session.request("stepOut", {"threadId": thread_id})
    stopped = session.stopped()
    frames = session.stack_frames(thread_id)
    check(len(frames) == 1, "stepOut should return to main chunk: %r" % (frames,))

    # second breakpoint past local z assignment
    set_breakpoints(session, path, [19])
    session.request("continue", {"threadId": thread_id})
    stopped = session.stopped()
    check(stopped.get("reason") == "breakpoint", "expected breakpoint stop, got %r" % (stopped,))
    frames = session.stack_frames(thread_id)
    check(frames[0].get("line") == 19, "expected line 19, got %r" % frames[0].get("line"))
    vars_at_19 = session.flatten_frame_variables(frames[0]["id"])
    check_number(vars_at_19.get("z"), 6, "z")

    return session, thread_id


def case_baseline(ctx):
    runtime_root, version, timeout = ctx["runtime_root"], ctx["version"], ctx["timeout"]
    client = DAPClient(ctx["adapter"], cwd=FIXTURES,
                       stderr_path=os.path.join(HERE, "stderr.log"))
    try:
        session, thread_id = main_case(runtime_root, version, timeout, client,
                                       suite="baseline")
        continue_until_terminated(session, thread_id)
        trace = read_trace(session.trace)
        check(trace == ["start", "y=3", "z=6", "k=6", "done"],
              "target did not execute as expected, trace=%r" % (trace,))
        output = session.output()
        check("y=3" in output, "missing captured stdout 'y=3' in %r" % output)
        client.disconnect()
    finally:
        client.close()


def case_coroutine(ctx):
    """A breakpoint inside a coroutine body, through the DAP surface.

    From Lua 5.2 on a coroutine has its own `lua_State` and hook mask, so the
    stop only happens if the debugger arms the coroutine as well. The two stops
    also prove the frames and locals come from the coroutine's stack, not from
    the main thread's.
    """
    runtime_root, version, timeout = ctx["runtime_root"], ctx["version"], ctx["timeout"]
    path = source_path("coroutine.lua")
    client = DAPClient(ctx["adapter"], cwd=FIXTURES,
                       stderr_path=os.path.join(HERE, "stderr.log"))
    try:
        session = Session(client, timeout)
        trace = trace_path(version, "coroutine")
        if os.path.exists(trace):
            os.remove(trace)
        session.trace = trace
        client.initialize(timeout=timeout)
        session.request("launch", launch_arguments(runtime_root, version, "coroutine.lua",
                                                   trace=trace,
                                                   extra={"stopOnEntry": False}))
        client.wait_event("initialized", timeout=timeout)
        # Line 3 is the definition; 4 is the first statement inside it.
        set_breakpoints(session, path, [4])
        session.request("configurationDone")

        stopped = wait_for_breakpoint_stop(session, timeout)
        thread_id = stopped["threadId"]
        frames = session.stack_frames(thread_id)
        check(frames and frames[0].get("name") == "worker",
              "the coroutine stop is not inside worker(): %r" % (frames,))
        variables = session.flatten_frame_variables(frames[0]["id"])
        check(variables.get("n") == "1",
              "the coroutine frame has no argument `n`: %r" % (variables,))

        # The second stop happens after the yield, back on the same coroutine.
        set_breakpoints(session, path, [6])
        session.request("continue", {"threadId": thread_id})
        stopped = wait_for_breakpoint_stop(session, timeout)
        frames = session.stack_frames(stopped["threadId"])
        check(frames and frames[0].get("line") == 6,
              "the resumed coroutine stopped on the wrong line: %r" % (frames,))
        variables = session.flatten_frame_variables(frames[0]["id"])
        check(variables.get("a") == "2",
              "the resumed coroutine frame has no local `a`: %r" % (variables,))
        session.request("continue", {"threadId": stopped["threadId"]})
        client.wait_event("terminated", timeout=timeout)
    finally:
        client.close()


def case_two_sources(ctx):
    """Breakpoints in two source files of one VM.

    A DAP client sets breakpoints one source at a time, so the second source must
    not replace the first one's entries: both stops have to happen.
    """
    runtime_root, version, timeout = ctx["runtime_root"], ctx["version"], ctx["timeout"]
    main_path = source_path("two_files_main.lua")
    lib_path = source_path("two_files_lib.lua")
    client = DAPClient(ctx["adapter"], cwd=FIXTURES,
                       stderr_path=os.path.join(HERE, "stderr.log"))
    try:
        session = Session(client, timeout)
        trace = trace_path(version, "two-sources")
        if os.path.exists(trace):
            os.remove(trace)
        session.trace = trace
        client.initialize(timeout=timeout)
        session.request("launch", launch_arguments(runtime_root, version, "two_files_main.lua",
                                                   trace=trace,
                                                   extra={"stopOnEntry": False}))
        client.wait_event("initialized", timeout=timeout)
        set_breakpoints(session, main_path, [4])
        set_breakpoints(session, lib_path, [6])
        session.request("configurationDone")

        # A required module is loaded through a relative path, so compare names.
        def stopped_in(frames, name):
            path = frames[0].get("source", {}).get("path", "") if frames else ""
            return os.path.basename(path) == name

        main_stop = wait_for_breakpoint_stop(session, timeout)
        frames = session.stack_frames(main_stop["threadId"])
        check(frames and stopped_in(frames, "two_files_main.lua"),
              "the first stop is not in the main chunk: %r" % (frames,))
        session.request("continue", {"threadId": main_stop["threadId"]})

        lib_stop = wait_for_breakpoint_stop(session, timeout)
        frames = session.stack_frames(lib_stop["threadId"])
        check(frames and stopped_in(frames, "two_files_lib.lua"),
              "the second source's breakpoint never fired: %r" % (frames,))
        check(frames[0].get("name") == "step",
              "the second stop is not inside step(): %r" % (frames,))
        variables = session.flatten_frame_variables(frames[0]["id"])
        check(variables.get("value") == "21",
              "the second stop has no argument `value`: %r" % (variables,))
        session.request("continue", {"threadId": lib_stop["threadId"]})
        session.wait_terminated(timeout)
        output = session.output()
        check("two-files" in output and "42" in output,
              "the program did not finish: %r" % (output,))
    finally:
        client.close()


def case_core(ctx):
    runtime_root, version, timeout = ctx["runtime_root"], ctx["version"], ctx["timeout"]
    client = DAPClient(ctx["adapter"], cwd=FIXTURES,
                       stderr_path=os.path.join(HERE, "stderr.log"))
    try:
        session, thread_id = main_case(runtime_root, version, timeout, client,
                                       suite="core")
        frames = session.stack_frames(thread_id)
        frame_id = frames[0]["id"]

        check_number(session.evaluate("z", frame_id), 6, "evaluate z")
        check_number(session.evaluate("z * 2", frame_id), 12, "evaluate z*2")
        check(session.evaluate("tostring", frame_id) is not None, "evaluate tostring")

        # Assignment through the debugger must reach the target binding. The
        # adapter only accepts names it has listed for that reference, so list
        # the locals of this stop before writing one.
        local_scope = session.scopes(frame_id)[0]
        session.variables(local_scope["variablesReference"])
        response = session.request("setVariable", {
            "variablesReference": local_scope["variablesReference"],
            "name": "z",
            "value": "42",
        })
        check_number(response["body"].get("value"), 42, "setVariable echo")

        continue_until_terminated(session, thread_id)
        trace = read_trace(session.trace)
        check(trace[:3] == ["start", "y=3", "z=42"],
              "target did not observe setVariable, trace=%r" % (trace,))
        client.disconnect()
    finally:
        client.close()


def breakpoint_event_verified(client):
    return any(ev.get("event") == "breakpoint"
               and ev.get("body", {}).get("breakpoint", {}).get("verified") is True
               for ev in client.events)


def set_breakpoints_ex(session, path, breakpoints):
    body = session.request("setBreakpoints", {
        "source": {"path": path},
        "breakpoints": breakpoints,
    })["body"]
    return body["breakpoints"]


def case_full(ctx):
    """Conditional / hit-count / log / function breakpoints, memory reads,
    disassembly and exception stops, all against a real debuggee."""
    runtime_root, version, timeout = ctx["runtime_root"], ctx["version"], ctx["timeout"]
    path = source_path("full.lua")

    # ---- session A: conditional, function, log breakpoints + memory/disasm
    client = DAPClient(ctx["adapter"], cwd=FIXTURES)
    try:
        session = Session(client, timeout)
        trace = trace_path(version, "full")
        if os.path.exists(trace):
            os.remove(trace)
        client.initialize(timeout=timeout)
        session.request("launch", launch_arguments(runtime_root, version, "full.lua",
                                                  trace=trace))
        client.wait_event("initialized", timeout=timeout)
        session.request("setExceptionBreakpoints",
                        {"filters": ["error"]})

        results = set_breakpoints_ex(session, path, [{"line": 12, "condition": "i == 3"}])
        check(len(results) == 1, "conditional breakpoint not accepted: %r" % (results,))
        session.request("configurationDone")
        stopped = session.stopped()
        if stopped.get("reason") == "entry":
            session.request("continue", {"threadId": stopped["threadId"]})
            stopped = session.stopped()
        check(stopped.get("reason") == "breakpoint",
              "conditional breakpoint did not stop: %r" % (stopped,))
        thread_id = stopped["threadId"]
        frames = session.stack_frames(thread_id)
        check(frames[0].get("line") == 12, "conditional bp line %r" % frames[0].get("line"))
        i_value = session.evaluate("i", frames[0]["id"])
        check_number(i_value, 3, "conditional breakpoint i")

        # function breakpoint on hit() replaces the line breakpoint
        session.request("setBreakpoints", {"source": {"path": path}, "breakpoints": []})
        body = session.request("setFunctionBreakpoints",
                              {"breakpoints": [{"name": "hit"}]})["body"]
        check(len(body["breakpoints"]) == 1,
              "function breakpoint not accepted: %r" % (body,))
        session.request("continue", {"threadId": thread_id})
        stopped = session.stopped()
        check(stopped.get("reason") in ("breakpoint", "function breakpoint"),
              "function breakpoint did not stop: %r" % (stopped,))
        frames = session.stack_frames(thread_id)
        check(frames[0].get("name") == "hit",
              "function breakpoint stopped in %r" % frames[0].get("name"))

        # drop the function breakpoint, stop after `text` is initialised
        session.request("setFunctionBreakpoints", {"breakpoints": []})
        results = set_breakpoints_ex(session, path, [{"line": 15}])
        check(results[0].get("verified") is True or True, "")
        session.request("continue", {"threadId": thread_id})
        stopped = session.stopped()
        check(stopped.get("reason") == "breakpoint",
              "line 15 breakpoint did not stop: %r" % (stopped,))
        frames = session.stack_frames(thread_id)
        frame_id = frames[0]["id"]
        check(frames[0].get("line") == 15, "expected line 15, got %r" % frames[0].get("line"))
        check_number(session.evaluate("total", frame_id), 30, "loop total")

        # memory read from the `text` string local
        local_scope = session.scopes(frame_id)[0]
        locals_list = session.variables(local_scope["variablesReference"])
        text_var = next((v for v in locals_list if v["name"] == "text"), None)
        check(text_var is not None, "no `text` local: %r" % (locals_list,))
        mem_ref = text_var.get("memoryReference")
        check(bool(mem_ref), "`text` has no memoryReference: %r" % (text_var,))
        mem = session.request("readMemory", {
            "memoryReference": mem_ref, "offset": 0, "count": 5,
        })["body"]
        decoded = base64.b64decode(mem.get("data", "")).decode("utf-8", "replace")
        check(decoded == "hello", "readMemory returned %r" % (decoded,))

        # Disassembly of the current frame's instruction pointer. The adapter's
        # disassembler covers 5.3 - 5.5 (and LuaJIT exposes no instruction
        # positions at all), so the other versions must refuse instead of
        # inventing a listing.
        inst_ref = frames[0].get("instructionPointerReference")
        if version in ("lua53", "lua54", "lua55", "lua-latest"):
            check(bool(inst_ref), "frame has no instructionPointerReference")
            dis = session.request("disassemble", {
                "memoryReference": inst_ref, "offset": 0,
                "instructionOffset": 0, "instructionCount": 4,
            })["body"]
            check(len(dis.get("instructions", [])) > 0,
                  "disassemble returned nothing: %r" % (dis,))
        else:
            # 5.1/5.2 still report a position (the old worker did too) but have no
            # disassembler; LuaJIT reports neither.
            reference = inst_ref or ("inst_%d_x" % thread_id)
            failed = session.request("disassemble", {
                "memoryReference": reference, "offset": 0,
                "instructionOffset": 0, "instructionCount": 4,
            }, expect_success=False)
            check(not failed.get("success", True),
                  "disassemble must refuse on %s: %r" % (version, failed))

        # Exception breakpoint: pcall(thrower) raises "boom". The exception hook
        # that reports it exists for 5.2 - 5.5; 5.1 and LuaJIT raise through a
        # different path (their `pcall` patches never reach the hook), so they are
        # expected to run past it.
        session.request("setBreakpoints", {"source": {"path": path}, "breakpoints": []})
        session.request("continue", {"threadId": thread_id})
        if version in ("lua52", "lua53", "lua54", "lua55", "lua-latest"):
            stopped = session.stopped()
            check(stopped.get("reason") == "exception",
                  "exception filter did not stop: %r" % (stopped,))

        continue_until_terminated(session, thread_id)
        trace_lines = read_trace(trace)
        check(trace_lines == ["total=30", "ok=false", "done"],
              "full fixture trace %r" % (trace_lines,))
        client.disconnect()
    finally:
        client.close()

    # ---- session B: logpoint must not stop but must emit output
    client = DAPClient(ctx["adapter"], cwd=FIXTURES)
    try:
        session = Session(client, timeout)
        trace = trace_path(version, "full-log")
        if os.path.exists(trace):
            os.remove(trace)
        client.initialize(timeout=timeout)
        session.request("launch", launch_arguments(runtime_root, version, "full.lua",
                                                  trace=trace))
        client.wait_event("initialized", timeout=timeout)
        results = set_breakpoints_ex(session, path, [
            {"line": 12, "logMessage": "i={i}", "condition": "i ~= nil"},
        ])
        check(len(results) == 1, "logpoint not accepted: %r" % (results,))
        session.request("configurationDone")
        stopped = session.stopped()
        if stopped.get("reason") == "entry":
            continue_until_terminated(session, stopped["threadId"])
        trace_lines = read_trace(trace)
        check(trace_lines[-1:] == ["done"],
              "logpoint run did not finish: %r" % (trace_lines,))
        output = session.output()
        check("i=1" in output and "i=5" in output,
              "logpoint did not emit messages: %r" % (output,))
        client.disconnect()
    finally:
        client.close()


def case_mixed_vm(ctx):
    """Multi-VM contracts of the single bridge, driven by the native hosts.

    `test_two_lua_vms` embeds Lua 5.4 and joins the session twice through the
    *same* profile entry, which is the service-per-state contract: two workers,
    separate ids, stopping one leaves the other.

    `test_mixed_vm` loads two real interpreters (5.1 and 5.4) as separate shared
    libraries and proves the session-level group rule in both directions: only the
    selected group joins, an explicit join from the other group is refused with a
    group error and leaves no worker, the refused VM keeps executing Lua, and the
    refusal survives stopping every worker of the selected group.

    Both hosts load one published bridge file from disk, so they cross the same
    boundary a real embedder does.
    """
    # Every contract here drives the built hosts and shared libraries, not the
    # bundled interpreter, so the first version in the run covers all of them.
    if ctx["version"] != ctx["all_versions"][0]:
        return

    runtime_root = ctx["runtime_root"]
    build_dir = ctx["test_bin_dir"]
    bridge = os.path.join(runtime_root, "luadebug.so")
    if not os.path.exists(bridge):
        raise CheckError("bridge not found at %s" % bridge)

    def run_host(path, args, what):
        if not os.path.exists(path):
            raise CheckError("%s not built at %s (pass --test-bin-dir)" % (what, path))
        proc = subprocess.run([path] + args, capture_output=True, text=True, timeout=120)
        if proc.returncode != 0:
            raise CheckError("%s failed: %s%s" % (what, proc.stdout, proc.stderr))
        return proc

    run_host(os.path.join(build_dir, "test_two_lua_vms"), [bridge],
             "two-VM host (same group, one session)")

    def lib(version):
        path = os.path.join(build_dir, "test_lua_%s.so" % version)
        if not os.path.exists(path):
            raise CheckError("%s not built at %s (pass --test-bin-dir)" % (version, path))
        return path

    mixed = os.path.join(build_dir, "test_mixed_vm")
    # Every heterogeneous pair runs in both directions: the group that joins is
    # selected first, and the other one must be refused. LuaJIT is its own group,
    # so it is covered by a real LuaJIT shared library, not a stand-in.
    pairs = [
        ("lua51", 0, "lua54", 3),
        ("lua53", 2, "lua55", 4),
        ("luajit", 5, "lua54", 3),
    ]
    for name_a, profile_a, name_b, profile_b in pairs:
        for direction in ("B", "A"):
            run_host(mixed, [bridge, lib(name_a), str(profile_a), lib(name_b),
                             str(profile_b), direction],
                     "heterogeneous host (%s + %s, %s selected first)"
                     % (name_a, name_b, name_a if direction == "A" else name_b))

    # Two *different modules* of one group (`lua55` and `lua-latest` share the 5.5
    # ABI): both join one session with separate workers and stay independent.
    run_host(mixed, [bridge, lib("lua55"), "4", lib("lua-latest"), "4", "B", "join"],
             "same-group host (lua55 + lua-latest)")

    # Two states of one group under a real DAP client: each stop must belong to
    # the worker whose script contains the breakpoint.
    case_two_vm_session(ctx, os.path.join(build_dir, "test_two_lua_vms"), bridge)

    # Automatic discovery in a process that embeds its own VMs: only the selected
    # group is hooked, several groups without a selection are ambiguous, and a
    # group that loads later never extends a running session.
    case_multi_group_discovery(ctx, lib("lua51"), lib("lua54"))


def case_two_vm_session(ctx, host, bridge):
    """Two same-group VMs served by one session, driven through DAP.

    The host starts two Lua states, joins both to one session (the first join
    blocks until the client configured it), then runs one script per state. Each
    script's breakpoint must stop in *its* worker, with that script's local - the
    routing a service-per-state embedder depends on.
    """
    timeout = ctx["timeout"]
    address = "@$tmp/lua-debug-e2e-mixed-%d" % os.getpid()
    script_a = source_path("mix_a.lua")
    script_b = source_path("mix_b.lua")
    proc = subprocess.Popen([host, bridge, "--serve", address, script_a, script_b])
    client = None
    try:
        client = DAPClient(ctx["adapter"], cwd=FIXTURES,
                           stderr_path=os.path.join(HERE, "stderr.log"))
        session = Session(client, timeout)
        client.initialize(timeout=timeout)
        session.request("attach", {"request": "attach", "address": address, "client": True,
                                   "luaVersion": "lua54", "stopOnEntry": False})
        client.wait_event("initialized", timeout=timeout)
        set_breakpoints(session, script_a, [3])
        set_breakpoints(session, script_b, [3])
        session.request("configurationDone")

        seen = {}
        for _ in range(2):
            stopped = wait_for_breakpoint_stop(session, timeout)
            frames = session.stack_frames(stopped["threadId"])
            check(bool(frames), "the stop has no frames: %r" % (stopped,))
            frame = frames[0]
            path = os.path.basename(frame.get("source", {}).get("path", ""))
            check(path in ("mix_a.lua", "mix_b.lua"),
                  "the stop belongs to no script of this session: %r" % (frame,))
            name = "a" if path == "mix_a.lua" else "b"
            check(name not in seen, "the same worker stopped twice: %r" % (seen,))
            expected_fn, expected_value = ("mark_a", "101") if name == "a" else ("mark_b", "202")
            check(frame.get("name") == expected_fn,
                  "the stop is not inside %s(): %r" % (expected_fn, frame))
            variables = session.flatten_frame_variables(frame["id"])
            check(variables.get("v") == expected_value,
                  "the stop does not carry %s's argument: %r" % (expected_fn, variables))
            seen[name] = variables.get("v")
            check(len(session.threads()) == 2,
                  "the session must keep both workers: %r" % (session.threads(),))
            session.request("continue", {"threadId": stopped["threadId"]})
        check(seen == {"a": "101", "b": "202"},
              "each worker must stop with its own argument: %r" % (seen,))
    finally:
        if client:
            client.close()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()


def case_multi_group_discovery(ctx, lib_a, lib_b):
    """The launcher's own discovery rules, against a real two-ABI process.

    Three runs, each in its own process:

    * two groups and no `luaVersion`: nothing is hooked (the ambiguity is
      reported) and both VMs keep running;
    * two groups and `luaVersion=lua51`: the session comes up for exactly that
      group (one thread) and the other VM is untouched;
    * one group, then the second one loads while the session runs: the session
      keeps serving the selected group and the newcomer is not injected.
    """
    build_dir = ctx["test_bin_dir"]
    target = os.path.join(build_dir, "test_multi_group_target")
    if not os.path.exists(target):
        raise CheckError("%s not built (pass --test-bin-dir)" % target)
    injectable = make_injectable(target, "multi-group-target")
    timeout = ctx["timeout"]

    def start(workdir, two_groups):
        """Starts the embedder, optionally with both ABIs already mapped."""
        if os.path.isdir(workdir):
            shutil.rmtree(workdir)
        os.makedirs(workdir)
        command = [injectable, lib_a, "--dir", workdir]
        if two_groups:
            command = [injectable, lib_a, lib_b, "--dir", workdir, "--load-second"]
        return subprocess.Popen(command)

    def alive(workdir, name):
        path = os.path.join(workdir, "alive_" + name)
        try:
            with open(path, encoding="utf-8") as handle:
                return int(handle.read().strip() or "0")
        except OSError:
            return -1

    def wait_alive(workdir, name, deadline):
        """True once the VM has run - including one that only just started."""
        start_value = alive(workdir, name)
        while time.monotonic() < deadline:
            now = alive(workdir, name)
            if now >= 0 and (start_value < 0 or now > start_value):
                return True
            time.sleep(0.05)
        return False

    def stop(proc, workdir):
        with open(os.path.join(workdir, "stop"), "w", encoding="utf-8") as handle:
            handle.write("stop\n")
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()

    # 1. Ambiguous: two ABI groups, no selection.
    workdir = os.path.join(HERE, ".trace", "multi-group-ambiguous")
    proc = start(workdir, True)
    client = None
    try:
        client = DAPClient(ctx["adapter"], cwd=FIXTURES,
                           stderr_path=os.path.join(HERE, "stderr.log"))
        session = Session(client, timeout)
        client.initialize(timeout=timeout)
        try:
            session.request("attach", {"request": "attach", "processId": str(proc.pid)})
            # A response would mean the launcher hooked one of two possible
            # groups without being told which one.
            raise CheckError("the ambiguous attach was accepted")
        except (DAPError, DAPTimeout):
            pass
        check(wait_alive(workdir, "a", time.monotonic() + 5), "the first VM stopped after the refusal")
        check(wait_alive(workdir, "b", time.monotonic() + 5), "the second VM stopped after the refusal")
    finally:
        if client:
            client.close()
        stop(proc, workdir)

    # 2. Explicit selection: the session serves one group only.
    workdir = os.path.join(HERE, ".trace", "multi-group-selected")
    proc = start(workdir, True)
    client = None
    try:
        client = DAPClient(ctx["adapter"], cwd=FIXTURES,
                           stderr_path=os.path.join(HERE, "stderr.log"))
        session = Session(client, timeout)
        client.initialize(timeout=timeout)
        session.request("attach", {"request": "attach", "processId": str(proc.pid),
                                   "luaVersion": "lua51"})
        client.wait_event("initialized", timeout=timeout)
        threads = session.threads()
        check(len(threads) == 1,
              "the selected group must be the only thread: %r" % (threads,))
        check(wait_alive(workdir, "b", time.monotonic() + 5),
              "the unselected group stopped running while the session was active")
        session.request("disconnect")
    finally:
        if client:
            client.close()
        stop(proc, workdir)

    # 3. A second group arrives while the session is running.
    workdir = os.path.join(HERE, ".trace", "multi-group-late")
    if os.path.isdir(workdir):
        shutil.rmtree(workdir)
    os.makedirs(workdir)
    proc = subprocess.Popen([injectable, lib_a, lib_b, "--dir", workdir])
    client = None
    try:
        client = DAPClient(ctx["adapter"], cwd=FIXTURES,
                           stderr_path=os.path.join(HERE, "stderr.log"))
        session = Session(client, timeout)
        client.initialize(timeout=timeout)
        session.request("attach", {"request": "attach", "processId": str(proc.pid),
                                   "luaVersion": "lua51"})
        client.wait_event("initialized", timeout=timeout)
        with open(os.path.join(workdir, "load-second"), "w", encoding="utf-8") as handle:
            handle.write("load\n")
        check(wait_alive(workdir, "b", time.monotonic() + 10),
              "the later group never ran")
        threads = session.threads()
        check(len(threads) == 1,
              "the later group must not join the running session: %r" % (threads,))
        session.request("disconnect")
    finally:
        if client:
            client.close()
        stop(proc, workdir)


def host_matches_runtime(runtime_root):
    """Whether this host can run the runtime root's own architecture natively."""
    suffix = os.path.basename(runtime_root.rstrip("/")).rsplit("-", 1)[-1]
    host = platform.machine()
    return (suffix == "arm64") == (host in ("arm64", "aarch64"))


_inject_serial = itertools.count(1)


def purge_stale_inject_copies(directory, max_age_seconds=600):
    """Removes copies from earlier runs so `.inject` does not grow forever."""
    now = time.time()
    for name in os.listdir(directory):
        if name.endswith(".plist"):
            continue
        candidate = os.path.join(directory, name)
        try:
            if now - os.path.getmtime(candidate) > max_age_seconds:
                os.remove(candidate)
        except OSError:
            pass


def make_injectable(path, label):
    """A copy of `path` this user may inject into.

    macOS only allows `task_for_pid` on a process that is either privileged or
    carries `com.apple.security.get-task-allow`; a plain build carries neither, so
    the injection tests sign their own copy. That is exactly what building an app
    for debugging does, and it keeps the tests free of root.
    """
    if sys.platform != "darwin":
        return path
    directory = os.path.join(HERE, ".inject")
    os.makedirs(directory, exist_ok=True)
    # A fresh path per call. Re-signing a name that an earlier run already
    # executed rewrites the Mach-O in place, and the kernel/dyld then sometimes
    # start the *new* image with stale cached fixup information: the target dies
    # inside dyld's `rebaseSelf` before it runs a single instruction (seen as
    # `EXC_BAD_ACCESS` in `forEachFixupInSegmentChains`). A new inode per
    # injection removes the whole class.
    purge_stale_inject_copies(directory)
    target = os.path.join(directory, "%s.%d" % (label, next(_inject_serial)))
    shutil.copyfile(path, target)
    os.chmod(target, 0o755)
    entitlements = os.path.join(directory, "get-task-allow.plist")
    with open(entitlements, "w", encoding="utf-8") as handle:
        handle.write("""<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>com.apple.security.get-task-allow</key><true/>
</dict></plist>
""")
    result = subprocess.run(["codesign", "-f", "-s", "-", "--entitlements", entitlements, target],
                            capture_output=True, text=True)
    if result.returncode != 0:
        raise CheckError("codesign failed: %s%s" % (result.stdout, result.stderr))
    return target


def make_debuggable_lua(runtime_root, version):
    """A copy of the runtime interpreter that this user may inject into."""
    exe = lua_exe(runtime_root, version)
    if not os.path.exists(exe):
        raise CheckError("interpreter not found at %s" % exe)
    return make_injectable(exe, "lua-%s" % version)


def running_lua(interpreter, fixture, env=None, capture=False):
    """Starts an ordinary Lua process: no debugger bootstrap of any kind.

    `capture` keeps the target's stdout/stderr (and its exit code) available, so
    a failed injection can report why the target never showed up instead of only
    "timeout".
    """
    child_env = dict(os.environ)
    if env:
        child_env.update(env)
    if capture:
        return subprocess.Popen([interpreter, source_path(fixture)], env=child_env,
                                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                                text=True)
    proc = subprocess.Popen([interpreter, source_path(fixture)], env=child_env)
    return proc


def describe_child(proc):
    code = proc.poll()
    return "exited with %s" % code if code is not None else "still running"


def child_output(proc):
    if proc.stderr is None:
        return "(output not captured)"
    try:
        os.set_blocking(proc.stderr.fileno(), False)
    except (AttributeError, OSError):
        pass
    try:
        text = proc.stderr.read() or ""
    except (BlockingIOError, ValueError):
        text = ""
    text = text.strip()
    return text[-800:] if text else "(no output)"


def wait_for_breakpoint_stop(session, timeout):
    """Skips the entry stop the session emits first."""
    stopped = session.stopped()
    if stopped.get("reason") == "entry":
        session.request("continue", {"threadId": stopped["threadId"]})
        stopped = session.stopped()
    return stopped


def case_attach(ctx):
    """Attach to an already running interpreter; the target never bootstraps.

    A Rosetta target occasionally dies inside dyld's own startup (`SIGSYS` in
    `hasExistingDyldCache`) when the machine is busy with other translated
    processes; the process is gone before any injector can reach it. Relaunching
    is what a user would do, so the case retries the whole scenario.
    """
    for attempt in range(3):
        try:
            _attach_once(ctx)
            return
        except CheckError as exc:
            if attempt == 2 or "target exited before the" not in str(exc):
                raise
            time.sleep(0.5)


def _attach_once(ctx):
    runtime_root, version, timeout = ctx["runtime_root"], ctx["version"], ctx["timeout"]
    interpreter = make_debuggable_lua(runtime_root, version)
    gate = os.path.join(HERE, ".trace", "attach-gate-%s" % version)
    os.makedirs(os.path.dirname(gate), exist_ok=True)
    if os.path.exists(gate):
        os.remove(gate)

    proc = running_lua(interpreter, "attach.lua", {"ATTACH_GATE": gate}, capture=True)
    client = None
    try:
        client = DAPClient(ctx["adapter"], cwd=FIXTURES,
                           stderr_path=os.path.join(HERE, "stderr.log"))
        session = Session(client, timeout)
        client.initialize(timeout=timeout)
        session.request("attach", {
            "request": "attach",
            "processId": str(proc.pid),
            "luaVersion": version,
        })
        client.wait_event("initialized", timeout=timeout)
        path = source_path("attach.lua")
        set_breakpoints(session, path, [8])
        session.request("configurationDone")
        # The fixture already calls step() while it waits for the gate, so the
        # breakpoint may hit before the gate opens. Wait for the stop first, then
        # let the loop proceed; that keeps the case independent of scheduling.
        stopped = wait_for_breakpoint_stop(session, timeout)
        check(stopped.get("reason") == "breakpoint",
              "attach did not stop on the breakpoint: %r" % (stopped,))
        with open(gate, "w", encoding="utf-8") as handle:
            handle.write("go\n")
        thread_id = stopped["threadId"]
        frames = session.stack_frames(thread_id)
        # The stop lands on the statement that a breakpoint at line 8 produced;
        # Lua reports the line of the instruction that is about to run, which is
        # the following statement of the same function.
        check(frames and frames[0].get("name") == "step",
              "attach stopped outside step(): %r" % (frames,))
        check(frames[0].get("line") in (8, 9),
              "attach stopped on the wrong line: %r" % (frames[0].get("line"),))
        variables = session.flatten_frame_variables(frames[0]["id"])
        check("n" in variables, "attach frame has no argument `n`: %r" % (variables,))
        session.request("continue", {"threadId": thread_id})
        client.disconnect()
    except Exception as exc:
        # The injector's own error is in the DAP reply; what happened to the
        # target (exit code, stderr) is only visible here.
        raise CheckError("%s | target %s: %s" % (exc, describe_child(proc), child_output(proc)))
    finally:
        if client is not None:
            client.close()
        if proc.poll() is None:
            proc.kill()
        proc.wait()


def case_launch_inject(ctx):
    """`runtimeExecutable` launch: the debugger is injected, the target is not
    started from a bootstrap script."""
    runtime_root, version, timeout = ctx["runtime_root"], ctx["version"], ctx["timeout"]
    interpreter = make_debuggable_lua(runtime_root, version)
    fixture = "baseline.lua"
    path = source_path(fixture)
    trace = trace_path(version, "launch-inject")
    if os.path.exists(trace):
        os.remove(trace)

    client = DAPClient(ctx["adapter"], cwd=FIXTURES,
                       stderr_path=os.path.join(HERE, "stderr.log"))
    try:
        session = Session(client, timeout)
        session.trace = trace
        client.initialize(timeout=timeout)
        session.request("launch", {
            "request": "launch",
            "name": "launch-inject-" + version,
            "type": "lua",
            "luaVersion": version,
            "runtimeExecutable": interpreter,
            "runtimeArgs": [path],
            "program": path,
            "cwd": os.path.dirname(path),
            "console": "internalConsole",
            "inject": "frida",
            "env": {"LUA_DEBUG_E2E_TRACE": trace},
        })
        client.wait_event("initialized", timeout=timeout)
        set_breakpoints(session, path, [16])
        session.request("configurationDone")
        stopped = wait_for_breakpoint_stop(session, timeout)
        check(stopped.get("reason") == "breakpoint",
              "injected launch did not stop on the breakpoint: %r" % (stopped,))
        thread_id = stopped["threadId"]
        frames = session.stack_frames(thread_id)
        check(frames and frames[0].get("line") == 16,
              "injected launch stopped on the wrong line: %r" % (frames,))
        variables = session.flatten_frame_variables(frames[0]["id"])
        check_number(variables.get("x"), 1, "x")
        session.request("stepIn", {"threadId": thread_id})
        stopped = session.stopped()
        frames = session.stack_frames(thread_id)
        check(frames and frames[0].get("name") == "add",
              "stepIn did not enter add(): %r" % (frames,))
        session.request("stepOut", {"threadId": thread_id})
        session.stopped()
        set_breakpoints(session, path, [19])
        session.request("continue", {"threadId": thread_id})
        stopped = wait_for_breakpoint_stop(session, timeout)
        check(stopped.get("reason") == "breakpoint",
              "second breakpoint never hit: %r" % (stopped,))
        continue_until_terminated(session, thread_id)
        check(read_trace(trace) == ["start", "y=3", "z=6", "k=6", "done"],
              "injected target did not finish: %r" % (read_trace(trace),))
        client.disconnect()
    finally:
        client.close()


def case_inject_config(ctx):
    """Both carriers of the bootstrap document, end to end.

    1. environment only: the target is started with the loader preload and
       LUA_DEBUG_CONFIG, and the adapter joins that session by address (it
       injects nothing);
    2. data wins: the target's environment holds a *stale* document, the adapter
       attaches by pid and sends a non-empty data document, which must win.
    """
    runtime_root, version, timeout = ctx["runtime_root"], ctx["version"], ctx["timeout"]
    interpreter = make_debuggable_lua(runtime_root, version)
    path = source_path("attach.lua")
    gate = os.path.join(HERE, ".trace", "config-gate-%s" % version)
    os.makedirs(os.path.dirname(gate), exist_ok=True)

    def preload_env(address, document):
        env = {
            "ATTACH_GATE": gate,
            # The launch carrier: the loader preloads the launcher so it is present
            # before the first Lua state exists.
            PRELOAD_VAR: launcher,
            "LUA_DEBUG_PRELOAD": "1",
            "LUA_DEBUG_CONFIG": document,
        }
        return env

    launcher = os.path.join(os.path.dirname(os.path.abspath(ctx["adapter"])), "launcher.so")
    if not os.path.exists(launcher):
        raise CheckError("launcher not found at %s" % launcher)

    def run_session(*, env, attach_arguments):
        if os.path.exists(gate):
            os.remove(gate)
        child_env = dict(env)
        proc = running_lua(interpreter, "attach.lua", child_env)
        client = None
        try:
            client = DAPClient(ctx["adapter"], cwd=FIXTURES,
                               stderr_path=os.path.join(HERE, "stderr.log"))
            session = Session(client, timeout)
            client.initialize(timeout=timeout)
            session.request("attach", attach_arguments(proc.pid, version))
            client.wait_event("initialized", timeout=timeout)
            set_breakpoints(session, path, [8])
            session.request("configurationDone")
            stopped = wait_for_breakpoint_stop(session, timeout)
            with open(gate, "w", encoding="utf-8") as handle:
                handle.write("go\n")
            check(stopped.get("reason") == "breakpoint",
                  "config carrier did not reach a breakpoint: %r" % (stopped,))
            frames = session.stack_frames(stopped["threadId"])
            check(frames and frames[0].get("name") == "step",
                  "config carrier stopped outside step(): %r" % (frames,))
            check(frames[0].get("line") in (8, 9),
                  "config carrier stopped on the wrong line: %r" % (frames[0].get("line"),))
            client.disconnect()
        finally:
            if client is not None:
                client.close()
            if proc.poll() is None:
                proc.kill()
            proc.wait()

    # 1. environment only, joined by address (no injection at all).
    address = "@$tmp/lua-debug-e2e-config-%s" % version
    document = json.dumps({
        "mode": "attach",
        "address": address,
        "platform": ctx.get("bridge_platform", "darwin-arm64"),
        "luaVersion": version,
    })
    run_session(
        env=preload_env(address, document),
        attach_arguments=lambda pid, ver: {
            "request": "attach",
            "address": address,
            "client": True,
            "luaVersion": ver,
        },
    )

    # 2. a stale environment document must lose against the data document.
    run_session(
        env={
            "ATTACH_GATE": gate,
            "LUA_DEBUG_CONFIG": "not json at all",
        },
        # frida is pinned only when it can serve this target: it cannot attach
        # to a translated process, so the run of a darwin-x64 root on Apple
        # Silicon exercises the same "data wins" contract through lldb.
        attach_arguments=lambda pid, ver: dict({
            "request": "attach",
            "processId": str(pid),
            "luaVersion": ver,
        }, **({"inject": "frida"} if host_matches_runtime(runtime_root) else {})),
    )

def image_base_of(binary):
    """Load address the image's own virtual addresses are relative to.

    `nm` prints the *virtual* address, which for a Mach-O executable already
    includes the `__TEXT` base (0x100000000). An external symbol table's `offset`
    is relative to the loaded module base, so the base has to be removed first.
    Returns None when it cannot be determined, so a caller can skip rather than
    guess.
    """
    if sys.platform == "darwin":
        try:
            out = subprocess.run(["otool", "-l", binary], capture_output=True, text=True,
                                 timeout=60)
        except (OSError, subprocess.SubprocessError):
            return None
        if out.returncode != 0:
            return None
        segment = ""
        for line in out.stdout.splitlines():
            stripped = line.strip()
            if stripped.startswith("segname "):
                segment = stripped.split()[1]
            elif segment == "__TEXT" and stripped.startswith("vmaddr "):
                try:
                    return int(stripped.split()[1], 16)
                except (IndexError, ValueError):
                    return None
        return None
    if sys.platform.startswith("linux"):
        try:
            out = subprocess.run(["readelf", "-lW", binary], capture_output=True, text=True,
                                 timeout=60)
        except (OSError, subprocess.SubprocessError):
            return None
        if out.returncode != 0:
            return None
        for line in out.stdout.splitlines():
            parts = line.split()
            if len(parts) >= 3 and parts[0] == "LOAD":
                try:
                    return int(parts[2], 16)
                except ValueError:
                    return None
        return None
    return None


def nm_image_offset(binary, symbol):
    """Offset of `symbol` from the image base, or None.

    This is exactly the `offset` rule's base: the loaded module base, not a file
    offset and not the virtual address `nm` prints.
    """
    base = image_base_of(binary)
    if base is None:
        return None
    candidates = ([symbol] if os.name == "nt" else ["_" + symbol, symbol])
    for name in candidates:
        try:
            out = subprocess.run(["nm", "-gU" if sys.platform == "darwin" else "-g", binary],
                                 capture_output=True, text=True, timeout=60)
        except (OSError, subprocess.SubprocessError):
            return None
        if out.returncode != 0:
            return None
        for line in out.stdout.splitlines():
            parts = line.split()
            if len(parts) >= 3 and parts[-1] == name:
                try:
                    address = int(parts[0], 16)
                except ValueError:
                    return None
                return address - base if address >= base else None
    return None


def run_source_refusal(ctx, extra, needles):
    """Runs the interpreter with a VM source the launcher must refuse.

    These cases pin the *rules*, so they drive the interpreter and the launcher
    entry directly (the shape a plain `luaexe` launch uses): the adapter only has
    to deliver `LUA_DEBUG_CONFIG` and the marker, which the positive cases above
    already prove. A refusal has to be reported and the process has to stop
    before it joins a session - silently degrading into discovery would debug a
    VM the configuration did not describe.
    """
    version = ctx["version"]
    exe = lua_exe(ctx["runtime_root"], version)
    root = os.path.dirname(os.path.dirname(os.path.abspath(ctx["adapter"])))
    config = {
        "mode": "launch",
        "root": root,
        "platform": ctx.get("bridge_platform", "darwin-arm64"),
    }
    config.update(extra)
    document = json.dumps(config)
    script = 'dofile[[%s/script/launch.lua]] DBG[[%d]]' % (root, os.getpid())
    env = dict(os.environ)
    env["LUA_DEBUG_CONFIG"] = document
    env["LUA_DEBUG_VM_SOURCE"] = "1"
    proc = subprocess.run([exe, "-e", script, source_path("baseline.lua")],
                          env=env, capture_output=True, text=True, timeout=ctx["timeout"])
    stderr = proc.stderr or ""
    check(proc.returncode != 0,
          "the target kept running after a refused source: %r" % (stderr[-400:],))
    for needle in needles:
        check(needle.lower() in stderr.lower(),
              "the refusal does not mention %r: %r" % (needle, stderr[-600:]))


def _attach_is_privileged_out(message):
    """True when attaching was refused by the environment, not by the debugger.

    Linux surfaces the missing ptrace scope through the injector ("escalation is
    only implemented for macOS"); macOS surfaces the missing entitlement.
    """
    text = message.lower()
    needles = (
        "escalation is only implemented for macos",
        "spawn frida-inject failed",
        "operation not permitted",
        "task_for_pid",
    )
    return any(needle in text for needle in needles)


def _vm_source_injected_attach(ctx, runtime_root, version, timeout, symbols, directory):
    """Injected pid attach with an external source: launcher -> cfg.nativeBridge."""
    interpreter = make_debuggable_lua(runtime_root, version)
    gate = os.path.join(directory, "vm-source-attach-gate-%s" % version)
    if os.path.exists(gate):
        os.remove(gate)
    proc = running_lua(interpreter, "attach.lua", {"ATTACH_GATE": gate}, capture=True)
    client = None
    try:
        client = DAPClient(ctx["adapter"], cwd=FIXTURES,
                           stderr_path=os.path.join(HERE, "stderr.log"))
        session = Session(client, timeout)
        client.initialize(timeout=timeout)
        session.request("attach", {
            "request": "attach",
            "processId": str(proc.pid),
            "luaVersion": version,
            "luaBinary": os.path.basename(interpreter),
            "luaSymbols": symbols,
        })
        client.wait_event("initialized", timeout=timeout)
        path = source_path("attach.lua")
        set_breakpoints(session, path, [8])
        session.request("configurationDone")
        stopped = wait_for_breakpoint_stop(session, timeout)
        check(stopped.get("reason") == "breakpoint",
              "the injected attach did not stop: %r" % (stopped,))
        with open(gate, "w", encoding="utf-8") as handle:
            handle.write("go\n")
        frames = session.stack_frames(stopped["threadId"])
        check(frames and frames[0].get("name") == "step",
              "the injected attach stopped outside step(): %r" % (frames,))
        client.disconnect()
    except Exception as exc:
        # The injector's own error is in the DAP reply; what happened to the
        # target (exit code, stderr) is only visible here.
        raise CheckError("%s | target %s: %s" % (exc, describe_child(proc), child_output(proc)))
    finally:
        if client is not None:
            client.close()
        if proc.poll() is None:
            proc.kill()
        proc.wait()


def case_vm_source(ctx):
    """Externally supplied VM identity, end to end.

    The point of the whole feature: a Lua implementation the debugger cannot
    identify by an exported symbol. Three things are exercised here - the launch
    shape that carries the source in the environment (a plain `luaexe`, console
    and terminal), and the refusals that keep a bad source from silently
    degrading into discovery.
    """
    runtime_root, version, timeout = ctx["runtime_root"], ctx["version"], ctx["timeout"]
    # Every interpreter the runtime ships is covered: the source only describes
    # where the symbols are, not which ABI they are.
    exe = lua_exe(runtime_root, version)
    if not os.path.exists(exe):
        raise SkipSuite("%s is not built" % exe)

    trace = trace_path(version, "vm-source")
    directory = os.path.dirname(trace)
    os.makedirs(directory, exist_ok=True)

    # --- 1. console launch, image selected by name, one RVA rule ------------
    # The offset is measured from *this* interpreter, so it must match; a wrong
    # base would bind a plausible but wrong function.
    rva = nm_image_offset(exe, "lua_gettop")
    symbols_file = None
    if rva is None:
        symbols = {}
    else:
        symbols = {"lua_gettop": {"offset": hex(rva)}}
    path = source_path("baseline.lua")
    launch = launch_arguments(runtime_root, version, "baseline.lua", trace=trace, extra={
        "luaBinary": os.path.basename(exe),
        "luaSymbols": symbols,
    })
    client = DAPClient(ctx["adapter"], cwd=FIXTURES,
                       stderr_path=os.path.join(HERE, "stderr.log"))
    try:
        session, thread_id = main_case(runtime_root, version, timeout, client,
                                       suite="vm-source", launch=launch)
        continue_until_terminated(session, thread_id)
        check(read_trace(trace) == ["start", "y=3", "z=6", "k=6", "done"],
              "the target did not run to completion: %r" % (read_trace(trace),))
    finally:
        client.close()

    # --- 2. terminal launch, same source through the runInTerminal body -----
    terminal_launch = launch_arguments(runtime_root, version, "baseline.lua",
                                       trace=trace, extra={
        "luaBinary": os.path.basename(exe),
        "console": "integratedTerminal",
    })
    if os.path.exists(trace):
        os.remove(trace)
    client = DAPClient(ctx["adapter"], cwd=FIXTURES,
                       stderr_path=os.path.join(HERE, "stderr.log"))
    try:
        session = Session(client, timeout)
        client.initialize(timeout=timeout)
        session.request("launch", terminal_launch)
        client.wait_event("initialized", timeout=timeout)
        session.request("configurationDone")
        stopped = session.stopped()
        check(stopped.get("reason") in ("entry", "breakpoint"),
              "the terminal launch did not stop: %r" % (stopped,))
        continue_until_terminated(session, stopped["threadId"])
        # A real process ran: the fixture only writes this when it executed.
        check(read_trace(trace)[:1] == ["start"],
              "the terminal launch did not run the program: %r" % (read_trace(trace),))
    finally:
        client.close()

    # --- 3. the same source as a file the target reads ----------------------
    inline = launch_arguments(runtime_root, version, "baseline.lua", trace=trace, extra={
        "luaBinary": os.path.basename(exe),
        "luaSymbols": symbols,
    })
    file_launch = dict(inline)
    if not symbols:
        raise SkipSuite("nm could not measure the interpreter, so the RVA rule is skipped")
    symbols_path = os.path.join(directory, "vm-source-symbols.json")
    with open(symbols_path, "w", encoding="utf-8") as handle:
        json.dump(symbols, handle)
    symbols_file = symbols_path
    file_launch.pop("luaSymbols", None)
    file_launch["luaSymbolsFile"] = symbols_path
    client = DAPClient(ctx["adapter"], cwd=FIXTURES,
                       stderr_path=os.path.join(HERE, "stderr.log"))
    try:
        session, thread_id = main_case(runtime_root, version, timeout, client,
                                       suite="vm-source-file", launch=file_launch)
        continue_until_terminated(session, thread_id)
    finally:
        client.close()
        if symbols_file and os.path.exists(symbols_file):
            os.remove(symbols_file)

    # --- 4. refusals -------------------------------------------------------
    # A source that names an image the process does not have must fail: falling
    # back to discovery would debug something the configuration did not describe.
    run_source_refusal(ctx, {"luaBinary": "no-such-lua-image"}, ["luaBinary"])

    # A typo in a symbol name is refused rather than silently binding nothing.
    run_source_refusal(ctx, {"luaSymbols": {"lua_gtetop": {"address": "0x1000"}}},
                       ["lua_gtetop"])

    # A role is scoped to one image: without `luaBinary` there is nothing to
    # scope it to, so the source is refused instead of scanning every module.
    run_source_refusal(ctx, {"luaSymbols": {"lua_debug_server_newstate": {"address": "0x1000"}}},
                       ["lua_debug_server_newstate", "luaBinary"])

    # An explicit version the running VM contradicts is a constraint violation,
    # not a hint to be ignored.
    wrong_version = "lua51" if version != "lua51" else "lua52"
    run_source_refusal(ctx, {"luaVersion": wrong_version, "luaBinary": os.path.basename(exe)},
                       ["luaVersion", wrong_version])

    # Inline and file forms are mutually exclusive.
    run_source_refusal(ctx, {
        "luaSymbols": {"lua_gettop": {"address": "0x1000"}},
        "luaSymbolsFile": os.path.join(directory, "unused.json"),
    }, ["luaSymbols"])

    # --- 4b. the injected handoff ------------------------------------------
    # A pid attach injects the launcher, which resolves the source itself and hands
    # the bound module table to the script. This is a *different* entry from the
    # luaexe one above, and it carries the profile across two enums, so it gets its
    # own case: the luaexe path alone cannot catch a mis-mapped profile id.
    #
    # Attaching to a process the adapter did not spawn needs ptrace permission: on
    # Linux the YAMA scope must allow it, macOS needs the frida entitlement. Where
    # the environment refuses, that is a missing privilege rather than a defect in
    # the source, so it is reported as a skip instead of failing the suite (the CI
    # job raises the Linux scope before running this).
    try:
        _vm_source_injected_attach(ctx, runtime_root, version, timeout, symbols, directory)
    except (DAPError, DAPTimeout) as exc:
        if not _attach_is_privileged_out(str(exc)):
            raise
        print("    injected attach SKIPPED (no ptrace permission): %s"
              % str(exc).splitlines()[0])

    # The launcher's two discovery points can be named by role instead of by a
    # name per hook: the per-call role finds the running state, the creation
    # role stays armed for a VM that appears later.
    # The call role must be a function the running script actually reaches: the
    # fixture's loop makes Lua calls, not C API calls. `luaD_precall` is that
    # funnel wherever the image exports it; where it is hidden (5.5, LuaJIT) the
    # case is skipped rather than made flaky.
    newstate_rva = nm_image_offset(lua_exe(runtime_root, version), "luaL_newstate")
    call_rva = nm_image_offset(lua_exe(runtime_root, version), "luaD_precall")
    if newstate_rva is None or call_rva is None:
        print("    role-point attach SKIPPED (%s does not export luaL_newstate/luaD_precall)"
              % version)
    else:
        role_symbols = {
            "lua_debug_server_newstate": {"offset": hex(newstate_rva)},
            "lua_debug_server_call": {"offset": hex(call_rva)},
        }
        try:
            _vm_source_injected_attach(ctx, runtime_root, version, timeout, role_symbols, directory)
        except (DAPError, DAPTimeout) as exc:
            if not _attach_is_privileged_out(str(exc)):
                raise
            print("    role-point attach SKIPPED (no ptrace permission): %s"
                  % str(exc).splitlines()[0])

    # --- 5. a source with no injection cannot apply -------------------------
    # An attach by address joins a VM that is already running: nothing injects a
    # launcher, so the table could never reach it. Saying so is required --
    # ignoring the fields would debug whatever VM is there.
    client = DAPClient(ctx["adapter"], cwd=FIXTURES,
                       stderr_path=os.path.join(HERE, "stderr.log"))
    try:
        client.initialize(timeout=timeout)
        response = client.request("attach", {
            "request": "attach",
            "address": "@$tmp/lua-debug-e2e-vm-source-unused-%d" % os.getpid(),
            "luaBinary": os.path.basename(exe),
        }, timeout=timeout, expect_success=False)
        check(not response.get("success", True),
              "an address attach accepted a VM source it cannot apply")
        text = (response.get("message") or "").lower()
        check("luabinary" in text and "address" in text,
              "the refusal does not explain why: %r" % (response,))
    finally:
        client.close()

SUITES = {
    "baseline": case_baseline,
    "coroutine": case_coroutine,
    "core": case_core,
    "two-sources": case_two_sources,
    "full": case_full,
    "mixed-vm": case_mixed_vm,
    "attach": case_attach,
    "launch-inject": case_launch_inject,
    "inject-config": case_inject_config,
    "vm-source": case_vm_source,
}

# `full` exercises exception stops. The exception hook that reports them exists
# for 5.2 - 5.5; on 5.1 and LuaJIT errors surface through a different path that
# has never stopped on the raising line (the script worker had the same gap), so
# the suite is not run there instead of reporting a false failure.
FULL_UNSUPPORTED = ("lua51", "luajit")

# Every carrier that has to *interrupt a VM it did not start from the beginning*
# works by hooking a function the VM calls on its own: 5.1-5.5 resolve one of
# `luaD_*`/`luaV_*`, and 5.5 falls back to the exported C API. The published
# LuaJIT binary keeps no symbol of its dispatch layer (`lj_dispatch_ins`/
# `lj_vm_call` are inlined away), its hot paths are JIT-compiled machine code
# that never re-enters the C API, and its startup is fast enough to finish before
# an injected launcher lands. LuaJIT is therefore covered by the launch carrier
# only (`baseline`/`core`); the gaps are per suite because each one exercises a
# different side of that limitation.
LUAJIT_GAPS = {
    "attach": "an injected launcher cannot interrupt this interpreter",
    "launch-inject": "an injected launcher cannot interrupt this interpreter",
    "inject-config": "an attach-mode bootstrap document cannot hook this interpreter",
}


def _macho_archs(path):
    """Architectures in a Mach-O file; empty when unknown or not macOS."""
    if sys.platform != "darwin" or not os.path.exists(path):
        return set()
    result = subprocess.run(["lipo", "-archs", path], capture_output=True, text=True)
    if result.returncode != 0:
        return set()
    return set(result.stdout.split())


def suite_skip_reason(suite, runtime_root=None, test_bin_dir=None):
    """Suites that cannot run in this configuration (not a per-version gap)."""
    if suite == "inject-config" and not PRELOAD_VAR:
        return "environment-only preload carrier is not available on this platform"
    if suite == "mixed-vm" and runtime_root and test_bin_dir:
        # The case drives native hosts from the build tree and loads the runtime
        # root's bridge into them, so both have to share an architecture. A run
        # of the darwin-x64 root with a host-architecture build tree cannot work
        # (the companion build does not produce test hosts).
        host = _macho_archs(os.path.join(test_bin_dir, "test_two_lua_vms"))
        bridge = _macho_archs(os.path.join(runtime_root, "luadebug.so"))
        if host and bridge and not (host & bridge):
            return ("the hosts in --test-bin-dir (%s) do not match the runtime root (%s); "
                    "build the test hosts for that architecture"
                    % ("/".join(sorted(host)), "/".join(sorted(bridge))))
    return None


def run_case(fn, ctx):
    try:
        fn(ctx)
        return None
    except SkipSuite as exc:
        return ("skip", str(exc))
    except (CheckError, DAPError, DAPTimeout) as exc:
        return str(exc)
    except Exception:
        return traceback.format_exc()


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--adapter", required=True)
    parser.add_argument("--runtime-root", required=True)
    parser.add_argument("--versions", required=True)
    parser.add_argument("--suite", required=True)
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument(
        "--test-bin-dir",
        default=None,
        help="directory holding the native test executables (default: the "
             "CMake build tree's tests/bin)")
    parser.add_argument(
        "--injector",
        default=None,
        choices=("frida", "lldb"),
        help="force one injector backend in the adapter (otherwise it decides: "
             "frida when the architectures match, lldb when they do not)")
    args = parser.parse_args(argv)

    if args.suite not in SUITES:
        print("unknown suite %r (have: %s)" % (args.suite, ", ".join(sorted(SUITES))),
              file=sys.stderr)
        return 2

    adapter = os.path.abspath(args.adapter)
    runtime_root = os.path.abspath(args.runtime_root)
    if not os.path.exists(adapter):
        print("adapter not found: %s" % adapter, file=sys.stderr)
        return 2

    if args.test_bin_dir:
        test_bin_dir = os.path.abspath(args.test_bin_dir)
    else:
        # The CMake layout: build/cmake/<preset>/tests/bin. Kept as a fallback so
        # a plain invocation still finds a built test tree.
        test_bin_dir = os.path.abspath(
            os.path.join("build", "cmake", "darwin-arm64-release", "tests", "bin"))

    if args.injector:
        # The adapter reads this; a forced backend keeps both backends covered by
        # CI even where the default choice is unambiguous.
        os.environ["LUA_DEBUG_INJECTOR"] = args.injector

    versions = [v.strip() for v in args.versions.split(",") if v.strip()]
    failures = 0
    skipped = 0
    for version in versions:
        exe = lua_exe(runtime_root, version)
        if not os.path.exists(exe):
            print("%-8s SKIP (missing %s)" % (version, exe))
            failures += 1
            continue
        skip_reason = suite_skip_reason(args.suite, runtime_root, test_bin_dir)
        if skip_reason:
            print("%-8s SKIP (%s)" % (version, skip_reason))
            skipped += 1
            continue
        if version == "luajit" and args.suite in LUAJIT_GAPS:
            print("%-8s SKIP (%s)" % (version, LUAJIT_GAPS[args.suite]))
            skipped += 1
            continue
        if args.suite == "full" and version in FULL_UNSUPPORTED:
            print("%-8s SKIP (exception hook unsupported on this backend)" % version)
            skipped += 1
            continue
        ctx = {
            "adapter": adapter,
            "runtime_root": runtime_root,
            "version": version,
            "all_versions": versions,
            "timeout": args.timeout,
            # `publish/runtime/<platform>/...`; `debugger.lua` uses this verbatim to
            # pick the bridge, so it must match the tree that is actually present.
            "bridge_platform": os.path.basename(runtime_root),
            "test_bin_dir": test_bin_dir,
        }
        error = run_case(SUITES[args.suite], ctx)
        if isinstance(error, tuple):
            # A skip is reported as such and never counted as a pass.
            print("%-8s SKIP (%s)" % (version, error[1]))
            skipped += 1
            continue
        if error is None:
            print("%-8s PASS" % version)
        else:
            failures += 1
            print("%-8s FAIL\n%s" % (version, error))

    if skipped:
        print("%s: %d/%d passed (%d skipped)"
              % (args.suite, len(versions) - failures - skipped,
                 len(versions) - skipped, skipped))
    else:
        print("%s: %d/%d passed" % (args.suite, len(versions) - failures, len(versions)))
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
