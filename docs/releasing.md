# Release a preview

Maintainers with repository write access can release from GitHub without a local
Git command or an additional token.

1. Open **Actions → Release preview → Run workflow**.
2. Select **main**. Optionally enter Markdown release notes; leave the field blank
   to use prepared notes or the new commit subjects.
3. Select **Run workflow**. Open its summary and follow **Builds, tests and publication**.

The workflow uses an untagged prepared Observer version when one exists.
Otherwise, new commits since the current preview advance its `dev.N` number by
one. It synchronizes the Observer version and development policy, prepares release
notes, and commits only those release files. Core keeps its independently declared
version. Existing tags and notes are never overwritten. A concurrent `main` update
refuses the handoff; start a fresh run against the updated main.

Release preparation checks that the platform job names, qualification and upload
steps, and artifact names in the workflow match the publisher's expectations.
Run `python3 -B tools/test_publish_preview.py` locally to catch a rename before
creating a tag.

The preparation run then starts **Build, test and publish** for that exact tag.
Its green preparation step means the platform checks have started, not that all
downloads are ready. Each platform publishes after its own Core, Observer and
exact-package gates pass. Missing macOS runners do not block qualified Windows or
Linux downloads. Monitor offers an update only when its platform's assets are ready.
Download links in maintained guides stay on a fully published preview until they
are explicitly refreshed.

If preparation created the tag but failed to start CI, start **Release preview**
again from main with Notes empty. With no new commits it resumes the same tag,
without an empty release or another version commit. For an already started CI run,
use its **Re-run failed jobs** control for infrastructure failures. Source fixes
belong in main and a new preview; keep published tags immutable.

## Which workflow to choose

| Workflow | Purpose |
| --- | --- |
| **Release preview** | Normal maintainer entry point: prepare the next version and start release CI. |
| **Build, test and publish** | Platform build/test/package runs. PRs and manual branch runs only check source; preview tag runs also publish qualified packages. |
| **Retry artifact publication (advanced)** | Retry an upload failure using an existing tag and the exact already qualified run, attempt and platform. It does not rebuild or bypass failed source/package checks. |

For the advanced workflow, use the run ID from the build run's URL and its exact
attempt number. The normal release path invokes the publisher automatically.
Implementation: [release preparation](../tools/create_preview.py),
[platform checks](../.github/workflows/ci.yml),
[guarded publisher](../tools/publish_preview.py).

The source publisher retries artifact and platform-receipt downloads at most
three times within the original three-minute read deadline. Each attempt uses
fresh temporary staging; partial files cannot enter qualification. Exhaustion
reports a bounded CLI/HTTP error with download URLs removed. Release creation,
uploads and edits remain single attempts followed by independent readback.
