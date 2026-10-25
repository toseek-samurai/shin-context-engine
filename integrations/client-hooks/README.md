# Optional client status hook

This directory contains a source-only Python adapter for a Codex `SessionStart` event. It can add a short status note from an explicitly configured native Observer binding. It does not inspect a transcript or source file, enroll a project, assess readiness, or publish an index.

The hook is optional and is not part of the native macOS kit. It requires a POSIX Python runtime and an owner-managed registry that pins the Observer executable, configuration, and existing project binding. The hook reads one event from standard input and is silent when the event or binding is not usable.

The adapter source is [`shin_status.py`](shin_status.py). Its local status fields follow the [Observer contract](../../docs/machine/shin-local-observer-v1.json); the [protocol tests](../../tests/tooling/test_client_status_hook.py) describe the event and output boundary. These files document source behavior, not automatic host configuration or a live client installation.

A status note is cached observation only. It is not a freshness or retrieval decision. Use the project's normal current-context and readiness workflow before relying on project evidence.
