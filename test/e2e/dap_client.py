"""Minimal Debug Adapter Protocol client (stdlib only).

Speaks Content-Length framed JSON over the adapter's stdio, answers reverse
requests (runInTerminal / startDebugging), and kills the adapter it started on
teardown or timeout. On timeout the raised DAPTimeout carries the last request
sent and every event received so failures are diagnosable without a dump file.
"""

import json
import os
import queue
import signal
import subprocess
import threading
import time


class DAPError(Exception):
    pass


class DAPTimeout(DAPError):
    def __init__(self, what, last_request, events, stderr_tail):
        self.what = what
        self.last_request = last_request
        self.events = events
        self.stderr_tail = stderr_tail
        super().__init__(self._render())

    def _render(self):
        lines = ["timeout waiting for %s" % self.what]
        if self.last_request is not None:
            lines.append("last request: %s" % _brief(self.last_request))
        lines.append("events received (%d):" % len(self.events))
        for ev in self.events[-20:]:
            lines.append("  - %s" % _brief(ev))
        if self.stderr_tail:
            lines.append("adapter stderr tail:")
            for ln in self.stderr_tail.splitlines()[-20:]:
                lines.append("  | " + ln)
        return "\n".join(lines)


def _brief(msg, limit=400):
    try:
        text = json.dumps(msg, ensure_ascii=False)
    except (TypeError, ValueError):
        text = repr(msg)
    if len(text) > limit:
        text = text[:limit] + "..."
    return text


