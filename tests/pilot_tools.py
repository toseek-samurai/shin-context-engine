"""Typed pilot-only tool admission, sharing the shell guard's budgets."""
import io
import shlex
import uuid

SEARCH_DESCRIPTION = ('Search this frozen project for code relevant to a natural-language question or identifier. '
                      'Returns bounded source excerpts with file/line references; omissions are explicit. '
                      'Optional: ordinary shell search is also available.')
from contextlib import redirect_stdout

def validate_tool(state, name, arguments):
    if not isinstance(arguments, dict):
        raise ValueError("pilot-tool-arguments")
    if name in {'shin_search', 'jbcontext_search'}:
        provider = name.removesuffix('_search')
        query = arguments.get('query')
        if (provider not in (state.get('comparison') or {}).get('providers', []) or set(arguments) != {'query'}
                or not isinstance(query, str) or not query.strip() or len(query.encode()) > 4096):
            raise ValueError('comparison-tool-unavailable-or-invalid')
        return shlex.join(['comparison-search', provider, query])
    raise ValueError("pilot-tool-unavailable")


def call_tool(path, name, arguments):
    from codex_patch_guard import dispatch, execute, locked_state
    with locked_state(path, write=False) as state:
        command = validate_tool(state, name, arguments)
    call = "mcp-" + uuid.uuid4().hex
    admitted = dispatch(path, {"hook_event_name": "PreToolUse", "tool_name": "Bash",
        "tool_use_id": call, "tool_input": {"command": command}})["hookSpecificOutput"]
    if admitted["permissionDecision"] != "allow":
        raise ValueError(admitted["permissionDecisionReason"])
    with locked_state(path) as state:
        state["calls"][call]["mcp_tool"] = name
        state["calls"][call]["mcp_arguments"] = arguments
    output = io.StringIO()
    with redirect_stdout(output):
        status = execute(path, call)
    dispatch(path, {"hook_event_name": "PostToolUse", "tool_name": "Bash", "tool_use_id": call})
    return output.getvalue(), status != 0
