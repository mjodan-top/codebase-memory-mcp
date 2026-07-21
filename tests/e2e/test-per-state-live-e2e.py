#!/usr/bin/env python3
"""Issue #28 first live per-state runner.

Uses the production binary in all three roles:
  * daemon owning the store/watcher/HTTP server
  * two default-mode stdio -> UDS shims
  * HTTP /api/index and /api/index-status

No fixed sleeps are used for readiness or state transitions: every wait is a
bounded condition poll with diagnostics on timeout.
"""
from __future__ import annotations

import argparse
import json
import os
import selectors
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any, Callable

STATES = (
    "R_EMPTY",
    "R_PROJECT_OPEN",
    "R_REUSED",
    "R_WATCHING",
    "R_INDEX_IN_FLIGHT",
    "R_UI_SERVING",
    "R_SESSION_CLOSED",
    "R_DAEMON_RESTARTED",
)


def fail(message: str) -> None:
    raise RuntimeError(message)


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def poll(label: str, timeout: float, probe: Callable[[], Any]) -> Any:
    deadline = time.monotonic() + timeout
    last: Any = None
    while time.monotonic() < deadline:
        try:
            value = probe()
            if value:
                return value
            last = value
        except Exception as exc:  # diagnostics retained for the timeout
            last = f"{type(exc).__name__}: {exc}"
        time.sleep(0.05)
    fail(f"timeout waiting for {label}; last={last!r}")


def http_json(port: int, path: str, payload: dict[str, Any] | None = None) -> Any:
    data = None if payload is None else json.dumps(payload).encode()
    req = urllib.request.Request(
        f"http://127.0.0.1:{port}{path}",
        data=data,
        headers={"Content-Type": "application/json"},
        method="GET" if data is None else "POST",
    )
    with urllib.request.urlopen(req, timeout=1.0) as response:
        body = response.read()
    return json.loads(body.decode())


def http_rpc(port: int, request_id: int, method: str, params: dict[str, Any]) -> Any:
    response = http_json(
        port,
        "/rpc",
        {"jsonrpc": "2.0", "id": request_id, "method": method, "params": params},
    )
    if (
        not isinstance(response, dict)
        or response.get("jsonrpc") != "2.0"
        or response.get("id") != request_id
    ):
        fail(f"HTTP /rpc returned unexpected response: {response!r}")
    if "error" in response:
        fail(f"HTTP /rpc JSON-RPC error: {response['error']!r}")
    if "result" not in response:
        fail(f"HTTP /rpc response has no result: {response!r}")
    return response["result"]


def index_status(port: int) -> list[dict[str, Any]]:
    value = http_json(port, "/api/index-status")
    if not isinstance(value, list):
        fail(f"/api/index-status returned non-list JSON: {value!r}")
    return value


def status_for(port: int, project: str) -> dict[str, Any] | None:
    for item in index_status(port):
        if item.get("project") == project or item.get("path", "").endswith("/" + project):
            return item
    return None


def file_tail(path: Path, lines: int = 80) -> str:
    try:
        return "\n".join(path.read_text(errors="replace").splitlines()[-lines:])
    except OSError as exc:
        return f"<unavailable: {exc}>"


