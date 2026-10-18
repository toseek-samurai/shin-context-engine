# Native client setup

## Automatic setup (next preview: dev.12)

This source revision includes automatic setup. The published dev.9 preview still
uses the manual configuration below.

1. Open Monitor → **Set up Shin…** and choose a project folder. Shin connects it
   and starts the local service, including on a fresh installation.
2. Select the agents and profiles that should have access, then click
   **Connect selected agents**. No JSON copying is needed.
3. Restart or reconnect those agents. Ask them to call `shin_status` and verify
   the selected project is available. **Check project** in Monitor checks local
   search readiness; it does not claim that a running agent has reloaded its settings.

For an existing project, use **Projects → project → actions → Connect coding agent…**.
The list detects settings directories and, on macOS, installed applications.
It shows the exact destination path. Detection does not modify files or select
agents on your behalf. Undetected supported agents can be configured for later use.
**Choose settings file…** selects an existing custom configuration.

| Agent | Default destination |
| --- | --- |
| Codex | `~/.codex/config.toml`, respecting `CODEX_HOME` |
| OpenCode | `~/.config/opencode/opencode.jsonc` when present, otherwise `opencode.json`; respects `XDG_CONFIG_HOME` and `OPENCODE_CONFIG` |
| Command Code | `~/.commandcode/mcp.json` (user scope) |
| Hermes | `~/.hermes/config.yaml`, respecting `HERMES_HOME`; named profiles under `~/.hermes/profiles` are listed separately |

