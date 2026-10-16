"""Private, non-destructive disposition of a bounded offline Observer queue."""

import hashlib
import json
import stat

from integration_test import Fixture, IntegrationFailure, tree_metadata


def check_outbox_archive(suite):
    fixture = Fixture(suite.fixture_root, "outbox-archive")
    config = fixture.config_data()
    config["outbox"] = {"enabled": True, "max_entries": 2, "max_bytes": 65536}
    fixture.write_config(config)
    for index in range(4):
        fixture.write_source(f"Distinct archived observation {index}.\n")
        suite.build_core(fixture)
        suite.observer_command(fixture, "sample")
    state_file = fixture.state / "state.json"
    state = json.loads(state_file.read_text())
    queue = state["outbox"]
    if len(queue["entries"]) != 2 or queue["rejected_observations"] != 2:
        raise IntegrationFailure("archive fixture did not saturate without dropping accepted entries")
    suite.core_command(fixture, "brief", "archived observation", "--require-current")
    archives = fixture.base / "archives"
    archives.mkdir(mode=0o700)
    archive = archives / "pending.json"

    def image():
        return (state_file.read_bytes(), tree_metadata(fixture.state),
                tree_metadata(fixture.root), tree_metadata(fixture.out))

    def command(name, path=archive, error=None):
        before = image()
        result, _ = suite.observer_command(fixture, name, "--archive", str(path), expect_success=error is None)
        if image() != before:
            raise IntegrationFailure("archive operation changed Observer state, source or index")
        if error is not None and result["error"]["code"] != error:
            raise IntegrationFailure("archive refusal used an unexpected reason: " + result["error"]["code"])
        return result

    exported = command("outbox-export")
    raw = archive.read_bytes()
    if raw != state_file.read_bytes() or stat.S_IMODE(archive.stat().st_mode) != 0o600 or len(raw) > 256 * 1024:
        raise IntegrationFailure("archive did not preserve the bounded private committed state")
    for result in (exported, command("outbox-export"), command("outbox-verify")):
        if (result["schema_version"] != "shin.observer-outbox-archive.v1" or
                result["archive_sha256"] != hashlib.sha256(raw).hexdigest() or
                result["stream_id"] != queue["stream_id"] or result["pending_entries"] != 2 or
                result["rejected_observations"] != 2 or result["acknowledged_sequence"] != 0 or
                result["current_state_verified"] is not True or result["observer_state_mutated"] is not False or
                result["delivery_performed"] is not False or result["external_archive_durability_verified"] is not False):
            raise IntegrationFailure("archive receipt lost identity or overclaimed durability/delivery")
    if exported["archive_created"] is not True or command("outbox-export")["archive_created"] is not False:
        raise IntegrationFailure("exact export retry did not preserve its original file")

    pending = fixture.state / "state.pending"
    pending.write_bytes(b'{"uncommitted":')
    pending.chmod(0o600)
    command("outbox-verify")
    if pending.read_bytes() != b'{"uncommitted":':
        raise IntegrationFailure("archive verification recovered uncommitted state")
    pending.unlink()
    archive.write_bytes(raw[: len(raw) // 2])
    command("outbox-verify", error="invalid-json")
    command("outbox-export", error="archive-conflict")
    if archive.read_bytes() != raw[: len(raw) // 2]:
        raise IntegrationFailure("incomplete archive was overwritten")
    archive.write_bytes(raw)

    tampered = json.loads(raw)
    tampered["outbox"]["entries"][0]["payload"]["project_id"] = "foreign-project"
    archive.write_text(json.dumps(tampered, sort_keys=True, separators=(",", ":")))
    command("outbox-verify", error="invalid-outbox")
    archive.write_bytes(raw)

    numeric_alias = json.loads(raw)
    numeric_alias["outbox"]["delivery"]["last_duration_ms"] = 0
    archive.write_text(json.dumps(numeric_alias, sort_keys=True, separators=(",", ":")))
    command("outbox-verify", error="archive-stale")
    archive.write_bytes(raw)

    for root in (fixture.root, fixture.out, fixture.profiles, fixture.state):
        command("outbox-export", root / "forbidden.json", error="invalid-archive")
    command("outbox-export", "relative.json", error="invalid-archive")
    for name in ("outbox-export", "outbox-verify"):
        alias = archives / (name + "-alias.json")
        alias.symlink_to(archive)
        command(name, alias, error="unsafe-file")
    archive.chmod(0o644)
    command("outbox-verify", error="unsafe-file")
    command("outbox-export", error="unsafe-file")
    archive.chmod(0o600)
    archives.chmod(0o755)
    command("outbox-verify", error="unsafe-directory")
    command("outbox-export", error="unsafe-directory")
    archives.chmod(0o700)

    lock = fixture.state / "observer.lock"
    lock.unlink()
    command("outbox-verify", error="writer-unavailable")
    if lock.exists():
        raise IntegrationFailure("archive verification created a missing writer lock")
    lock.touch(mode=0o600)
    watch = suite.start_watch(fixture)
    suite.wait_for_lifecycle(fixture, {"waiting"}, watch)
    for name in ("outbox-export", "outbox-verify"):
        result, _ = suite.observer_command(fixture, name, "--archive", str(archive), expect_success=False)
        if result["error"]["code"] != "writer-busy" or archive.read_bytes() != raw:
            raise IntegrationFailure("active writer did not fence archive maintenance")
    suite.finish_watch(watch)
    command("outbox-verify", error="archive-stale")
    command("outbox-export", error="archive-conflict")
    latest = archives / "latest.json"
    command("outbox-export", latest)
    command("outbox-verify", latest)
    if json.loads(latest.read_text())["outbox"]["entries"] != queue["entries"]:
        raise IntegrationFailure("saturated/restarted archive lost accepted packets")
