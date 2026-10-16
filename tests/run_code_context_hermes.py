#!/usr/bin/env python3
"""One fresh Hermes task with its existing subscription route and one guarded tool."""

import argparse
from copy import deepcopy
import os
from pathlib import Path
import signal
import sys
import time

from code_context_workflow import CliBackend, Workflow, TOOL_NAME, MAX_CALLS
from code_context_pair import BaselineBackend, GUIDE, MODEL, PROVIDER, EFFORT, CONTEXT_LENGTH, tool_schema, audit_messages, check_answer, answer_guide
from run_agent_query_pilot import write
from shin_semantic.artifact import decode, encode, require, sha


class PilotStop(BaseException):
    """Do not let the host's provider retry/fallback handlers retry a guard failure."""


def agent_type(base, guide=GUIDE):
    class GuardedAgent(base):
        def _build_system_prompt(self, system_message=None):
            return guide

        def _build_api_kwargs(self, messages, tools_for_api=None):
            try:
                require(self.flow.remaining_seconds() > 0, "pilot-task-deadline")
                require(self.model == MODEL and self.provider == PROVIDER and self.api_mode == "codex_responses",
                        "pilot-provider-mismatch")
                require(self.tools == self.pilot_tools and (tools_for_api is None or tools_for_api == self.pilot_tools),
                        "pilot-tools-changed")
                # Keep host context injection and result compaction out of the measured transcript.
                messages = deepcopy(self.pilot_messages)
                audit_messages(messages, self.flow, self.question, guide)
                final_only = self.flow.stopped is not None
                if final_only:
                    require(self.flow.can_finalize() and not getattr(self, "final_request_sent", False),
                            "pilot-finalization-not-allowed")
                    self.final_request_sent = True
                result = super()._build_api_kwargs(messages, tools_for_api=[] if final_only else tools_for_api)
                if final_only:
                    require(not result.get("tools"), "pilot-finalization-tools-present")
                delivered = [row["output"] for row in result.get("input", []) if row.get("type") == "function_call_output"]
                require(delivered == self.flow.trace, "pilot-wire-delivery-mismatch")
                self.request_audits.append({"tool_responses": len(delivered), "tool_bytes": sum(len(v.encode()) for v in delivered),
                                            "model": self.model, "provider": self.provider})
                return result
            except Exception as error:
                raise PilotStop(str(error) if type(error).__name__ == "Rejected" else "pilot-request-audit-failed") from None

        def _execute_tool_calls(self, assistant_message, messages, effective_task_id, api_call_count=0):
            if self.flow.stopped:
                raise PilotStop("pilot-agent-called-after-stop")
            if not 0 < len(assistant_message.tool_calls) <= MAX_CALLS:
                raise PilotStop("pilot-tool-batch-budget")
            self.pilot_messages.append({"role": "assistant", "content": assistant_message.content,
                "tool_calls": [{"id": call.id, "type": "function", "function": {
                    "name": call.function.name, "arguments": call.function.arguments}}
                    for call in assistant_message.tool_calls]})
            for call in assistant_message.tool_calls:
                try:
                    args = decode(call.function.arguments.encode())
                except Exception:
                    args = {}
                if self.flow.stopped:
                    if not self.flow.can_finalize():
                        raise PilotStop("pilot-agent-called-after-stop")
                    payload = self.flow.cancel_queued()
                else:
                    payload = self.flow.invoke(call.function.name, args)
                self.requests.append({"name": call.function.name, "arguments": args})
                message = {"role": "tool", "tool_call_id": call.id, "name": call.function.name, "content": payload.decode()}
                messages.append(message)
                self.pilot_messages.append(deepcopy(message))
                write(self.output / "progress.json", self.flow.receipt())

    return GuardedAgent


