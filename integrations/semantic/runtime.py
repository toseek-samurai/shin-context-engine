"""Read-only, explicitly configured single-project query path."""

from contextlib import nullcontext
import os
from pathlib import Path
import resource
import subprocess
import sys

from .artifact import Artifact, Rejected, SHA, SLUG, canonical, decode, encode, read, require, root_identity, sha
from . import model
from .selection import navigate

MAX_OUTPUT = 128 * 1024
BASE_ROUTE_FIELDS = {"core_binary", "core_sha256", "root", "root_identity", "project", "profile",
                     "profile_path", "profile_sha256", "out_dir", "model_dir"}


def validate_base_route(config):
    require(isinstance(config, dict) and BASE_ROUTE_FIELDS <= set(config), "invalid-semantic-route")
    for field in ("root", "core_binary", "profile_path", "out_dir", "model_dir"):
        canonical(config[field])
    for field in ("core_sha256", "profile_sha256"):
        require(isinstance(config[field], str) and SHA.fullmatch(config[field]), "invalid-route-pin")
    for field in ("project", "profile"):
        require(isinstance(config[field], str) and SLUG.fullmatch(config[field]), "invalid-route-name")
    require(Path(config["profile_path"]).name == config["profile"] + ".json", "profile-path-mismatch")


def verify_base_route(config):
    require(root_identity(canonical(config["root"])) == config["root_identity"], "root-identity-changed")
    require(sha(read(canonical(config["profile_path"]), 1024 * 1024)) == config["profile_sha256"], "profile-changed")
    require(sha(read(canonical(config["core_binary"]), 64 * 1024 * 1024)) == config["core_sha256"], "core-binary-changed")


