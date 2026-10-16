#!/usr/bin/env python3
"""Search or collect bounded task context through existing read-only Shin routes."""

import argparse
from contextlib import nullcontext
import importlib.util
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True


def load_semantic():
    path = Path(__file__).resolve().with_name("shin_semantic.py")
    spec = importlib.util.spec_from_file_location("shin_semantic_entry", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def load_adapter():
    directory = (Path(__file__).resolve().parents[1] / "integrations" / "hermes"
                 / "plugins" / "shin-context")
    spec = importlib.util.spec_from_file_location(
        "shin_context_adapter", directory / "__init__.py",
        submodule_search_locations=[str(directory)],
    )
    if spec is None or spec.loader is None:
        raise RuntimeError("The source-owned Shin adapter is missing")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module.adapter


def collect_task(args):
    semantic = load_semantic()
    from shin_semantic.artifact import canonical, decode, read
    try:
        path = args.task.expanduser().absolute()
        path = path.parent.resolve() / path.name
        task = decode(read(canonical(str(path)), 128 * 1024))
        adapter = load_adapter()
        # Share the existing public client budget; native Core owns task semantics.
        adapter.task_request(task)
        if args.config is not None:
            result = semantic.Session(args.config.expanduser().absolute()).pack(task)
            output = semantic.encode(result).decode()
        else:
            request = route_request(args)
            request.update(task_request=task, jobs=1)
            output = adapter.pack_context(request)
            result = json.loads(output)
    except semantic.Rejected as error:
        result = {"schema_version": "shin.agent-pack.v1", "ok": False, "error": {"code": str(error)}}
        output = semantic.encode(result).decode()
    except Exception:
        result = {"schema_version": "shin.agent-pack.v1", "ok": False, "error": {"code": "agent-pack-rejected"}}
        output = semantic.encode(result).decode()
    print(output)
    return 0 if result["ok"] else 1


def route_request(args):
    request = {"root": str((args.root or Path.cwd()).expanduser().resolve())}
    if args.out_dir is not None:
        request["out_dir"] = str(args.out_dir.expanduser().resolve())
    if args.marker is not None:
        request["marker"] = args.marker
    return request


def inspect_file(args):
    semantic = load_semantic()
    try:
        result = semantic.Session(args.config.expanduser().absolute()).inspect(args.inspect)
    except semantic.Rejected as error:
        result = {"schema_version": "shin.agent-inspect.v1", "ok": False, "error": {"code": str(error)}}
    except Exception:
        result = {"schema_version": "shin.agent-inspect.v1", "ok": False, "error": {"code": "agent-inspect-rejected"}}
    print(semantic.encode(result).decode())
    return 0 if result["ok"] else 1


def read_source(args):
    semantic = load_semantic()
    from shin_semantic.artifact import canonical, decode, read
    from shin_semantic.compact import number_read
    try:
        path = args.read.expanduser().absolute()
        path = path.parent.resolve() / path.name
        request = decode(read(canonical(str(path)), 128 * 1024))
        result = semantic.Session(args.config.expanduser().absolute()).read(request)
        if args.projection == "compact":
            result = number_read(result)
    except semantic.Rejected as error:
        result = {"schema_version": "shin.agent-read.v1", "ok": False, "error": {"code": str(error)}}
    except Exception:
        result = {"schema_version": "shin.agent-read.v1", "ok": False, "error": {"code": "agent-read-rejected"}}
    print(semantic.encode(result).decode())
    return 0 if result["ok"] else 1


def main(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__,
        epilog="No indexing, repair, profile switching or installation. Advanced operations remain in shin-context.",
    )
    parser.add_argument("query", nargs="?", help="one quoted question or exact identifier")
    parser.add_argument("--task", type=Path, help="collect an explicit shin.task-request.v1 using the same project route")
    parser.add_argument("--inspect", help="list indexed ranges of one literal relative path; native development --config only")
    parser.add_argument("--read", type=Path, help="read published record IDs (v1) or files (v2); native development --config only")
    parser.add_argument("--root", type=Path, help="project root (default: current directory)")
    parser.add_argument("--status", action="store_true", help="check readiness without retrieval")
    parser.add_argument("--projection", choices=("compact", "audit"), default="compact", help="agent view or original audit envelope")
    parser.add_argument("--config", type=Path, help="explicit private semantic development route; no installed route discovery")
    parser.add_argument("--metrics-dir", type=Path, help="opt-in initialized private local journal; requires --config")
    route = parser.add_argument_group("explicit route overrides (only when needed)")
    route.add_argument("--out-dir", type=Path, help="reviewed external publication directory")
    route.add_argument("--marker", help="authoritative repository-relative adoption marker")
    args = parser.parse_args(argv)
    if sum((args.status, args.query is not None, args.task is not None, args.inspect is not None, args.read is not None)) != 1:
        parser.error("provide exactly one question, --task, --inspect, --read or --status")
    if args.inspect is not None and (args.config is None or args.metrics_dir is not None or args.projection != "compact"):
        parser.error("--inspect requires a native development --config; query metrics and audit projection do not apply")
    if args.read is not None and (args.config is None or args.metrics_dir is not None):
        parser.error("--read requires a native development --config; query metrics do not apply")
    if args.task is not None and (args.metrics_dir is not None or args.projection != "compact"):
        parser.error("--task retains native compact Pack; query metrics and audit projection do not apply")
    if args.metrics_dir is not None and args.config is None:
        parser.error("--metrics-dir requires an explicit semantic development --config")
    if args.config is not None:
        if args.status or args.root is not None or args.out_dir is not None or args.marker is not None:
            parser.error("--config cannot be combined with --status or installed route overrides")
    if args.task is not None:
        return collect_task(args)
    if args.inspect is not None:
        return inspect_file(args)
    if args.read is not None:
        return read_source(args)
    if args.config is not None:
        semantic = load_semantic()
        from shin_semantic.compact import condense_query, from_semantic
        from shin_semantic.telemetry import Trace, save
        trace = Trace() if args.metrics_dir is not None else None
        phase = trace.phase if trace is not None else lambda _: nullcontext()
        session = raw = None
        try:
            with phase("route-admission"):
                session = semantic.Session(args.config.expanduser().absolute())
            raw = result = session.query(args.query, trace=trace)
            with phase("projection"):
                if args.projection == "compact":
                    result = condense_query(from_semantic(result))
        except semantic.Rejected as error:
            result = {"schema_version": "shin.agent-query.v1", "ok": False, "error": {"code": str(error)}}
        except Exception:
            result = {"schema_version": "shin.agent-query.v1", "ok": False, "error": {"code": "agent-query-rejected"}}
        output = semantic.encode(result)
        if trace is not None:
            warning = save(args.metrics_dir.expanduser().absolute(), trace, session, raw, result, len(output) + 1, args.projection)
            if warning is not None:
                print(semantic.encode(warning).decode(), file=sys.stderr)
        print(output.decode())
        return 0 if result["ok"] else 1
    request = route_request(args)
    adapter = load_adapter()
    if args.status:
        output = adapter.ready_context(request)
    else:
        request["query"] = args.query
        output = adapter.brief_context(request)
    # Failed admission and status retain the original diagnostic envelope.
    if not args.status and args.projection == "compact" and json.loads(output).get("ok") is True:
        semantic = load_semantic()
        from shin_semantic.compact import condense_query, from_adapter
        try:
            output = semantic.encode(condense_query(from_adapter(json.loads(output)))).decode()
        except semantic.Rejected as error:
            output = semantic.encode({"schema_version": "shin.agent-query.v1", "ok": False, "error": {"code": str(error)}}).decode()
        except Exception:
            output = semantic.encode({"schema_version": "shin.agent-query.v1", "ok": False, "error": {"code": "agent-query-rejected"}}).decode()
    print(output)
    return 0 if json.loads(output).get("ok") is True else 1


if __name__ == "__main__":
    raise SystemExit(main())