class DAPClient:
    def __init__(self, adapter, cwd=None, env=None, extra_args=None,
                 stderr_path=None):
        self.adapter = adapter
        self.stderr_path = stderr_path
        self._seq = 0
        self._write_lock = threading.Lock()
        self._lock = threading.Lock()
        self._terminals = []
        self._last_terminal = None
        self._responses = {}
        self._resp_cv = threading.Condition()
        self._events = []
        self._event_cv = threading.Condition()
        self._pending_events = []
        self._stderr_lines = []
        self._closed = False
        self._last_request = None

        argv = [adapter]
        if extra_args:
            argv.extend(extra_args)
        # Own session/process group: the teardown kill must never take the
        # test runner (or the caller's shell) down with the adapter.
        spawn_kwargs = {}
        if os.name == "posix":
            spawn_kwargs["start_new_session"] = True
        else:
            spawn_kwargs["creationflags"] = getattr(
                subprocess, "CREATE_NEW_PROCESS_GROUP", 0)

        # The adapter's stdio DAP path uses bee's subprocess.peek(), which is a
        # MSG_PEEK recv() and therefore requires socketpair-backed stdio (what
        # libuv hands a spawn()'d child on POSIX, and what VS Code provides).
        # A plain pipe makes peek() fail with ENOTSOCK and the adapter never
        # reads a byte.
        self._sock = None
        if os.name == "posix":
            import socket as _socket
            client_sock, child_sock = _socket.socketpair()
            stdin_arg = child_sock.fileno()
            stdout_arg = child_sock.fileno()
        else:
            stdin_arg = subprocess.PIPE
            stdout_arg = subprocess.PIPE

        self.proc = subprocess.Popen(
            argv,
            stdin=stdin_arg,
            stdout=stdout_arg,
            stderr=subprocess.PIPE,
            cwd=cwd,
            env=env,
            bufsize=0,
            **spawn_kwargs
        )
        if os.name == "posix":
            child_sock.close()
            self._sock = client_sock
            self._reader_stream = client_sock.makefile("rb")
        else:
            self._reader_stream = self.proc.stdout

        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()
        self._err_reader = threading.Thread(target=self._stderr_loop, daemon=True)
        self._err_reader.start()

    # ---------------------------------------------------------------- transport

    def _stderr_loop(self):
        for raw in iter(self.proc.stderr.readline, b""):
            line = raw.decode("utf-8", "replace").rstrip("\n")
            self._stderr_lines.append(line)
            if len(self._stderr_lines) > 500:
                del self._stderr_lines[:100]

    def stderr_tail(self):
        return "\n".join(self._stderr_lines[-40:])

    def _read_loop(self):
        stream = self._reader_stream
        while True:
            headers = {}
            while True:
                line = stream.readline()
                if not line:
                    self._mark_closed()
                    return
                line = line.decode("ascii", "replace").strip()
                if not line:
                    break
                if ":" in line:
                    k, v = line.split(":", 1)
                    headers[k.strip().lower()] = v.strip()
            try:
                length = int(headers.get("content-length", "0"))
            except ValueError:
                self._mark_closed()
                return
            body = b""
            while len(body) < length:
                chunk = stream.read(length - len(body))
                if not chunk:
                    self._mark_closed()
                    return
                body += chunk
            try:
                message = json.loads(body.decode("utf-8"))
            except ValueError as exc:
                with open("/tmp/parseerr.log", "ab") as fh:
                    fh.write(b"ERR %r HEAD %r TAIL %r\n" % (str(exc).encode(), body[:200], body[-120:]))
                self._mark_closed()
                return
            self._dispatch(message)

    def _mark_closed(self):
        with self._resp_cv:
            self._closed = True
            self._resp_cv.notify_all()
        with self._event_cv:
            self._event_cv.notify_all()

    def _dispatch(self, message):
        kind = message.get("type")
        if kind == "response":
            with self._resp_cv:
                self._responses[message.get("request_seq")] = message
                self._resp_cv.notify_all()
        elif kind == "event":
            self.events.append(message)
            with self._event_cv:
                self._event_cv.notify_all()
        elif kind == "request":
            # Reverse request from the adapter to the client.
            command = message.get("command")
            if command == "runInTerminal":
                # A terminal launch is only proven by actually starting the
                # process: answering success without spawning would leave the
                # adapter waiting for a target that never exists, and would let a
                # broken argv/cwd/env go unnoticed.
                self._run_in_terminal(message)
            elif command == "startDebugging":
                self._write({
                    "seq": self._next_seq(),
                    "type": "response",
                    "request_seq": message.get("seq"),
                    "command": command,
                    "success": True,
                    "body": {},
                })
            else:
                self._write({
                    "seq": self._next_seq(),
                    "type": "response",
                    "request_seq": message.get("seq"),
                    "command": command,
                    "success": False,
                    "message": "unsupported reverse request " + str(command),
                })

    def _run_in_terminal(self, message):
        """Spawns the `runInTerminal` body as this client's child.

        The body carries `args`, `cwd` and an `env` *override* map (a null value
        deletes the variable), the same semantics the adapter documents. The
        child runs in its own process group so teardown kills the whole target,
        exactly like a real terminal.
        """
        request_seq = message.get("seq")
        body = message.get("arguments") or {}
        argv = body.get("args") or []
        if not isinstance(argv, list) or not argv:
            self._write({
                "seq": self._next_seq(),
                "type": "response",
                "request_seq": request_seq,
                "command": "runInTerminal",
                "success": False,
                "message": "runInTerminal without args",
            })
            return
        env = dict(os.environ)
        for key, value in (body.get("env") or {}).items():
            if isinstance(value, str):
                env[str(key)] = value
            elif key in env:
                del env[key]
        stderr_path = None
        stderr_handle = None
        try:
            directory = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".trace")
            os.makedirs(directory, exist_ok=True)
            stderr_path = os.path.join(directory, "terminal-%d.err" % os.getpid())
            stderr_handle = open(stderr_path, "wb")
            proc = subprocess.Popen(
                [str(item) for item in argv],
                cwd=body.get("cwd") or None,
                env=env,
                start_new_session=True,
                stdout=subprocess.DEVNULL,
                stderr=stderr_handle,
            )
        except OSError as exc:
            self._write({
                "seq": self._next_seq(),
                "type": "response",
                "request_seq": request_seq,
                "command": "runInTerminal",
                "success": False,
                "message": "cannot start %r: %s" % (argv[0], exc),
            })
            return
        if stderr_handle is not None:
            stderr_handle.close()
        with self._lock:
            self._terminals.append(proc)
            self._last_terminal = (proc, stderr_path)
        self._write({
            "seq": self._next_seq(),
            "type": "response",
            "request_seq": request_seq,
            "command": "runInTerminal",
            "success": True,
            "body": {"processId": proc.pid},
        })

    def last_terminal(self):
        """(process, stderr path) of the most recent `runInTerminal` target."""
        with self._lock:
            return self._last_terminal

    def _next_seq(self):
        self._seq += 1
        return self._seq

    def _write(self, message):
        data = json.dumps(message, ensure_ascii=False).encode("utf-8")
        header = ("Content-Length: %d\r\n\r\n" % len(data)).encode("ascii")
        with self._write_lock:
            try:
                if self._sock is not None:
                    self._sock.sendall(header + data)
                else:
                    self.proc.stdin.write(header + data)
                    self.proc.stdin.flush()
            except (BrokenPipeError, ValueError, OSError):
                self._mark_closed()

    # ------------------------------------------------------------------- API

    @property
    def events(self):
        return self._events

    def send_request(self, command, arguments=None):
        message = {
            "seq": self._next_seq(),
            "type": "request",
            "command": command,
        }
        if arguments is not None:
            message["arguments"] = arguments
        self._last_request = message
        self._write(message)
        return message["seq"]

    def request(self, command, arguments=None, timeout=10.0, expect_success=True):
        seq = self.send_request(command, arguments)
        response = self.wait_response(seq, timeout)
        if expect_success and not response.get("success"):
            raise DAPError("request %s failed: %s" % (
                command, response.get("message", "")))
        return response

    def wait_response(self, seq, timeout=10.0):
        deadline = time.time() + timeout
        with self._resp_cv:
            while seq not in self._responses:
                if self._closed:
                    raise DAPTimeout("response to seq %r (adapter closed)" % seq,
                                     self._last_request, self._events,
                                     self.stderr_tail())
                remaining = deadline - time.time()
                if remaining <= 0:
                    raise DAPTimeout(
                        "response to seq %r" % seq,
                        self._last_request, self._events, self.stderr_tail())
                self._resp_cv.wait(remaining)
            return self._responses.pop(seq)

    def wait_event(self, names, timeout=10.0, predicate=None, consume=True):
        if isinstance(names, str):
            names = (names,)
        names = tuple(names)
        deadline = time.time() + timeout
        with self._event_cv:
            while True:
                for ev in self._events:
                    if ev.get("event") in names and (predicate is None or predicate(ev)):
                        if consume:
                            self._events.remove(ev)
                        return ev
                remaining = deadline - time.time()
                if remaining <= 0:
                    raise DAPTimeout(
                        "event %s" % (names,),
                        self._last_request, self._events, self.stderr_tail())
                if self._closed:
                    raise DAPTimeout("event %s (adapter closed)" % (names,),
                                     self._last_request, self._events,
                                     self.stderr_tail())
                self._event_cv.wait(remaining)

    def event_seen(self, name):
        return any(ev.get("event") == name for ev in self._events)

    def mark_events_seen(self):
        with self._event_cv:
            del self._events[:]

    # ------------------------------------------------------------- lifecycle

    def initialize(self, timeout=15.0, wait_initialized=False, **capabilities):
        args = {
            "clientID": "lua-debug-e2e",
            "clientName": "lua-debug e2e harness",
            "adapterID": "lua-debug",
            "pathFormat": "path",
            "linesStartAt1": True,
            "columnsStartAt1": True,
            "supportsRunInTerminalRequest": True,
            "supportsVariableType": True,
            "supportsVariablePaging": True,
        }
        args.update(capabilities)
        response = self.request("initialize", args, timeout=timeout)
        # The adapter answers `initialize` from the frontend proxy, while the
        # `initialized` event is only emitted once the backend (master) is
        # connected, i.e. after `launch`/`attach`. VS Code has the same order.
        if wait_initialized:
            self.wait_event("initialized", timeout=timeout)
        return response

    def disconnect(self, terminate_debuggee=False, timeout=10.0):
        try:
            self.request("disconnect",
                         {"terminateDebuggee": terminate_debuggee},
                         timeout=timeout, expect_success=False)
        except DAPError:
            pass

    def close(self, grace=3.0):
        with self._lock:
            terminals = list(self._terminals)
            self._terminals = []
        for proc in terminals:
            if proc.poll() is not None:
                continue
            try:
                if os.name == "posix":
                    os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
                else:
                    proc.kill()
            except Exception:
                try:
                    proc.kill()
                except Exception:
                    pass
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                pass
        try:
            if self.proc.poll() is None:
                try:
                    if self._sock is not None:
                        self._sock.shutdown(1)
                    else:
                        self.proc.stdin.close()
                except Exception:
                    pass
                try:
                    self.proc.wait(timeout=grace)
                except subprocess.TimeoutExpired:
                    self._kill()
            self._mark_closed()
        finally:
            for stream in (self._reader_stream, self.proc.stderr, self._sock):
                try:
                    stream.close()
                except Exception:
                    pass

    def _kill(self):
        proc = self.proc
        if proc.poll() is not None:
            return
        try:
            if os.name == "posix":
                os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
            else:
                proc.kill()
        except Exception:
            try:
                proc.kill()
            except Exception:
                pass
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            pass

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False
