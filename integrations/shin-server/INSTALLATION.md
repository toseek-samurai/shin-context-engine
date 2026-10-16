# Headless Candidate Installation And Rollback

Development candidate procedure, not Stable promotion or a global installer.
Current delivery is a loopback Server with PostgreSQL and independent local
Core/Observer. Remote multi-host deployment and mixed-version fleets are not
qualified. Core remains usable without Server, PostgreSQL or Git.

The first coordinated release-candidate identity is `2.0.0-rc.1` for all three
components. `stability=development` still means a prerelease, including RC;
it is not approved Stable delivery. An RC may satisfy a lower released adoption
minimum, but never the equal final `2.0.0` minimum. Do not change consumer
minimums to prerelease strings. Historical contract baseline and generator
identities remain independently pinned; RC does not redate inherited evidence.

## Prepare An Explicit Prefix

Use `make candidate-kit KIT_CORE_BUILD=... KIT_SERVER_BUILD=...
KIT_OBSERVER_BUILD=... KIT_OUTPUT=build/new-kit` for repeatable assembly.
It uses existing CMake builds, refuses an existing destination, validates
identities, license notices and templates, and emits `prefix` plus a file-hash
manifest. It never compiles, downloads dependencies, starts services or grants
release approval. Temporary fixture/staging files are removed on exit.
The manifest detects local changes; it is not an authenticity signature.
The bundled contract library includes historical records; current protocol
inventory and release status govern use, not an old document's version stamp.

Stage the selected, already qualified Core/Server/Observer CMake builds with
`cmake --install BUILD --prefix PREFIX`, then relocate the prefix and verify
its binary identities and inventory. Keep the previous executable and matching
state under explicit operator custody; never overwrite the installed Stable
route as part of a candidate test. Core's generic profiles, contracts and
agent integrations live under `share/shin-context`.

Companion binaries require separately provisioned system libraries. Verify the
actual platform's loader dependencies, not just the database server version.
On the measured macOS host Server uses libpq14.24 and OpenSSL libcrypto3;
Observer uses libcrypto3, while Core has no PostgreSQL/crypto dependency.
Server dev71 also links libcurl7.85+ with TLS/asynchronous DNS for optional
OIDC discovery and JWKS refresh. Verify this runtime dependency on the target
host even when OIDC is not configured. The current macOS build uses system curl;
no curl executable or Python authentication runtime is required.
Relocation on a provisioned host does not prove operation on a clean machine.

On the measured Ubuntu24.04 AMD64 host, the owner installed the runtime-only
PostgreSQL client package with `sudo apt-get install --no-install-recommends libpq5`
(16.15). This does not install a PostgreSQL server. OpenSSL and the C/C++ runtime
libraries must also resolve on the destination. Check `ldd PREFIX/bin/shin-server`
and `ldd PREFIX/bin/shin-observer` for missing libraries after relocation.
The candidate assembler deliberately removes `LD_*`/`DYLD_*` overrides from its
probes: a private build dependency directory is not an installed prerequisite.

