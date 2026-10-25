# Contributing

For a bug or a proposed capability, open an [Issue](https://github.com/toseek-samurai/shin-context-engine/issues/new/choose)
with the problem, expected outcome, and a minimal reproduction where applicable.
Discuss larger changes before implementation. Small documentation and maintenance
fixes can go directly to a pull request.

Link related issues in pull requests and, where practical, in commit messages.
Prefer Conventional Commits, for example `fix(cli): restore terminal state (#123)`.
Use `Refs #123` for related work; use `Closes #123` in a pull request description
only when the complete issue is resolved and verified. Numbers here are examples.

Keep changes focused on a reproduced problem or an agreed capability. Explain
the behavior, the responsible layer, and the checks you ran. Include a minimal
synthetic regression fixture for behavior changes, especially source boundaries,
freshness, isolation, ownership, cancellation, and recovery.

Do not include private repositories, customer data, credentials, local runtime
state, generated indexes, or machine-specific paths. Preserve third-party
copyright notices and licenses. Contributions are submitted under Apache-2.0
unless explicitly agreed otherwise; see [LICENSE](LICENSE).

Build prerequisites and component scope are described in [README](README.md).
The [test directory guide](tests/README.md) explains test ownership and commands.
Run the affected component's CTest suite. Shared Core changes require companion
checks as well. Keep original limits and negative assertions; do not skip a
failing test to claim support. Report untested platforms and installation steps.

Source builds and tests do not authorize changing a running installation or
enrolling a project. Use disposable, explicitly selected state for qualification.
Report security issues through [the private reporting channel](SECURITY.md).
