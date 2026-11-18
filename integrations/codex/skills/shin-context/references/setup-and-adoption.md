# Project setup and exclusions

The user chooses a folder and which agent settings may access it. Register the
folder in Monitor or run `shin-ce init` from the project with the installed kit.
CLI registration and client connection are separate actions. Use the existing
home and selected agent settings. An MCP read grants no authority to enroll,
detach or change access.

Shin discovers eligible text recursively. An unfamiliar extension or file layout
needs no special registration. Registering the folder does not write project
configuration. Project `.gitignore` files apply to paths on disk. An optional
UTF-8 `.shinignore` in the project root takes precedence. Blank lines and comments starting with `#` are ignored. Use one pattern
per line and `/` between path components. Paths with multiple components are
root-relative. A basename matches at any depth unless anchored with a leading
slash. `*` and `?` match within a component,
and `**/` matches zero or more directories. Use `!/local-notes/handbook.md` to include an eligible file inside a Git-ignored
directory. The last matching Shin rule wins. Built-in exclusions, safety and
resource limits still apply.

Use `shin_projects` and `shin_status` to check the selected root, then issue a
focused query. The service observes added, moved and deleted paths and exclusion
edits. New kits rebuild each registered folder with its saved agent selections.
Report partial coverage, exclusions, blocked files and missing evidence as
returned. For a reproducible miss retain the original question and a safe
expected location for Shin maintainers.

Explicit standalone Core qualification uses its own fixtures, profiles and
strict eval/Pack checks. Start that workflow only for a requested engine
development or diagnostics task in an external output directory. A failed
managed read is resolved through its own grant, freshness and integrity checks.