For WSL qualification, observe wall-clock steps before lease, expiry and HTTP
workloads. A successful build or `NTPSynchronized=yes` is not that proof. The
2026-10-01 stand stepped backwards three times in120 seconds, so its database
workflow qualification was held separately from passing offline checks.
[Ubuntu documents possible conflicts between Hyper-V and guest NTP](https://ubuntu.com/wsl/docs/stable/explanation/time-sync/).
Resolve host time administration explicitly; do not loosen Shin's freshness,
lease or timestamp assertions, or change host clock services from a test script.
On2026-10-02 the owner synchronized Windows Time and disabled the competing
Ubuntu systemd-timesyncd service, retaining Hyper-V synchronization and default
tsc. A five-minute observation and the unchanged fresh-report tests then passed;
see `tests/evidence/shin-r3-wsl-clock-20261002.md` in the source checkout. Verify
the host again after restart/suspend; installers must not apply this OS policy
automatically.

## External Configuration

An explicitly selected Linux Core CLI may embed mimalloc3.5.3 with transparent
huge pages disabled. The candidate manifest's `core_allocator` and installed
`share/shin-context/shin-core-allocator.json` record this variant; its MIT license
is under `share/shin-context/licenses/mimalloc`. No `LD_PRELOAD`, shared mimalloc
library or environment setup is required. Server/Observer are unchanged.
Earlier/default kits may have no allocator policy record; never infer mimalloc
from the Core version alone. This opt-in is not Stable delivery or cutover.

Use `shin-companion-protocols-v1.json` for current schemas and the full Server
template. Substitute reviewed paths, independently chosen deployment identity,
principal token digests and allowed projects; placeholders are not usable
credentials. Configurations and credential files must be private, outside
source/index trees. Use separate runtime and schema-owner connection files.

Run `shin-server check-config --config /absolute/private/runtime.json` and the
same check for the migrator configuration. These checks do not contact the
database or prove readiness. With old writers stopped and a reviewed backup,
run explicit `shin-server migrate --config /absolute/private/migrator.json`;
ordinary `serve` does not migrate. Then run
`shin-server serve --config /absolute/private/runtime.json` in the foreground
and verify health, authentication and the independently pinned Server identity.
No daemon installation, credential creation or schema change is automatic.

Observer source config v2 names separate source, profile registry, index and
private state roots. Delivery is optional and separately configured with an
explicit Server/principal pin and token file; see `shin-observer-delivery-v1`.
`sample` mutates local state; `status` does not. Neither sends data without an
explicit delivery operation/configuration. A configuration template does not
grant permission to enroll a client or transmit metadata.

## Version Switches And Rollback

1. Stop writers and preserve the reviewed configuration, database/artifact
   backup and matching binaries. Check schema/config compatibility explicitly.
2. Start the selected binary. A changed Server version changes the execution
   fingerprint. Existing activation may return `active-generation-route-changed`;
   do not edit stored fingerprints or turn off freshness verification.
3. Inspect the current activation and source, explicitly prepare, inspect and
   pass preflight, then activate with the observed revision. Reassess the saved
   task and receipts; healthy process status alone does not restore context.
4. Returning to the candidate repeats this reviewed preparation/activation.
   Snapshot content can stay identical while preparation identity changes.
5. For incompatible schema versions, never run a destructive down-migration.
   Restore the matching backup in an isolated destination using the recovery
   contract. Review deployment identity rotation and client re-enrollment;
   binary replacement alone is not database rollback.

The dev38 -> dev37 -> dev38 fixture proves same-schema019 switching, version
fencing, explicit reactivation and checkpoint preservation. Dev37 is a test
control, not a recommended deployed fallback: it predates Server-owned umask
hardening and the fixture supplies0077 to its launcher. Candidate dev38 is
still tested under0000. An actual fallback must satisfy current safety policy.
That historical same-Core fixture does not qualify current dev39/Core22 to
dev38/Core21 rollback. The driver rejects differing Core identities even with
matching protocols, before fixture mutation; no cross-Core reader compatibility
is implied by an unchanged database schema.

## Matched-Kit Restore Across Core Versions

The2026-10-02 owner decision chooses full matched-kit restore for R3, not an
in-place cross-Core downgrade. Keep the previous Core/Server/Observer, its
configuration and matching stopped database/artifact backup together. Verify
the exact kit inventory and embedded Core identities before starting services.
Restore into a separate destination with the prior kit; follow recovery's
identity rotation, old-client fencing and explicit preparation/activation.
Never point the older Server at the candidate's live data merely because both
use schema019. The same-Core binary-switch guard above remains unchanged.

Restored state is the previous backup point. Later candidate-only writes are
not merged or implicitly retained in that restored view. Preserve the current
deployment separately and review that loss of recency before an actual cutover.
The local fixture owns disposable databases; its automatic deletion must not
be copied into a production rollback procedure.

`make kit-restore-check KIT_RESTORE_PREVIOUS_PREFIX=...`
`KIT_RESTORE_CANDIDATE_PREFIX=... KIT_RESTORE_REPORT=build/new-report.json`
reuses the existing guarded headless and backup/restore fixtures. It captures
the previous kit's backup, exercises the candidate on separate data, restores
the old backup with the old kit, and verifies that candidate data was untouched.
Both prefixes must have verified candidate manifests. It preserves a separate
workflow receipt and emits `shin.full-kit-restore-report.v1`; altered/mixed kits
are refused before the stand is mutated. This is scripted local proof, not a
new model-agent session, an in-place upgrade, a production backup utility,
credential/role backup, cross-host recovery or general schema compatibility.

## Repeatable Installed Check

`make headless-workflow-check` accepts explicit `HEADLESS_CORE`,
`HEADLESS_SERVER`, `HEADLESS_OBSERVER`, a new `HEADLESS_REPORT`, and
`HEADLESS_INSTALLED_PREFIX`. The installed mode requires agent guidance and
the generic profile from that prefix, with no source-checkout fallback.
`HEADLESS_ROLLBACK_SERVER` optionally names the reviewed previous test control.
This target resets only the dedicated Shin development test schema; it must
never be pointed at a product database. The existing stand guard serializes it.

The fixture uses fresh scripted processes, not a fresh LLM agent, and keeps the
test driver in the checkout. Linux relocated packaging, offline template probes
and installed workflow/restore now pass on the provisioned stand; see
`tests/evidence/shin-r3-current-platform-gates-20261002.md` in the source checkout.
Source-tree-hidden execution, fresh model-agent proof and final release approval
remain separate checks.
