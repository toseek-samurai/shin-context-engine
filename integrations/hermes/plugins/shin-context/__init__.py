"""Hermes registration for the Shin Context Engine adapter."""

from pathlib import Path

from . import adapter, schemas


_TOOLS = (
    ("shin_discover_project", schemas.DISCOVER_PROJECT, adapter.discover_project),
    ("shin_ready_context", schemas.READY_CONTEXT, adapter.ready_context),
    ("shin_brief_context", schemas.BRIEF_CONTEXT, adapter.brief_context),
    ("shin_pack_context", schemas.PACK_CONTEXT, adapter.pack_context),
    ("shin_diagnose_context", schemas.DIAGNOSE_CONTEXT, adapter.diagnose_context),
    ("shin_worktree_diff", schemas.WORKTREE_DIFF, adapter.worktree_diff),
    ("shin_suggest_profile", schemas.SUGGEST_PROFILE, adapter.suggest_profile),
)


def register(ctx):
    """Register the adapter tools and its read-only workflow skill."""
    for name, schema, handler in _TOOLS:
        ctx.register_tool(
            name=name,
            toolset="plugin_shin_context",
            schema=schema,
            handler=handler,
        )

    skill = Path(__file__).parent / "skills" / "workflow" / "SKILL.md"
    ctx.register_skill(
        "workflow",
        skill,
        "Use adopted Shin context before broad project scans or changes.",
    )
