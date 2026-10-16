# Native client setup

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
