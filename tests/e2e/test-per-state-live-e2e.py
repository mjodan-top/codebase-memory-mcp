#!/usr/bin/env python3
"""Issue #28 first live per-state runner.

Uses the production binary in all three roles:
  * daemon owning the store/watcher/HTTP server
  * two default-mode stdio -> UDS shims
  * HTTP /api/index and /api/index-status

No fixed sleeps are used for readiness or state transitions: every wait is a
bounded condition poll with diagnostics on timeout.

Issue #29 additions (both off by default; the default path is unchanged):
  * --activation launchd: instead of Popen-ing the daemon, install a throwaway
    LaunchAgent plist (shape copied from tests/e2e/test_launchd_activation.sh)
    into gui/$UID. launchd binds the UDS and the FIRST real shim connection
    spawns the daemon; R_DAEMON_RESTARTED becomes SIGKILL + launchd respawn on
    the next connection (the socket stays launchd-owned, never unlinked here).
    macOS only; elsewhere the mode SKIPs explicitly (exit 0).
  * --soak N: after the full state pass, N rounds of 2 concurrent shims
    (initialize + tools/call index_status + close) with per-round evidence and
    leak assertions (daemon pid set, fd count, RSS) against the first-round
    baseline.
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

    def initialize(self, timeout: float = 10.0) -> None:
        self.request(
            "initialize",
            {
                "protocolVersion": "2025-06-18",
                "capabilities": {},
                "clientInfo": {"name": "per-state-live-e2e", "version": "1"},
            },
            timeout=timeout,
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


LAUNCHD_ENV_KEYS = (
    "CBM_CACHE_DIR",
    "CBM_LOG_LEVEL",
    "CBM_TEST_HANG_ON",
    "CBM_INDEX_SINGLE_THREAD",
    "CBM_INDEX_MARKER_FILE",
)


def write_launchd_plist(
    plist_path: Path,
    label: str,
    binary: Path,
    socket_path: Path,
    log_path: Path,
    env: dict[str, str],
    home: Path,
) -> None:
    # Shape copied from tests/e2e/test_launchd_activation.sh: throwaway label,
    # `daemon --launchd --socket`, launchd-owned <Sockets> listener (0600),
    # temp HOME. Test-only CBM_* knobs ride in EnvironmentVariables because in
    # this mode launchd (not this runner) is the daemon's parent.
    env_entries = "".join(
        f"        <key>{key}</key><string>{env[key]}</string>\n"
        for key in LAUNCHD_ENV_KEYS
        if key in env
    )
    plist_path.write_text(
        f"""<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
 "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key><string>{label}</string>
    <key>ProgramArguments</key>
    <array>
        <string>{binary}</string>
        <string>daemon</string>
        <string>--launchd</string>
        <string>--socket</string>
        <string>{socket_path}</string>
    </array>
    <key>EnvironmentVariables</key>
    <dict>
        <key>HOME</key><string>{home}</string>
{env_entries}    </dict>
    <key>Sockets</key>
    <dict>
        <key>Listeners</key>
        <dict>
            <key>SockPathName</key><string>{socket_path}</string>
            <key>SockPathMode</key><integer>384</integer>
        </dict>
    </dict>
    <key>StandardOutPath</key><string>{log_path}</string>
    <key>StandardErrorPath</key><string>{log_path}</string>
