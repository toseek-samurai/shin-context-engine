# Native Windows source status

Windows support is an additional platform implementation of Shin. It uses the
same Core, profiles, generated-index contracts, Observer and MCP protocol as
macOS and Linux. It does not replace their implementations or qualification.

This is a development source route targeting **Windows x64 on local NTFS**.
The published dev.54 release has no Windows download. Unreleased source adds
guarded ZIP/MSI publication alongside macOS; only a published Release containing
the MSI/ZIP and their checksums is a downloadable Windows preview.

## Current scope

| Area | Native Windows source behavior |
| --- | --- |
| Core and retrieval | Indexing, source discovery and snapshot-bound reads use the shared engine. Windows smoke checks cover a selected native subset of the Core suite. |
| Local Observer, CLI and MCP | Native processes and authenticated named pipes; user runtimes require unelevated tokens. Explicit project/root authority and output bounds remain enforced. |
| Storage and workers | Windows file identities, owner-private ACLs, atomic replacement and bounded Job Objects. UNC paths and reparse-point traversal are refused. |
| Monitor | Win32/DX11 host with the shared EN/RU presentation and diagnostics. Source render checks include documentation warnings in project lists and details. |
| Client configuration | `mcp-config` exports configuration for manual review. Automatic `client-connect` editing and graphical project onboarding are not enabled. |
| Terminal UI, usage and updating | `shin-ce stats`, automatic client-history collection and in-app updating remain pending on Windows. Use the native Monitor or text/JSON status where applicable. |
| Distribution | ZIP/WiX MSI recipes and qualification tests exist; development presets leave packaging disabled. Installed lifecycle and exact-archive checks remain separate gates. |

Runtime binaries do not require WSL, Bash, Python, Node or a model service.
Python and the native compiler are development/test dependencies. WSL builds
exercise the Linux implementation and do not substitute for native Windows tests.

## Build and verification

