"""JSON schemas exposed to Hermes models by the Shin plugin."""


def _object(properties, required):
    return {
        "type": "object",
        "properties": properties,
        "required": required,
        "additionalProperties": False,
    }


_ROOT = {
    "root": {
        "type": "string",
        "description": "Absolute path to the consumer repository root.",
    },
    "marker": {
        "type": "string",
        "description": (
            "Optional repository-relative Shin adoption marker. Omit to use "
            "the conventional marker locations."
        ),
    },
    "out_dir": {
        "type": "string",
        "description": (
            "Optional absolute external index directory. Omit to resolve the "
            "canonical index by marker index key."
        ),
    },
}

DISCOVER_PROJECT = {
    "name": "shin_discover_project",
    "description": (
        "Read-only Shin cold-start discovery for an unknown repository. Use "
        "before broad scans when no permanent Shin adoption marker is known."
    ),
    "parameters": _object({"root": _ROOT["root"]}, ["root"]),
}

READY_CONTEXT = {
    "name": "shin_ready_context",
    "description": (
        "Validate the adopted Shin route, external index, worktree freshness, "
        "and quality gates. This never builds or repairs an index."
    ),
    "parameters": _object(dict(_ROOT), ["root"]),
}

BRIEF_CONTEXT = {
    "name": "shin_brief_context",
    "description": (
        "Return one compact, current Shin brief for an adopted project. The "
        "adapter first requires strict readiness and stops on stale context."
    ),
    "parameters": _object(
        {
            **_ROOT,
            "query": {
                "type": "string",
                "description": "One bounded implementation or architecture question.",
            },
            "limit": {
                "type": "integer",
                "minimum": 1,
                "maximum": 20,
                "default": 10,
            },
            "per_doc": {
                "type": "integer",
                "minimum": 1,
                "maximum": 4,
                "default": 2,
            },
        },
        ["root", "query"],
    ),
}

PACK_CONTEXT = {
    "name": "shin_pack_context",
    "description": (
        "Return a compact, strict Shin Agent Context Pack from an in-memory "
        "shin.task-request.v1 object. The adapter requires readiness first."
    ),
    "parameters": _object(
        {
            **_ROOT,
            "task_request": {
                "type": "object",
                "description": "A bounded shin.task-request.v1 JSON object.",
            },
            "jobs": {
                "type": "integer",
                "minimum": 1,
                "maximum": 16,
                "default": 4,
            },
        },
        ["root", "task_request"],
    ),
}

DIAGNOSE_CONTEXT = {
    "name": "shin_diagnose_context",
    "description": (
        "Explain why one expected repository-relative path was returned, "
        "ranked out, excluded, unpublished, or absent from an adopted route."
    ),
    "parameters": _object(
        {
            **_ROOT,
            "query": {"type": "string"},
            "expected_path": {
                "type": "string",
                "description": "Repository-relative expected evidence path.",
            },
            "limit": {
                "type": "integer",
                "minimum": 1,
                "maximum": 20,
                "default": 10,
            },
            "per_doc": {
                "type": "integer",
                "minimum": 1,
                "maximum": 4,
                "default": 2,
            },
        },
        ["root", "query", "expected_path"],
    ),
}

WORKTREE_DIFF = {
    "name": "shin_worktree_diff",
    "description": (
        "Report unpublished profile-owned worktree drift against the selected "
        "Shin snapshot. Use this when readiness reports stale context."
    ),
    "parameters": _object(
        {
            **_ROOT,
            "limit": {
                "type": "integer",
                "minimum": 1,
                "maximum": 200,
                "default": 50,
            },
        },
        ["root"],
    ),
}

SUGGEST_PROFILE = {
    "name": "shin_suggest_profile",
    "description": (
        "Return a read-only profile recommendation from an explicit reviewed "
        "Shin routing contract. It never switches profiles or merges indexes."
    ),
    "parameters": _object(
        {
            "routing_config": {
                "type": "string",
                "description": "Absolute path to a shin.profile-routing.v1 file.",
            },
            "query": {"type": "string"},
            "from_root": {
                "type": "string",
                "description": "Optional absolute current repository root.",
            },
            "limit": {
                "type": "integer",
                "minimum": 1,
                "maximum": 20,
                "default": 5,
            },
        },
        ["routing_config", "query"],
    ),
}
