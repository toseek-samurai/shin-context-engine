# Experimental semantic integration

This directory contains optional Python source for a semantic navigation path alongside native Core retrieval. The query helper admits a pinned, current Core result first; when separately configured local artifacts and runtime are available, it may add navigation candidates. Those candidates are not answers or verified source reads.

The integration is experimental and is not enabled by default or required by Core, Observer, or the native macOS kit. The native kit does not require Python semantic adapters or a model service. The optional path uses an explicitly supplied local runtime and model files; it does not provision them.

Source entry points are [`tools/shin_semantic.py`](../../tools/shin_semantic.py) for queries and [`tools/shin_semantic_build.py`](../../tools/shin_semantic_build.py) for an explicitly configured local artifact build. The range exporter is [`tools/semantic_source_export.cpp`](../../tools/semantic_source_export.cpp). Implementation modules here validate the route and artifacts, project ranges, and keep semantic navigation separate from the native result.

Use the current [retrieval evidence contract](../../docs/machine/retrieval-evidence-v1.json) and [source member range contract](../../docs/machine/source-member-ranges-v1.json) for Core evidence boundaries. This experimental integration does not change Core ranking, profile scope, publication, or source-read authority.