class Session:
    def __init__(self, config_path):
        config_path = canonical(str(config_path))
        data = read(config_path, 65536, private=True)
        config = decode(data)
        require(isinstance(config, dict) and set(config) == {
            "schema_version", "core_binary", "core_sha256", "root", "root_identity", "project", "profile",
            "profile_path", "profile_sha256", "out_dir", "artifact_dir", "artifact_sha256", "model_dir"}, "invalid-semantic-route")
        require(config["schema_version"] == "shin.semantic-route.v1")
        validate_base_route(config)
        canonical(config["artifact_dir"])
        require(isinstance(config["artifact_sha256"], str) and SHA.fullmatch(config["artifact_sha256"]), "invalid-route-pin")
        self.path, self.config_bytes, self.config = config_path, data, config
        self.artifact = self.model = None
        self.verify_route()

    def verify_route(self):
        require(read(self.path, 65536, private=True) == self.config_bytes, "route-config-changed")
        verify_base_route(self.config)

    def core_arguments(self):
        config = self.config
        return [config["core_binary"], "--profile", config["profile"], "--profile-dir", str(Path(config["profile_path"]).parent),
                "--root", config["root"], "--out-dir", config["out_dir"]]

    def lexical(self, query):
        require(isinstance(query, str) and 0 < len(query.encode()) <= 4096 and "\0" not in query
                and not query.startswith("-"), "invalid-query")
        self.verify_route()
        config = self.config
        args = self.core_arguments() + ["--from-index", "brief", query,
                "--limit", "20", "--per-doc", "2", "--projection", "compact", "--require-current"]
        process = subprocess.run(args, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                 timeout=60, env={"PATH": "", "LC_ALL": "C"})
        require(process.returncode == 0 and len(process.stdout) <= MAX_OUTPUT, "core-admission-rejected")
        core = decode(process.stdout)
        require(core.get("ok") is True and core.get("tool") == "shin-context" and core.get("command") == "brief"
                and core.get("schema_version") == "shin.context-brief-compact.v1" and core.get("index_mode") == "generated-index"
                and core.get("query") == query and core.get("profile") == config["profile"], "core-response-mismatch")
        freshness = core["index_freshness"]
        require(freshness["current"] is True and freshness["fully_verified"] is True
                and freshness["status"] == "current" and freshness["snapshot_id"] == core["snapshot_id"], "core-currentness-rejected")
        self.verify_route()
        return core

    def inspect(self, path):
        require(isinstance(path, str) and 0 < len(path.encode()) <= 4096
                and not path.startswith(("/", "-")) and "\\" not in path
                and all(part not in ("", ".", "..") for part in path.split("/"))
                and all(ord(c) >= 32 and ord(c) != 127 for c in path), "invalid-inspect-path")
        self.verify_route()
        process = subprocess.run(self.core_arguments() + ["inspect", path, "--require-current"],
                                 stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                 timeout=60, env={"PATH": "", "LC_ALL": "C"})
        require(process.returncode == 0 and 0 < len(process.stdout) <= 65536, "core-admission-rejected")
        core = decode(process.stdout)
        require(core.get("ok") is True and core.get("tool") == "shin-context" and core.get("command") == "inspect"
                and core.get("schema_version") == "shin.file-inspection.v1" and core.get("index_mode") == "generated-index"
                and core.get("path") == path and core.get("profile") == self.config["profile"]
                and core.get("root") == self.config["root"], "core-response-mismatch")
        freshness = core["index_freshness"]
        require(freshness["current"] is True and freshness["fully_verified"] is True
                and freshness["status"] == "current" and freshness["snapshot_id"] == core["snapshot_id"], "core-currentness-rejected")
        self.verify_route()
        result = {"schema_version": "shin.agent-inspect.v1", "ok": True, "project": self.config["project"],
                  "native": core, "semantic_status": "not-used-native-inspect",
                  "admission": {"kind": "core-current-only", "ready": None,
                                "readiness": "not-evaluated-use-canonical-adoption-gates",
                                "source_admission": "current-at-core-admission-not-a-future-mutation-guarantee"}}
        require(len(encode(result)) + 1 <= MAX_OUTPUT, "agent-inspect-output-budget")
        return result

    def pack(self, task):
        require(isinstance(task, dict) and task.get("schema_version") == "shin.task-request.v1", "invalid-task-request")
        payload = encode(task)
        require(len(payload) <= 128 * 1024, "task-request-byte-budget")
        self.verify_route()
        args = self.core_arguments() + ["pack", "--input", "-", "--jobs", "1",
                                       "--projection", "compact", "--require-current", "--strict"]
        process = subprocess.run(args, input=payload, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                 timeout=120, env={"PATH": "", "LC_ALL": "C"})
        require(process.returncode in (0, 1) and 0 < len(process.stdout) <= MAX_OUTPUT, "core-admission-rejected")
        core = decode(process.stdout)
        require(core.get("tool") == "shin-context" and core.get("command") == "pack"
                and core.get("schema_version") == "shin.agent-context-pack-compact.v1"
                and core.get("projection") == "compact" and core.get("profile") == self.config["profile"]
                and core.get("root") == self.config["root"] and core.get("request_id") == task.get("request_id")
                and core.get("require_current") is True and core.get("strict") is True
                and type(core.get("ok")) is bool and process.returncode == (0 if core["ok"] else 1),
                "core-response-mismatch")
        freshness = core["index_freshness"]
        require(freshness["current"] is True and freshness["fully_verified"] is True
                and freshness["status"] == "current" and freshness["snapshot_id"] == core["snapshot_id"], "core-currentness-rejected")
        self.verify_route()
        result = {"schema_version": "shin.agent-pack.v1", "ok": core["ok"], "project": self.config["project"],
                  "native": core, "semantic_status": "not-used-native-pack",
                  "admission": {"kind": "core-current-only", "ready": None,
                                "readiness": "not-evaluated-use-canonical-adoption-gates",
                                "source_admission": "current-at-core-admission-not-a-future-mutation-guarantee"},
                  "interpretation": {"requirements": "caller-supplied", "source_inspection_required": True,
                                     "answer_completeness": "not-asserted"}}
        require(len(encode(result)) + 1 <= MAX_OUTPUT, "agent-pack-output-budget")
        return result

    def read(self, request):
        require(isinstance(request, dict) and request.get("schema_version") in
                ("shin.source-read-request.v1", "shin.source-read-request.v2"),
                "invalid-source-read-request")
        whole_files = request["schema_version"] == "shin.source-read-request.v2"
        payload = encode(request)
        require(len(payload) <= 128 * 1024, "source-read-request-byte-budget")
        self.verify_route()
        process = subprocess.run(self.core_arguments() + ["read", "--input", "-"], input=payload,
                                 stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                 timeout=60, env={"PATH": "", "LC_ALL": "C"})
        require(process.returncode == 0 and 0 < len(process.stdout) <= MAX_OUTPUT, "core-admission-rejected")
        core = decode(process.stdout)
        require(core.get("ok") is True and core.get("tool") == "shin-context" and core.get("command") == "read"
                and core.get("schema_version") == ("shin.source-read.v2" if whole_files else "shin.source-read.v1")
                and core.get("profile") == self.config["profile"]
                and core.get("snapshot_id") == request.get("snapshot_id") and type(core.get("complete")) is bool
                and isinstance(core.get("evidence"), dict)
                and all(isinstance(core["evidence"].get(field), list)
                        for field in ("files" if whole_files else "records", "bodies", "omissions")),
                "core-response-mismatch")
        freshness = core["index_freshness"]
        require(freshness["current"] is True and freshness["fully_verified"] is True
                and freshness["status"] == "current" and freshness["snapshot_id"] == core["snapshot_id"],
                "core-currentness-rejected")
        self.verify_route()
        result = {"schema_version": "shin.agent-read.v1", "ok": True, "project": self.config["project"],
                  "native": core, "semantic_status": "not-used-native-read",
                  "admission": {"kind": "core-current-only", "ready": None,
                                "readiness": "not-evaluated-use-canonical-adoption-gates",
                                "source_admission": "current-at-core-admission-not-a-future-mutation-guarantee"}}
        require(len(encode(result)) + 1 <= MAX_OUTPUT, "agent-read-output-budget")
        return result

    def query(self, query, *, trace=None):
        phase = trace.phase if trace is not None else lambda _: nullcontext()
        with phase("core-brief"):
            core = self.lexical(query)
        result = {"schema_version": "shin.semantic-query.v1", "ok": True, "project": self.config["project"],
                  "core": core, "navigation": None, "semantic_status": "not-needed-exact-or-owner",
                  "source_admission": "current-at-core-admission-not-a-future-mutation-guarantee"}
        if core["retrieval_mode"] not in ("identifier-scoped", "owner-scoped"):
            with phase("optional-runtime"):
                directory, model_dir = canonical(self.config["artifact_dir"]), canonical(self.config["model_dir"])
                if self.artifact is None and not directory.exists():
                    result["semantic_status"] = "artifact-unavailable-lexical-only"
                elif self.model is None and not model_dir.exists():
                    result["semantic_status"] = "model-unavailable-lexical-only"
                elif self.model is None and not model.available():
                    result["semantic_status"] = "runtime-unavailable-lexical-only"
                else:
                    result["semantic_status"] = "available"
            if result["semantic_status"] == "available":
                with phase("artifact-admission"):
                    if self.artifact is None:
                        self.artifact = Artifact(directory, self.config["artifact_sha256"], self.config)
                    self.artifact.admit(core)
                    require(self.artifact.manifest["model"] == model.MODEL, "unsupported-semantic-model")
                if self.model is None:
                    with phase("model-load"):
                        self.model = model.load(model_dir, self.artifact.manifest["model"])
                with phase("query-embedding"):
                    import numpy as np
                    vector = np.asarray(next(self.model.embed([model.MODEL["query_prefix"] + query])), dtype=np.float32)
                    require(vector.shape == (384,) and np.isfinite(vector).all()
                            and abs(float(np.linalg.norm(vector)) - 1) < 0.001, "invalid-query-vector")
                with phase("selection"):
                    navigation, excluded = navigate(self.artifact, core, self.artifact.matrix @ vector)
                result.update(navigation=navigation, lexical_excluded=excluded, semantic_status="available",
                              artifact_sha256=self.config["artifact_sha256"])
        with phase("response-validation"):
            rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss * (1 if sys.platform == "darwin" else 1024)
            require(rss <= 3 * 1024 * 1024 * 1024, "semantic-memory-budget")
            require(len(encode(result)) <= MAX_OUTPUT, "semantic-output-budget")
        return result
