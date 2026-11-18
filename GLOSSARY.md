# Shin Context Engine

Shin provides current, source-backed context to coding agents. These terms keep
project state, retrieval quality, agent access and software releases distinct.

## Language

**Project**:
A selected source folder with an identity. Registration lets Shin discover
eligible text under that folder. Agent access determines which connections can use it.

**Profile**:
A standalone Core configuration that selects inputs and defines retrieval
scenarios. A named agent profile is a separate client settings destination.

**Source scope**:
The files eligible for search within a root, subject to exclusions, safety
checks and resource limits.

**Source coverage**:
The safely observed portion of the selected source scope. Withheld or unsupported
inputs limit coverage even when other verified context is available.

**Snapshot**:
An identifiable version of indexed project context. Its identity alone does not
establish that the source is still current.

**Context publication**:
A qualified snapshot selected for serving project context.
_Avoid_: Release, when referring to indexed project data.

**Context delivery**:
The evidence actually included in a bounded query response. Available candidates,
omitted fragments and answer correctness are separate from what was delivered.

**Qualification**:
Evidence that a candidate satisfies the required checks for its declared use.
Source tests, package checks and installed-operation checks qualify different uses.

**Retrieval availability**:
Whether safe, current project context can be served. Declared search quality and
temporary reader preparation are separate aspects of the project state.

**Search quality**:
How retrieval performs on a measured task at a snapshot. Core developers use
strict scenarios to qualify the engine. Managed project registration has no
scenario-authoring step.

**Agent access**:
The projects an agent connection is allowed to query and read. Project
registration alone does not grant access to every connection.

**Kit**:
An installable group of Shin tools for a particular platform and software version.

**Preview release**:
A published development distribution of Shin software, with independently
qualified platform downloads.
_Avoid_: Publication, when referring to a software release rather than indexed context.
