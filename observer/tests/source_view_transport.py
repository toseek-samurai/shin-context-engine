#!/usr/bin/env python3
"""Check native view bytes through OpenCode's local file service, without inference."""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

from source_query_test import decode_source_view


def run(observer, prepared):
    freeze = json.loads((prepared / "private/freeze.json").read_text())
    rows = []
    with tempfile.TemporaryDirectory(prefix="shin-view-client-") as temporary:
        home = Path(temporary).resolve()
        env = {"PATH": "/opt/homebrew/bin:/usr/bin:/bin", "HOME": str(home), "LC_ALL": "C",
               "XDG_CONFIG_HOME": str(home / "config"), "XDG_DATA_HOME": str(home / "data"),
               "XDG_CACHE_HOME": str(home / "cache"), "XDG_STATE_HOME": str(home / "state"),
               "OPENCODE_CONFIG_CONTENT": json.dumps({"plugin": [], "mcp": {}, "instructions": [],
                                                      "autoupdate": False, "share": "disabled"})}
        for case in ("jsonl", "media"):
            native = json.loads((prepared / "private" / case / "context-audit.json").read_text())
            inline = freeze["cases"][case].get("context_delivery") == "verified-inline-prompt-v1"
            if inline:
                for client_name in ("luna", "opencode"):
                    folder = prepared / "inputs" / (client_name + "-" + case + "-shin")
                    prompt = (folder / "prompt.txt").read_text()
                    delivered = prompt.split("[begin_source_context]\n", 1)[1].rsplit("[end_source_context]\n", 1)[0]
                    assert delivered.encode() == (folder / "context.txt").read_bytes()
                    decode_source_view(delivered.encode())
            for budget in (8192, 32768):
                command = [str(observer), "source-context", "--config", str(prepared / "private" / case / "observer.json"),
                           "--query", freeze["cases"][case]["symptom"], "--projection", "compact", "--max-bytes", str(budget)]
                process = subprocess.run(command, capture_output=True, check=True, timeout=30,
                                         env={"PATH": "", "LC_ALL": "C"})
                raw = process.stdout
                header, metadata, bodies, receipt = decode_source_view(raw)
                assert header["snapshot_id"] == native["snapshot_id"]
                assert len(metadata) == len(native["context"]["bodies"])
                for number, text in bodies.items():
                    assert text == native["context"]["bodies"][number - 1]["text"].encode()
                if budget == 32768:
                    assert len(bodies) == len(native["context"]["bodies"])
                path = home / (case + str(budget) + ".txt")
                path.write_bytes(raw)
                client = subprocess.run(["/opt/homebrew/bin/opencode", "debug", "file", "read", str(path), "--pure"],
                                        cwd=home, env=env, capture_output=True, check=True, timeout=30)
                result = json.loads(client.stdout)
                assert result["encoding"] == "base64", "Unexpected file-service encoding"
                delivered = base64.b64decode(result["content"], validate=True)
                assert delivered == raw
                decode_source_view(delivered)
                rows.append({"case": case, "budget": budget, "bytes": len(raw),
                    "max_line_bytes": max(len(line) for line in raw.split(b"\n")),
                    "bodies_shown": len(bodies), "bodies_total": len(metadata),
                    "omitted": receipt["omitted_for_view_budget"], "sha256": hashlib.sha256(raw).hexdigest(),
                    "file_service_exact": True, "inline_prompt_exact": inline})
    return {"schema_version": "shin.source-view-transport-check.v1", "ok": True, "model_runs": 0,
            "scope": "native rendering and OpenCode debug file-service bytes; not a model tool-use or task result",
            "observer_sha256": hashlib.sha256(observer.read_bytes()).hexdigest(), "cases": rows}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("observer", "prepared", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    os.umask(0o077)
    result = run(args.observer.resolve(strict=True), args.prepared.resolve(strict=True))
    with args.report.open("x") as output:
        json.dump(result, output, indent=2)
        output.write("\n")
    print(json.dumps(result))