Use the matching checkout's `docs/building.md` for PowerShell commands and full
prerequisites. The [repository build guide](https://github.com/toseek-samurai/shin-context-engine/blob/main/docs/building.md)
also describes the distinction between local and hosted-runner checks.

- `windows-native` builds Core and native Observer tools; its
  `windows-native-smoke` test preset selects native Core checks.
- `windows-desktop`, from `observer/`, builds Observer/CLI/MCP and Monitor;
  its full test preset runs all checks registered in that configuration.
- `windows-desktop-smoke` is the hosted-runner subset. It does not include
  interactive desktop, full user-service lifecycle or protected-image checks.
- `windows-kit` builds production binaries for ZIP/MSI in a separate `kit/`
  directory. It does not register source tests; first run `windows-desktop`.

Keep the checkout and an absolute external `SHIN_BUILD_ROOT` outside AppData
and TEMP. Full local tests need an unelevated interactive desktop, symlink
creation rights for refusal fixtures, and a protected CMake installation. Do not substitute an
elevated shell for the user-runtime checks. The source workflow uploads test
reports. Unreleased source also qualifies Windows packages before publication.

The shared refresh regressions cover initial bootstrap, refresh and cached
queries when documentation has a broken local link, warning retention and
clearing after repair. Only the known link-only bootstrap warning is tolerated;
permanent profiles and other safety, freshness and integrity failures remain
strict. Tests use independent disposable state rather than installed projects.

## Runtime and installation boundaries

A source build does not install Shin, register an SCM service or enroll a real
project. Development CLI use must select its own project and data home; a
project's sources and its private index/history must remain separate. Use the
same matching binaries and home for CLI, MCP and Monitor.

The Windows MSI design installs a machine-owned `ShinObserverHost` supervisor
under LocalSystem. That host starts separate unelevated runtimes for existing
and newly signed-in user sessions. Project observation, indexes and queries
belong to those user runtimes. The native service-definition command describes
this model; it does not prove that a matching service is installed or healthy.

Monitor starts separately. Closing its window does not stop an already running
Observer. Installation does not enroll projects or rewrite client configuration.
Project sources, indexes, history and credentials are outside the MSI payload
and must be preserved during maintenance and removal.

## Installer presentation checks

The source MSI wizard uses Shin branding, Segoe UI and separate welcome,
preview-notes, confirmation, progress and completion pages. The confirmation
shows the fixed protected Program Files destination. Repair and removal use
the standard WiX maintenance dialogs. These are development-source changes,
not evidence of a published or qualified Windows installer.

Use CPack 4.3+ and WiX 4 with matching UI/Util extensions. With the external
`SHIN_BUILD_ROOT` from the build guide still set, run from the checkout root:

```powershell
Push-Location observer
cmake --preset windows-kit
cmake --build --preset windows-kit
cpack --config "$env:SHIN_BUILD_ROOT/kit/CPackConfig.cmake" -C Release -G ZIP
cpack --config "$env:SHIN_BUILD_ROOT/kit/CPackConfig.cmake" -C Release -G WIX
$version = (Get-Content VERSION -Raw).Trim()
$zip = "$env:SHIN_BUILD_ROOT/kit/packages/shin-desktop-$version-windows-AMD64.zip"
$msi = "$env:SHIN_BUILD_ROOT/kit/packages/shin-desktop-$version-windows-AMD64.msi"
python -B tests/windows_kit_test.py $zip --msi $msi
Pop-Location
```

Run this archive check unelevated in an interactive session. It verifies both
package identities, hashes, runtime dependencies, relocated Monitor renders,
and init/query/stop using disposable project data. MSI inspection is read-only;
no installation actions execute. Stop on a failed command before proceeding.
Keep the exact archives and their `.sha256` files together.

For MSI-only metadata checks or visual dialog previews, from the checkout root:

```powershell
python observer/tests/windows_msi_test.py $msi --phase metadata
python observer/installer/preview_windows_msi.py $msi --dialog WelcomeDlg
```

Set `$msi` to the absolute package path. Repeat the preview for
`LicenseAgreementDlg`, `VerifyReadyDlg`, `ProgressDlg`, `ExitDialog`,
`MaintenanceWelcomeDlg`, `MaintenanceTypeDlg`, `FatalError` and `UserExit`.
The helper opens the MSI database read-only with `MsiEnableUIPreview` and
automatically closes after 60 seconds (`--seconds` changes the interval).
It does not run installation actions. Preview properties are not populated by
costing or an installation session, so the destination and some conditional
controls may be absent; check their actual behavior in a disposable installed
lifecycle test. Metadata checks also verify wizard navigation, note acceptance,
the protected root and the Monitor launch action.

Artwork is checked in under `observer/installer/assets`; normal packaging
does not regenerate it. To refresh it from the project logo on Windows, run
`python observer/installer/generate_windows_branding.py` with Pillow installed.
Keep the WiX bitmap canvases at 493 x 58 and 493 x 312 pixels.

## Package publication and remaining gates

In unreleased source, the [unified CI](../.github/workflows/ci.yml) requires both
Windows source jobs and all four Linux/macOS jobs before publishing either
platform. The Windows desktop job builds ZIP/MSI with pinned CPack 4.3.5 and WiX
4.0.6. Qualification checks the compiled MSI identity, UI, upgrade bounds and
SCM actions, compares its extracted payload byte-for-byte with the ZIP, and runs
native Monitor rendering plus init/query/stop from the relocated ZIP under a
standard user. A receipt binds these checks to the commit, run attempt and exact
asset hashes. Failed or missing Windows proof prevents publication of the release.

`qualify_windows.ps1` creates a temporary standard account only on disposable
GitHub-hosted VMs, then removes the task and account/profile. It does not change
UAC or install an MSI. The runtime test uses a private temporary project and home;
Monitor renders with software D3D11/WARP on the task's noninteractive desktop.

Real interactive-session installation, repair, upgrade, downgrade refusal,
uninstall/reinstall and data/history preservation were rehearsed for the dev.54
packaging policy. That is separate installed evidence, not a claim that every
hosted package check repeats the lifecycle rehearsal. Changes to installer or
SCM behavior still need a new installed rehearsal, including multi-user and
rollback scenarios where affected. TUI, automatic client setup/usage collection
and in-app Windows updating remain pending.

Linux/macOS gates, Windows source checks, exact-archive checks and installed
service checks retain separate proof boundaries. Local binaries never substitute
for the CI artifact that is published.

## MSI version policy

Windows Installer compares only three numeric fields, bounded by
`255.255.65535`; a fourth field is ignored. See Microsoft's
[ProductVersion contract](https://learn.microsoft.com/en-us/windows/win32/msi/productversion).
The kit and UI retain the full Shin version. The internal MSI version is
`major.minor.(patch * 1000 + channelOffset)`:

| Shin version | MSI ProductVersion |
| --- | --- |
| `2.0.5-dev.53` | `2.0.5053` |
| `2.0.5-dev.54` | `2.0.5054` |
| `2.0.5-rc.0` | `2.0.5900` |
| `2.0.5` | `2.0.5999` |
| `2.0.6-dev.0` | `2.0.6000` |

Development serials occupy offsets 0–899; release candidates use 900–998;
stable uses 999. This policy supports major/minor 0–255, patch 0–64,
dev serial 0–899 and rc serial 0–98. Packaging fails for unsupported values or
noncanonical versions instead of truncating, wrapping or reusing an identity.
These are MSI packaging bounds, not runtime or indexing limits.

The existing UpgradeCode stays constant. ProductCode is a deterministic UUIDv5
of `windows-x64:<full Shin version>` in that UpgradeCode namespace: the same
version has the same product identity; a new version has a different one.
Compiled-MSI checks verify these values, older-version removal bounds and the
downgrade refusal. The ZIP and MSI must describe the same full version.
Never replace a published archive with changed bytes under the same version.
Packaging retains CPack's installed WiX 4 template and disables its default
same-version replacement. If the expected template fragment changes, packaging
fails for review. Normal maintenance of the same ProductCode remains separate
from replacing one version with another.