class Shim:
    def __init__(
        self, binary: Path, socket_path: Path, env: dict[str, str], daemon_log_path: Path
    ) -> None:
        self.proc = subprocess.Popen(
            [str(binary), "--socket", str(socket_path)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            bufsize=0,
            env=env,
        )
        self.next_id = 1
        self.stdout_buffer = b""
        self.stderr_tail = b""
        self.daemon_log_path = daemon_log_path

    def diagnostics(self) -> str:
        stderr = self.stderr_tail[-32768:].decode(errors="replace")
        return (
            f"shim_rc={self.proc.poll()}\n"
            f"--- shim stderr tail ---\n{stderr}\n"
            f"--- daemon.log tail ---\n{file_tail(self.daemon_log_path)}"
        )

    def request(self, method: str, params: dict[str, Any], timeout: float = 10.0) -> Any:
        if self.proc.stdin is None or self.proc.stdout is None:
            fail("shim pipes unavailable")
        request_id = self.next_id
        self.next_id += 1
        wire = {"jsonrpc": "2.0", "id": request_id, "method": method, "params": params}
        self.proc.stdin.write((json.dumps(wire, separators=(",", ":")) + "\n").encode())
        self.proc.stdin.flush()
        deadline = time.monotonic() + timeout
        with selectors.DefaultSelector() as selector:
            selector.register(self.proc.stdout, selectors.EVENT_READ, "stdout")
            if self.proc.stderr is not None:
                selector.register(self.proc.stderr, selectors.EVENT_READ, "stderr")
            while True:
                while b"\n" in self.stdout_buffer:
                    line, self.stdout_buffer = self.stdout_buffer.split(b"\n", 1)
                    if not line:
                        continue
                    response = json.loads(line.decode())
                    if response.get("id") != request_id:
                        fail(f"unexpected JSON-RPC response id: {response!r}")
                    if "error" in response:
                        fail(f"JSON-RPC error: {response['error']!r}")
                    return response.get("result")

                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    fail(
                        f"timeout waiting {timeout:.1f}s for shim {method} id={request_id}\n"
                        + self.diagnostics()
                    )
                events = selector.select(remaining)
                if not events:
                    fail(
                        f"timeout waiting {timeout:.1f}s for shim {method} id={request_id}\n"
                        + self.diagnostics()
                    )
                for key, _ in events:
                    chunk = os.read(key.fileobj.fileno(), 65536)
                    if key.data == "stderr":
                        if chunk:
                            self.stderr_tail = (self.stderr_tail + chunk)[-32768:]
                        else:
                            selector.unregister(key.fileobj)
                    elif chunk:
                        self.stdout_buffer += chunk
                    else:
                        selector.unregister(key.fileobj)
                        if self.proc.poll() is not None:
                            fail(f"shim exited before response\n{self.diagnostics()}")

    def initialize(self) -> None:
        self.request(
            "initialize",
            {
                "protocolVersion": "2025-06-18",
                "capabilities": {},
                "clientInfo": {"name": "per-state-live-e2e", "version": "1"},
            },
        )
        if self.proc.stdin is None:
            fail("shim stdin unavailable")
        self.proc.stdin.write(
            (json.dumps(
                {"jsonrpc": "2.0", "method": "notifications/initialized"},
                separators=(",", ":"),
            )
            + "\n").encode()
        )
        self.proc.stdin.flush()

    def tool(self, name: str, arguments: dict[str, Any]) -> Any:
        return self.request("tools/call", {"name": name, "arguments": arguments})

    def close(self) -> None:
        if self.proc.stdin:
            self.proc.stdin.close()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)
        if self.proc.returncode != 0:
            err = self.proc.stderr.read().decode(errors="replace") if self.proc.stderr else ""
            fail(f"shim close rc={self.proc.returncode}: {err}")


def make_repo(root: Path, name: str, files: int) -> Path:
    repo = root / name
    repo.mkdir()
    subprocess.run(["git", "init", "-q", str(repo)], check=True)
    subprocess.run(["git", "-C", str(repo), "config", "user.email", "e2e@example.invalid"], check=True)
    subprocess.run(["git", "-C", str(repo), "config", "user.name", "Issue 28 E2E"], check=True)
    for i in range(files):
        (repo / f"unit_{i:04d}.c").write_text(
            f"int issue28_symbol_{i:04d}(void) {{ return {i}; }}\n", encoding="utf-8"
        )
    subprocess.run(["git", "-C", str(repo), "add", "."], check=True)
    subprocess.run(["git", "-C", str(repo), "commit", "-qm", "fixture"], check=True)
    return repo


def make_inflight_repo(root: Path) -> Path:
    repo = root / "issue28-inflight-repo"
    repo.mkdir()
    subprocess.run(["git", "init", "-q", str(repo)], check=True)
    (repo / "good.py").write_text("def good():\n    return 1\n", encoding="utf-8")
    (repo / "hang_me.py").write_text("def slow():\n    return 2\n", encoding="utf-8")
    return repo