</dict>
</plist>
""",
        encoding="utf-8",
    )


def launchd_bootout(label: str) -> None:
    # Cleanup is part of the contract: a failed bootout is a test failure,
    # never swallowed.
    target = f"gui/{os.getuid()}/{label}"
    proc = subprocess.run(["launchctl", "bootout", target], capture_output=True, text=True)
    if proc.returncode != 0:
        fail(f"launchctl bootout {target} rc={proc.returncode}: {proc.stderr.strip()}")


def daemon_pids(pattern: str) -> list[int]:
    proc = subprocess.run(["pgrep", "-f", "--", pattern], capture_output=True, text=True)
    return [int(line) for line in proc.stdout.split()]


def fd_count(pid: int) -> int:
    proc_fd = Path(f"/proc/{pid}/fd")
    if proc_fd.is_dir():
        return len(list(proc_fd.iterdir()))
    proc = subprocess.run(["lsof", "-p", str(pid)], capture_output=True, text=True)
    lines = [line for line in proc.stdout.splitlines() if line.strip()]
    if len(lines) < 2:
        fail(f"lsof -p {pid} returned no fd table rc={proc.returncode}: {proc.stderr.strip()}")
    return len(lines) - 1  # minus the lsof header row


def rss_kb(pid: int) -> int:
    proc = subprocess.run(["ps", "-o", "rss=", "-p", str(pid)], capture_output=True, text=True)
    value = proc.stdout.strip()
    if proc.returncode != 0 or not value:
        fail(f"ps -o rss= -p {pid} failed rc={proc.returncode}")
    return int(value)


def run_soak(
    rounds: int,
    binary: Path,
    socket_path: Path,
    env: dict[str, str],
    log_path: Path,
    project: str,
    pattern: str,
    timeout: float,
    shims: list[Shim],
) -> None:
    pids = daemon_pids(pattern)
    if len(pids) != 1:
        fail(f"soak baseline expected exactly 1 daemon for {pattern!r}, got {pids!r}")
    base_pid = pids[0]
    base_fds: int | None = None
    base_rss: int | None = None
    for round_no in range(1, rounds + 1):
        pair = []
        for _ in range(2):
            shim = Shim(binary, socket_path, env, log_path)
            shims.append(shim)
            pair.append(shim)
        for shim in pair:
            shim.initialize()
        for shim in pair:
            result = shim.tool("index_status", {"project": project})
            if project not in json.dumps(result):
                fail(f"soak round {round_no} index_status lost project: {result!r}")
        for shim in pair:
            shim.close()
            shims.remove(shim)

        pids = daemon_pids(pattern)
        if pids != [base_pid]:
            fail(
                f"soak round {round_no}: daemon process set changed to {pids!r}"
                f" (baseline single pid {base_pid})"
            )
        if base_fds is None:
            base_fds = fds = fd_count(base_pid)
            base_rss = rss = rss_kb(base_pid)
        else:
            fd_limit = base_fds + 8
            fds = poll(
                f"soak round {round_no} daemon fds <= {fd_limit}",
                timeout,
                lambda: (value if (value := fd_count(base_pid)) <= fd_limit else None),
            )
            rss = rss_kb(base_pid)
            if rss > 2 * base_rss:
                fail(f"soak round {round_no}: rss {rss} KiB > 2x baseline {base_rss} KiB")
        print(
            f"[soak] round={round_no}/{rounds} pids={len(pids)} fds={fds} rss_kb={rss}",
            flush=True,
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", default="build/codebase-memory-mcp")
    parser.add_argument("--timeout", type=float, default=45.0)
    parser.add_argument("--fixture-files", type=int, default=1200)
    parser.add_argument("--activation", choices=("launchd",), default=None)
    parser.add_argument("--soak", type=int, default=0)
    args = parser.parse_args()

    if args.activation == "launchd" and sys.platform != "darwin":
        print("SKIP per-state live E2E --activation launchd: launchd only exists on macOS")
        return 0

    binary = Path(args.bin).resolve()
    if not binary.is_file():
        fail(f"production binary not found: {binary}")

    tmp = Path(tempfile.mkdtemp(prefix="cbm-per-state-live-"))
    daemon: subprocess.Popen[str] | None = None
    daemon_log: Any = None
    shims: list[Shim] = []
    passed: list[str] = []
    bootstrap_label: str | None = None
    rc = 1
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

        pgrep_pattern = f"{binary} daemon --socket {socket_path}"
        shim1: Shim | None = None
        if args.activation == "launchd":
            pgrep_pattern = f"{binary} daemon --launchd --socket {socket_path}"
            home = tmp / "home"
            # In activation mode the daemon reads config from the temp HOME's
            # cache dir; the HTTP UI must be enabled there for the R_UI_SERVING
            # / /api assertions to stay unchanged.
            write_ui_config(home / ".cache" / "codebase-memory-mcp", port)
            label = f"dev.codebase-memory.perstate-e2e.{os.getpid()}"
            plist_path = tmp / f"{label}.plist"
            write_launchd_plist(plist_path, label, binary, socket_path, log_path, env, home)
            bootstrap = subprocess.run(
                ["launchctl", "bootstrap", f"gui/{os.getuid()}", str(plist_path)],
                capture_output=True,
                text=True,
            )
            if bootstrap.returncode != 0:
                fail(
                    f"launchctl bootstrap rc={bootstrap.returncode}:"
                    f" {bootstrap.stderr.strip()}"
                )
            bootstrap_label = label
            poll(
                "launchd-bound UDS listener",
                args.timeout,
                lambda: socket_path.exists() or None,
            )
            if daemon_pids(pgrep_pattern):
                fail("daemon already running before first connection (not on-demand)")
            # The FIRST real shim connection is what makes launchd spawn the
            # daemon: no manual Popen in this mode.
            shim1 = Shim(binary, socket_path, env, log_path)
            shims.append(shim1)
            shim1.initialize(timeout=args.timeout)
            poll(
                "R_UI_SERVING (launchd-activated)",
                args.timeout,
                lambda: index_status(port) is not None,
            )
        else:
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
        if daemon is not None and daemon.poll() is not None:
            fail(f"daemon exited after HTTP /rpc initialize rc={daemon.returncode}")
        passed.append("R_UI_SERVING")

        empty = index_status(port)
        if empty:
            fail(f"R_EMPTY expected [], got {empty!r}")
        passed.append("R_EMPTY")

        if shim1 is None:
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
        if args.activation == "launchd":
            # Launchd owns the socket: SIGKILL the daemon and let the NEXT real
            # connection re-activate it. No manual unlink/restart here.
            pids = daemon_pids(pgrep_pattern)
            if len(pids) != 1:
                fail(f"expected exactly 1 launchd-spawned daemon, got {pids!r}")
            killed_pid = pids[0]
            os.kill(killed_pid, signal.SIGKILL)
            poll(
                "SIGKILLed daemon gone",
                args.timeout,
                lambda: (True if killed_pid not in daemon_pids(pgrep_pattern) else None),
            )
            shim3 = Shim(binary, socket_path, env, log_path)
            shims.append(shim3)
            shim3.initialize(timeout=args.timeout)
            respawned = poll(
                "launchd respawned daemon",
                args.timeout,
                lambda: daemon_pids(pgrep_pattern) or None,
            )
            if len(respawned) != 1 or respawned[0] == killed_pid:
                fail(f"launchd respawn expected 1 new pid, got {respawned!r} (old {killed_pid})")
            poll(
                "restarted HTTP server (launchd)",
                args.timeout,
                lambda: index_status(port) is not None,
            )
        else:
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

        if args.soak > 0:
            run_soak(
                args.soak,
                binary,
                socket_path,
                env,
                log_path,
                project,
                pgrep_pattern,
                args.timeout,
                shims,
            )
            print(f"PASS soak: {args.soak} rounds (pids/fds/rss within bounds)")
        rc = 0
    except Exception as exc:
        print(f"FAIL per-state live E2E: {exc}", file=sys.stderr)
        log_path = tmp / "daemon.log"
        if log_path.exists():
            print("--- daemon.log tail ---", file=sys.stderr)
            print("\n".join(log_path.read_text(errors="replace").splitlines()[-120:]), file=sys.stderr)
        rc = 1
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
        if bootstrap_label is not None:
            # bootout failures must surface, not be swallowed by cleanup.
            try:
                launchd_bootout(bootstrap_label)
            except Exception as exc:
                print(f"FAIL launchd cleanup: {exc}", file=sys.stderr)
                rc = 1
        if os.environ.get("CBM_KEEP_E2E_TMP") == "1":
            print(f"kept fixture: {tmp}", file=sys.stderr)
        else:
            shutil.rmtree(tmp, ignore_errors=True)
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