def run(task, output):
    started = time.monotonic()
    contract = task.get("answer_contract", "legacy-v1")
    guide = answer_guide(contract)
    sys.dont_write_bytecode = True
    os.umask(0o077)
    output.mkdir(mode=0o700, parents=True, exist_ok=False)
    # Credentials stay inside Hermes memory. Never copy/store a key or choose another provider.
    from hermes_cli.runtime_provider import resolve_runtime_provider
    resolved = resolve_runtime_provider(requested=PROVIDER, target_model=MODEL)
    require(resolved["provider"] == PROVIDER and resolved["api_mode"] == "codex_responses", "pilot-provider-mismatch")
    home = output / "hermes-home"
    home.mkdir(mode=0o700)
    os.environ["HERMES_HOME"] = str(home)
    for key in ("HERMES_CONFIG", "HERMES_CONFIG_PATH", "HERMES_ENV", "HERMES_ENV_PATH", "HERMES_PROFILE",
                "HERMES_SESSION_ID", "HERMES_DUMP_REQUESTS", "HERMES_ENABLE_PROJECT_PLUGINS"):
        os.environ.pop(key, None)
    write(home / "config.yaml", {"model": {"default": MODEL, "provider": PROVIDER, "context_length": CONTEXT_LENGTH},
        "compression": {"enabled": False}, "agent": {"background_review": {"enabled": False}}, "plugins": {}})
    os.chdir(home)
    from run_agent import AIAgent
    agent = agent_type(AIAgent, guide)(provider=PROVIDER, api_mode="codex_responses", model=MODEL,
        api_key=resolved["api_key"], base_url=resolved["base_url"], requested_provider=PROVIDER,
        enabled_toolsets=[], disabled_toolsets=None, max_iterations=16, max_tokens=12000,
        reasoning_config={"enabled": True, "effort": EFFORT}, run_budget_seconds=180,
        skip_context_files=True, skip_memory=True, skip_background_review=True, load_soul_identity=False,
        quiet_mode=True, save_trajectories=False, fallback_model=None, cwd=str(home))
    agent._persist_disabled = True
    agent.tools = agent.pilot_tools = [{"type": "function", "function": tool_schema(task["variant"])}]
    agent.valid_tool_names = {TOOL_NAME}
    require(agent.context_compressor.context_length == CONTEXT_LENGTH, "pilot-context-limit-mismatch")
    backend_type = BaselineBackend if task["variant"] == "baseline" else CliBackend
    scratch = output / "requests"
    scratch.mkdir(mode=0o700)
    kwargs = {"safe_sources": task["sources"]} if task["variant"] == "baseline" else {}
    backend = backend_type(Path(task["route"]), Path(task["python"]), scratch, **kwargs)
    agent.flow = Workflow(backend, set(task["sources"]))
    agent.flow.started = started
    agent.question, agent.output, agent.requests, agent.request_audits = task["question"], output, [], []
    agent.pilot_messages = [{"role": "system", "content": guide}, {"role": "user", "content": task["question"]}]
    result = answer = None
    answer_check = None
    failure = None
    setup_seconds = time.monotonic() - started

    def expired(*_):
        raise PilotStop("pilot-task-deadline")

    old = signal.signal(signal.SIGALRM, expired)
    signal.setitimer(signal.ITIMER_REAL, max(0.001, agent.flow.remaining_seconds()))
    try:
        result = agent.run_conversation(task["question"])
        require(result.get("completed") is True and not result.get("error"), "pilot-model-turn-incomplete")
        answer = decode(result["final_response"].encode())
        answer_check = check_answer(answer, agent.flow.trace, contract)
    except PilotStop as error:
        failure = str(error)
    except Exception as error:
        failure = str(error) if type(error).__name__ == "Rejected" else "pilot-model-result-rejected"
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        signal.signal(signal.SIGALRM, old)
        agent.flow.close()
        receipt = agent.flow.receipt()
        usage_fields = ("input_tokens", "output_tokens", "reasoning_tokens", "cache_read_tokens", "cache_write_tokens",
                        "total_tokens", "last_prompt_tokens", "api_calls")
        usage = {key: result.get(key) for key in usage_fields} if result else None
        receipt.update(model=MODEL, provider=PROVIDER, effort=EFFORT, context_limit=CONTEXT_LENGTH,
                       answer=answer, failure=failure, completed=result.get("completed") if result else False,
                       provider_usage=usage, usage_source="hermes-result-not-derived-from-response-bytes",
                       served_model=result.get("served_model") if result else None,
                       model_result_keys=sorted(result) if result else [], setup_seconds=round(setup_seconds, 6),
                       forwarded_tool_bytes=max((row["tool_bytes"] for row in agent.request_audits), default=0),
                       elapsed_seconds=round(time.monotonic() - started, 6), request_audits=agent.request_audits,
                       fresh_model_session=True, variant=task["variant"])
        receipt.update(answer_contract=contract, guide_sha256=sha(guide.encode()), answer_check=answer_check)
        write(output / "receipt.json", receipt)
        write(output / "trace.json", {"requests": agent.requests, "responses": agent.flow.trace,
            "final_response": result.get("final_response") if result else None})
        agent.close()
    print(encode({"ok": failure is None and receipt["stop_reason"] is None, "failure": failure,
                  "calls": receipt["calls"], "bytes": receipt["delivered_bytes"], "output": str(output)}).decode())
    return 0 if failure is None and receipt["stop_reason"] is None else 1


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--task", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        raise SystemExit(run(decode(args.task.read_bytes()), args.output.resolve()))
    except Exception as error:
        print(encode({"ok": False, "error": str(error) if type(error).__name__ == "Rejected"
                      else "pilot-host-start-rejected", "class": type(error).__name__}).decode(), file=sys.stderr)
        raise SystemExit(1)