def write_ui_config(cache_dir: Path, port: int) -> None:
    cache_dir.mkdir(parents=True, exist_ok=True)
    (cache_dir / "config.json").write_text(
        json.dumps({"ui_enabled": True, "ui_port": port}), encoding="utf-8"
    )


def start_daemon(
    binary: Path, socket_path: Path, env: dict[str, str], log_path: Path
) -> tuple[subprocess.Popen[str], Any]:
    log = log_path.open("a", encoding="utf-8")
    proc = subprocess.Popen(
        [str(binary), "daemon", "--socket", str(socket_path)],
        stdin=subprocess.DEVNULL,
        stdout=log,
        stderr=subprocess.STDOUT,
        text=True,
        env=env,
    )
    return proc, log


def daemon_http_ready(proc: subprocess.Popen[str], port: int, log_path: Path) -> Any:
    rc = proc.poll()
    if rc is not None:
        fail(f"daemon exited before HTTP readiness rc={rc}\n{file_tail(log_path)}")
    return {"status": index_status(port)}


def stop_daemon(proc: subprocess.Popen[str], log: Any) -> None:
    if proc.poll() is None:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)
    log.close()
    if proc.returncode != 0:
        fail(f"daemon exited rc={proc.returncode}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", default="build/codebase-memory-mcp")
    parser.add_argument("--timeout", type=float, default=45.0)
    parser.add_argument("--fixture-files", type=int, default=1200)
    args = parser.parse_args()

    binary = Path(args.bin).resolve()
    if not binary.is_file():
        fail(f"production binary not found: {binary}")

    tmp = Path(tempfile.mkdtemp(prefix="cbm-per-state-live-"))
    daemon: subprocess.Popen[str] | None = None
    daemon_log: Any = None
    shims: list[Shim] = []
    passed: list[str] = []
    try:
        cache_dir = tmp / "cache"
        socket_path = tmp / "daemon.sock"
        log_path = tmp / "daemon.log"
        port = free_port()
        write_ui_config(cache_dir, port)
        env = os.environ.copy()
        env["CBM_CACHE_DIR"] = str(cache_dir)
        env["CBM_LOG_LEVEL"] = "debug"

        repo = make_repo(tmp, "issue28-live-repo", args.fixture_files)
        project = repo.name
        inflight_repo = make_inflight_repo(tmp)
        inflight_marker = tmp / "inflight.marker"
        env["CBM_TEST_HANG_ON"] = "hang_me.py"
        env["CBM_INDEX_SINGLE_THREAD"] = "1"
        env["CBM_INDEX_MARKER_FILE"] = str(inflight_marker)

        daemon, daemon_log = start_daemon(binary, socket_path, env, log_path)
        poll("daemon UDS listener", args.timeout, lambda: socket_path.exists() or None)
        poll(
            "R_UI_SERVING",
            args.timeout,
            lambda: daemon_http_ready(daemon, port, log_path),
        )
        rpc_init = http_rpc(
            port,
            9001,
            "initialize",
            {
                "protocolVersion": "2025-06-18",
                "capabilities": {},
                "clientInfo": {"name": "per-state-live-http", "version": "1"},
            },
        )
        if not isinstance(rpc_init, dict) or "capabilities" not in rpc_init:
            fail(f"HTTP /rpc initialize returned invalid result: {rpc_init!r}")
        if daemon.poll() is not None:
            fail(f"daemon exited after HTTP /rpc initialize rc={daemon.returncode}")
        passed.append("R_UI_SERVING")

        empty = index_status(port)
        if empty:
            fail(f"R_EMPTY expected [], got {empty!r}")
        passed.append("R_EMPTY")

        shim1 = Shim(binary, socket_path, env, log_path)
        shims.append(shim1)
        shim1.initialize()

        http_json(port, "/api/index", {"root_path": str(repo), "project_name": project})
        poll("R_PROJECT_OPEN", args.timeout, lambda: status_for(port, project))
        passed.append("R_PROJECT_OPEN")

        poll(
            "index completion",
            args.timeout,
            lambda: (item if (item := status_for(port, project)) and item.get("status") == "done" else None),
        )

        shim2 = Shim(binary, socket_path, env, log_path)
        shims.append(shim2)
        shim2.initialize()
        result = shim2.tool("index_status", {"project": project})
        if project not in json.dumps(result):
            fail(f"R_REUSED second shim did not resolve indexed project: {result!r}")
        passed.append("R_REUSED")

        watched = repo / "unit_0000.c"
        watched.write_text(watched.read_text(encoding="utf-8") + "int issue28_watch_event(void) { return 28; }\n", encoding="utf-8")
        # The watcher is daemon-owned. Poll through the public HTTP endpoint
        # until it remains responsive after the real filesystem event, then
        # verify the new symbol through the second independent UDS session.
        poll("R_WATCHING HTTP liveness", args.timeout, lambda: index_status(port) is not None)
        poll(
            "R_WATCHING symbol visible",
            args.timeout,
            lambda: (
                value
                if "issue28_watch_event" in json.dumps(
                    value := shim2.tool(
                        "search_graph", {"project": project, "name_pattern": "issue28_watch_event"}
                    )
                )
                else None
            ),
        )
        passed.append("R_WATCHING")

        shim1.close()
        shims.remove(shim1)
        if status_for(port, project) is None:
            fail("R_SESSION_CLOSED lost daemon-owned project after first shim exit")
        passed.append("R_SESSION_CLOSED")

        inflight_start = http_json(
            port,
            "/api/index",
            {"root_path": str(inflight_repo), "project_name": inflight_repo.name},
        )
        if not isinstance(inflight_start, dict) or inflight_start.get("status") != "indexing":
            fail(f"in-flight index did not start: {inflight_start!r}")
        poll(
            "in-flight worker marker",
            args.timeout,
            lambda: (
                inflight_marker
                if inflight_marker.exists()
                and "hang_me.py" in inflight_marker.read_text(errors="replace")
                else None
            ),
        )
        poll(
            "R_INDEX_IN_FLIGHT",
            args.timeout,
            lambda: (
                item
                if (item := status_for(port, inflight_repo.name))
                and item.get("status") == "indexing"
                else None
            ),
        )
        passed.append("R_INDEX_IN_FLIGHT")

        shim2.close()
        shims.remove(shim2)
        stop_daemon(daemon, daemon_log)
        daemon = None
        daemon_log = None

        if socket_path.exists():
            socket_path.unlink()
        daemon, daemon_log = start_daemon(binary, socket_path, env, log_path)
        poll("restarted daemon UDS listener", args.timeout, lambda: socket_path.exists() or None)
        poll("restarted HTTP server", args.timeout, lambda: index_status(port) is not None)
        shim3 = Shim(binary, socket_path, env, log_path)
        shims.append(shim3)
        shim3.initialize()
        result = shim3.tool("index_status", {"project": project})
        if project not in json.dumps(result):
            fail(f"R_DAEMON_RESTARTED did not reopen persisted project: {result!r}")
        passed.append("R_DAEMON_RESTARTED")

        if set(passed) != set(STATES):
            fail(f"state coverage mismatch passed={passed!r}")
        print("PASS per-state live E2E: " + " ".join(STATES))
        return 0
    except Exception as exc:
        print(f"FAIL per-state live E2E: {exc}", file=sys.stderr)
        log_path = tmp / "daemon.log"
        if log_path.exists():
            print("--- daemon.log tail ---", file=sys.stderr)
            print("\n".join(log_path.read_text(errors="replace").splitlines()[-120:]), file=sys.stderr)
        return 1
    finally:
        for shim in shims:
            try:
                shim.close()
            except Exception:
                pass
        if daemon is not None and daemon_log is not None:
            try:
                stop_daemon(daemon, daemon_log)
            except Exception:
                pass
        if os.environ.get("CBM_KEEP_E2E_TMP") == "1":
            print(f"kept fixture: {tmp}", file=sys.stderr)
        else:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
