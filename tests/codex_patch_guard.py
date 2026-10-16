#!/usr/bin/env python3
"""Test-only Codex lifecycle boundary; state is outside the model's workspace."""
from contextlib import contextmanager, ExitStack, nullcontext
import fcntl
import hashlib
import json
import os
from pathlib import Path
import selectors
import shlex
import signal
import subprocess
import sys
import time

MAX_CALLS = 12
MAX_BYTES = 65536
PER_CALL_BYTES = 8192


@contextmanager
def locked_state(path, write=True):
    with path.with_suffix(".lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        state = json.loads(path.read_text())
        yield state
        if write:
            path.write_text(json.dumps(state))


def initialize(path, root, seconds=180, mcp=False, initial_bytes=0, comparison=None):
    if type(initial_bytes) is not int or not 0 <= initial_bytes <= PER_CALL_BYTES:
        raise ValueError("pilot-initial-context-budget")
    with path.open("x") as output:
        json.dump({"root": str(root.resolve(strict=True)), "started": time.monotonic(), "seconds": seconds,
                   "calls": {}, "outputs": {}, "children": [], "bytes": initial_bytes,
                   "initial_bytes": initial_bytes, "stopped": None, "events": [],
                   "mcp": mcp, "comparison": comparison}, output)
    path.chmod(0o600)


def isolated_command(root, command, read_only=False):
    policy = '(version 1)(deny default)(allow process*)(allow sysctl-read)(allow mach-lookup)(allow file-read-metadata)'
    policy += '(allow file-read* (require-all (require-not (subpath "/Users"))'
    policy += '(require-not (subpath "/private/tmp"))(require-not (subpath "/private/var"))))'
    policy += '(allow file-read* (subpath ' + json.dumps(str(root)) + '))'
    policy += '(allow file-write* ' + ('' if read_only else '(subpath ' + json.dumps(str(root)) + ')') + '(literal "/dev/null"))'
    args = ["/usr/bin/sandbox-exec", "-p", policy, "/usr/bin/env", "-i",
            "PATH=/opt/homebrew/bin:/usr/bin:/bin", "LC_ALL=C", "PYTHONDONTWRITEBYTECODE=1",
            "HOME=" + str(root), "TMPDIR=" + str(root), "/bin/bash", "--noprofile", "--norc", "-c", command]
    return "cd " + shlex.quote(str(root)) + " && exec " + shlex.join(args)


def bounded_text(value, limit):
    raw = value.encode()
    if len(raw) <= limit:
        return value, False
    marker = "\n[pilot output clipped; read a narrower range]\n"
    if limit < len(marker.encode()):
        return "", True
    return raw[:limit - len(marker.encode())].decode(errors="ignore") + marker, True


def execute(path, call):
    # Serialize mixed shell/MCP execution so concurrent outputs cannot overspend.
    with path.with_suffix(".execution-lock").open("a") as lock, ExitStack() as resources:
        fcntl.flock(lock, fcntl.LOCK_EX)
        return execute_admitted(path, call, resources)


def execute_admitted(path, call, resources):
    with locked_state(path) as state:
        entry = state["calls"][call]
        if entry.get("started") or state["stopped"]:
            raise ValueError("pilot-execution-reused-or-stopped")
        if state["bytes"] >= MAX_BYTES or time.monotonic() - state["started"] >= state["seconds"]:
            state["stopped"] = "pilot-budget-exhausted"
            raise ValueError("pilot-budget-exhausted")
        entry["started"] = True
        root = Path(state["root"])
        deadline = min(state["started"] + state["seconds"], time.monotonic() + 30)
        source_context = nullcontext(
            ["/bin/bash", "--noprofile", "--norc", "-c", isolated_command(root, entry["command"], state.get("comparison") is not None)])
        if entry.get("comparison_search"):
            source_context = nullcontext([sys.executable, '-B', str(path.parent / 'pilot_comparison_query.py'),
                str(path), *entry['comparison_search'], str(min(PER_CALL_BYTES, MAX_BYTES - state['bytes']))])
        args = resources.enter_context(source_context)
        child = subprocess.Popen(args,
            cwd=root, env={"PATH": "/opt/homebrew/bin:/usr/bin:/bin", "HOME": str(root), "TMPDIR": str(root)},
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, start_new_session=True)
        state["children"].append(child.pid)
    prefix = bytearray()
    total = 0
    failure = None
    try:
        with selectors.DefaultSelector() as selector:
            selector.register(child.stdout, selectors.EVENT_READ)
            while selector.get_map():
                if time.monotonic() >= deadline:
                    failure = "pilot-command-deadline"
                    break
                for key, _ in selector.select(timeout=min(0.05, max(0, deadline - time.monotonic()))):
                    block = os.read(key.fileobj.fileno(), 16384)
                    if not block:
                        selector.unregister(key.fileobj)
                        continue
                    total += len(block)
                    prefix.extend(block[:max(0, 32768 - len(prefix))])
                    if total > 1048576:
                        failure = "pilot-command-output-cap"
                        break
                if failure:
                    break
        if failure is None:
            try:
                child.wait(timeout=max(0.01, deadline - time.monotonic()))
            except subprocess.TimeoutExpired:
                failure = "pilot-command-deadline"
    finally:
        if child.poll() is None:
            try:
                os.killpg(child.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        child.wait()
        child.stdout.close()
    with locked_state(path) as state:
        state["children"].remove(child.pid)
        text = prefix.decode(errors="replace")
        if failure:
            state["stopped"] = failure
            text += "\n[" + failure + "]\n"
        limit = min(PER_CALL_BYTES, MAX_BYTES - state["bytes"])
        if native and len(text.encode()) > limit:
            state["stopped"] = "pilot-source-output-budget"
            text = '{"ok":false,"error":"pilot-source-output-budget"}\n'
        delivered, clipped = bounded_text(text, limit)
        size = len(delivered.encode())
        state["bytes"] += size
        state["outputs"][call] = {"bytes": size, "source_bytes": total, "clipped": clipped or total > len(prefix),
            "sha256": hashlib.sha256(delivered.encode()).hexdigest(), "exit_code": child.returncode}
    # No partial output escapes before accounting, including unified-exec yields.
    sys.stdout.write(delivered)
    return child.returncode if child.returncode >= 0 else 1


def dispatch(path, event):
    kind = event.get("hook_event_name")
    with locked_state(path) as state:
        root = Path(state["root"])
        call = event.get("tool_use_id")
        tool = event.get("tool_name")
        if kind in {"PreToolUse", "PostToolUse"} and isinstance(tool, str) and tool.startswith("mcp__context_search__"):
            from pilot_tools import validate_tool
            try:
                if not state.get("mcp"):
                    raise ValueError("pilot-mcp-not-enabled")
                validate_tool(state, tool.split('__', 2)[2], event.get("tool_input", {}))
            except ValueError:
                state["stopped"] = "pilot-invalid-mcp-tool"
                return {"continue": False, "stopReason": state["stopped"], "hookSpecificOutput": {
                    "hookEventName": kind, "permissionDecision": "deny", "permissionDecisionReason": state["stopped"]}}
            # The trusted MCP server admits/accounts execution through this same
            # guard. Do not count a call twice or permit arbitrary MCP servers.
            state["events"].append({"event": kind, "tool": tool, "id": call})
            return {}
        if kind == "SessionStart":
            state["events"].append({"event": "session-start"})
            return {}
        if kind == "PreToolUse":
            command = event.get("tool_input", {}).get("command")
            reason = None
            comparison_search = None
            if state["stopped"] or time.monotonic() - state["started"] >= state["seconds"]:
                reason = "pilot-deadline-or-stopped"
            elif len(state["calls"]) >= MAX_CALLS or state["bytes"] >= MAX_BYTES:
                reason = "pilot-budget-exhausted"
            elif not isinstance(call, str) or call in state["calls"]:
                reason = "pilot-invalid-call-identity"
            elif tool != "Bash" or not isinstance(command, str) or len(command.encode()) > 32768:
                reason = "pilot-only-isolated-shell"
            else:
                try:
                    if command.startswith('comparison-search '):
                        words = shlex.split(command)
                        from pilot_tools import validate_tool
                        if len(words) != 3:
                            raise ValueError('comparison-command')
                        validate_tool(state, words[1] + '_search', {'query': words[2]})
                        comparison_search = words[1:]
                except ValueError:
                    reason = "pilot-invalid-comparison-command"
            if reason:
                state["stopped"] = reason
                state["events"].append({"event": "denied", "tool": tool, "reason": reason})
                return {"hookSpecificOutput": {"hookEventName": kind, "permissionDecision": "deny",
                                               "permissionDecisionReason": reason}}
            state["calls"][call] = {"tool": tool, "command": command}
            if comparison_search:
                state['calls'][call]['comparison_search'] = comparison_search
            state["events"].append({"event": "admitted", "id": call, "tool": tool})
            result = {"hookEventName": kind, "permissionDecision": "allow"}
            result["updatedInput"] = {"command": shlex.join([sys.executable, "-B", str(Path(__file__).resolve()),
                                                            str(path), "execute", call])}
            return {"hookSpecificOutput": result}
        if kind == "PostToolUse":
            if call not in state["calls"] or call not in state["outputs"]:
                state["stopped"] = "pilot-unadmitted-output"
                return {"continue": False, "stopReason": "pilot-unadmitted-output"}
            state["outputs"][call]["post_seen"] = True
            return {}
        state["stopped"] = "pilot-unexpected-hook"
        return {"decision": "block", "reason": "pilot-unexpected-hook"}


if __name__ == "__main__":
    try:
        if len(sys.argv) == 4 and sys.argv[2] == "execute":
            raise SystemExit(execute(Path(sys.argv[1]), sys.argv[3]))
        print(json.dumps(dispatch(Path(sys.argv[1]), json.load(sys.stdin))))
    except Exception:
        # Command-hook exit 2 blocks the call instead of continuing on bad state.
        print("pilot-hook-state-invalid", file=sys.stderr)
        raise SystemExit(2)