Choose the profile you actually use. Project-specific or centrally managed client
settings can take precedence over these user settings. Connecting a Hermes
profile does not switch the active profile or configure its siblings.
Rider/Junie and other clients do not yet have automatic writers; use the manual
export and [special-case instructions](#special-cases) below for those hosts. Shin does not edit a running IDE's private XML files.

Existing Shin roots, other servers, model/provider settings and permissions are
retained. If the same `shin` entry uses another executable or data home, Shin
reports a conflict rather than taking it over. An existing disabled matching
entry is enabled when explicitly selected. Repeating the operation is a no-op.
Comments outside changed values and unrelated source text are retained.

All selected files are checked before writing begins. Each changed existing file
gets a mode-0600 copy under its parent's `.shin-backups` directory. Replacements
are atomic and retain file permission bits; changed revisions, symlinks, hardlinks,
foreign ownership and writable-by-others files are refused. Invalid or ambiguous
formats, including YAML anchors, aliases and merge keys, are preserved for manual
review. Configuration files are bounded to 256 KiB.
If a file changes during the write phase, earlier successful targets stay configured
and the UI reports where it stopped. Retry safely after reviewing that conflict;
already configured targets are not duplicated. Restore a backup only after stopping
the affected client and reviewing changes made since that backup.

The CLI exposes discovery and the same two-step write contract:

```sh
shin-ce client-targets
shin-ce client-connect --client codex --root /path/to/project
# Review the returned destination and revision, then apply that exact plan:
shin-ce client-connect --client codex --root /path/to/project --revision REVISION
```

Use `--config /absolute/path/to/settings` for a specific profile or custom file.
Portable kits also need `--home` pointing to the same data home used for `init`.
The commands do not enroll projects, run other clients, or print existing secrets.

A read-only export remains available for manual integrations:

```sh
shin-ce mcp-config --client codex --root /path/to/project --text
shin-ce mcp-config --client opencode --root /path/to/project --text
shin-ce mcp-config --client rider --root /path/to/project --text
```

Client format references: [Codex](https://developers.openai.com/codex/mcp),
[OpenCode](https://opencode.ai/docs/mcp-servers/),
[Command Code](https://commandcode.ai/docs/mcp),
[Hermes](https://hermes-agent.nousresearch.com/docs/user-guide/features/mcp),
[Rider AI Assistant](https://www.jetbrains.com/help/ai-assistant/mcp.html).

## Special cases

### Rider AI Assistant and Junie

Export the configuration for your enrolled project:

```sh
shin-ce mcp-config --client rider --root /absolute/path/to/project --text
```

For a portable installation, add `--home /absolute/path/to/private-data`.
The export command is available in the next preview; for older kits use the
command and arguments in [Manual configuration](#manual-configuration).

- **Rider AI Assistant:** open Settings → Tools → AI Assistant → Model Context
  Protocol, add a STDIO server and paste the exported configuration. Choose the
  intended project scope and apply. See the
  [AI Assistant MCP guide](https://www.jetbrains.com/help/ai-assistant/mcp.html).
- **Junie:** use Settings → Tools → Junie → MCP Settings, or merge the `shin`
  server entry into `.junie/mcp/mcp.json` for the project. The IDE settings page
  saves user-wide configuration to `~/.junie/mcp/mcp.json`. Preserve existing
  server entries. See the [Junie IDE guide](https://junie.jetbrains.com/docs/junie-ide-plugin.html)
  for the controls supported by your installed version.

These are separate client integrations. Rider's **MCP Server** page exposes IDE
features to other agents; it does not connect an agent to Shin.

### Custom settings and profiles

For a supported client, use **Choose settings file…** in the setup window. Select
that client's actual configuration file, including its project-specific file if
it overrides user settings. The format must match the selected client.

The equivalent CLI flow keeps the same destination for preview and apply:

```sh
shin-ce client-connect --client codex --root /absolute/path/to/project \
  --config /absolute/path/to/config.toml
shin-ce client-connect --client codex --root /absolute/path/to/project \
  --config /absolute/path/to/config.toml --revision REVISION
```

Use the revision returned by the first command. If using a portable data home,
pass the same `--home` to both commands. For another MCP host, enter the absolute
`shin-mcp` executable and argument list from [Manual configuration](#manual-configuration)
using that host's STDIO settings.

### An existing Shin entry conflicts

Compare the existing executable, `--home` and all `--root` arguments with the
installation you intend to use. If both installations are needed, create a
separately named MCP server entry through the client's own settings. If replacing
an old entry, back up the file and review the roots and data home before editing;
changing the executable alone does not migrate enrolled projects or their data.

For configurations using YAML aliases, managed symlinks or other unsupported
layouts, merge the entry manually using the client's documented configuration
mechanism. Keep its existing structure and other settings intact.

### The agent still shows the old project list

Restart the affected agent or reconnect its Shin MCP server. Ask it to call
`shin_projects`, select the intended project from that response, then call
`shin_status` with that project handle. If it is still missing, check the active
client profile, settings precedence and the `--root` arguments in that entry.
Adding a project to Shin and allowing a particular MCP client to see it are
separate operations; the setup window performs both for the selected clients.

### Restore a configuration backup

Close the affected client. Back up its current configuration before restoring.
Find the original file under the configuration directory's `.shin-backups`
subdirectory and compare it with the current file. Restore only the intended
file, retaining any later settings changes you still need; do not replace the
whole configuration directory. Reopen the client and check its other MCP servers
as well as Shin. Restoring client settings does not remove a project from Shin.

## Manual configuration

The kit includes `bin/shin-mcp`, a native stdio MCP server. It serves only the project roots supplied at process startup and uses local project bindings under the selected data home. It does not provide login, select a project implicitly, or authorize publication.

For a default per-user installation, use the installed binary and an absolute
project path. The installation already records the data home:

```json
{
  "command": "/Users/YOUR_NAME/.local/opt/shin-local/bin/shin-mcp",
  "args": ["--root", "/absolute/path/to/project"]
}
```

Replace `YOUR_NAME` and the project path; JSON command fields do not expand `~`.
Initialize the project with `shin-ce init` first and check `shin-ce status --text`.

For portable use, keep each client entry scoped to the intended project and set
`--home` to the same private data home used by `shin-ce`. A single-root
configuration has this command and argument shape:

```json
{
  "command": "/path/to/extracted-kit/bin/shin-mcp",
  "args": [
    "--root", "/path/to/project",
    "--home", "/path/to/private-data"
  ]
}
```

Use the configuration field names required by the MCP host. Preserve its existing unrelated settings. Do not paste credentials into this entry; the local adapter does not use an account credential.

For a conversation that needs several projects, supply each allowed root explicitly. The adapter advertises project listing and requires the selected project handle on subsequent project calls. A host that does not support this flow can use one MCP entry per project instead.

```json
{
  "command": "/path/to/extracted-kit/bin/shin-mcp",
  "args": [
    "--root", "/path/to/project-a",
    "--root", "/path/to/project-b",
    "--home", "/path/to/private-data"
  ]
}
```

The native `shin-ce` CLI can query the same enrolled project without changing client settings:

```sh
/path/to/extracted-kit/bin/shin-ce query "a focused question" \
  --root /path/to/project --home /path/to/private-data
/path/to/extracted-kit/bin/shin-ce inspect path/to/file.cpp \
  --snapshot SNAPSHOT_ID --root /path/to/project --home /path/to/private-data
/path/to/extracted-kit/bin/shin-ce read path/to/file.cpp \
  --snapshot SNAPSHOT_ID --root /path/to/project --home /path/to/private-data
```

Use the snapshot returned by the query for `inspect` and `read`. Paths are project-relative. Check project and service status before relying on results. Missing or stale evidence is not permission to enroll, rebuild, or replace a project index; ask its owner before changing an existing binding or publication authority.

For installation, package contents, and the local service lifecycle, see the [local kit guide](local-kit.md).
