"""Declared history metadata fixtures, not additional qualified source artifacts."""
import json

from identity_directory_checks import require


def check_scoped_history(suite):
    def original(project):
        return int(suite.scalar("SELECT p.id FROM shin_server.publication_proposals p "
            "JOIN shin_server.publication_resolutions r ON r.proposal_id=p.id AND r.decision='accept' "
            f"WHERE p.project_id='{project}' AND p.publication_id='scoped-publication' ORDER BY p.id LIMIT 1"))

    def copy_row(project, key, *, workspace="primary", base="none", base_workspace="primary"):
        base_revision, preparation, withdrawal = "0", "NULL", "NULL"
        origin = "NULL,NULL,NULL,NULL,NULL"
        if base == "active":
            base_revision, preparation = "1", "p.preparation_id"
            origin = f"p.source_route_id,'{base_workspace}',p.source_host_id,p.profile_id,p.route_fingerprint"
        elif base in ("withdrawn", "missing"):
            base_revision = "2"
            withdrawal = ("(SELECT id FROM shin_server.publication_withdrawals w WHERE w.project_id=p.project_id "
                          "AND w.publication_id=p.publication_id AND w.revision=2)") if base == "withdrawn" else "9223372036854775806"
        return int(suite.scalar("INSERT INTO shin_server.publication_proposals(project_id,publication_id,preparation_id,"
            "base_revision,base_preparation_id,submitted_by,idempotency_key,request_hash,source_route_id,workspace_id,"
            "source_host_id,profile_id,route_fingerprint,base_withdrawal_id,history_epoch,base_source_route_id,"
            "base_workspace_id,base_source_host_id,base_profile_id,base_route_fingerprint) "
            f"SELECT p.project_id,p.publication_id,p.preparation_id,{base_revision},{preparation},p.submitted_by,"
            f"'{key}',p.request_hash,p.source_route_id,'{workspace}',p.source_host_id,p.profile_id,p.route_fingerprint,"
            f"{withdrawal},p.history_epoch,{origin} FROM shin_server.publication_proposals p WHERE p.id={original(project)} RETURNING id"))

    fixture = {"private_seed": original("visible"), "shared_seed": original("other")}
    fixture["hidden_base"] = copy_row("visible", "history-hidden-base", base="active", base_workspace="secondary")
    fixture["unknown_base"] = copy_row("visible", "history-unknown-base", base="missing")
    fixture["foreign_candidate"] = copy_row("visible", "history-foreign-candidate", workspace="secondary")
    fixture["visible_one"] = copy_row("visible", "history-visible-one")
    fixture["visible_two"] = copy_row("visible", "history-visible-two", base="active")
    fixture["withdrawn_base"] = copy_row("other", "history-withdrawn-base", base="withdrawn")
    result = suite.run_process([str(suite.binary.parent / "shin-server-scoped-retrieval-probe"),
        str(suite.runtime_config_path), json.dumps(fixture)], "native scoped history", timeout=30.0)
    receipt = json.loads(result.stdout)
    require(receipt.get("ok") is True and receipt.get("scoped_history") is True and receipt.get("checks", 0) >= 20,
            "scoped history receipt is incomplete")
