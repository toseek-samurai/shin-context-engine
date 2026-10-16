"""Project-separated workspace registration retries on the dedicated HTTP stand."""
from concurrent.futures import ThreadPoolExecutor
from threading import Barrier

from publication_test_support import require


def check_workspace_request_keys(suite):
    projects = ("workspace-keys-a", "workspace-keys-b")
    roots = [suite.state_root / "fixtures" / "roots" / project for project in projects]
    payloads = []
    for project, root in zip(projects, roots):
        root.mkdir(exist_ok=True)
        suite.assert_register(project, project + "-create")
        payloads.append({**suite.workspace_payload("primary", root), "display_name": "Same checkout"})
    barrier = Barrier(4)

    def deliver(index):
        barrier.wait(timeout=5)
        return index, suite.post_workspace(projects[index], payloads[index], "same-workspace-key")

    with ThreadPoolExecutor(max_workers=4) as pool:
        results = list(pool.map(deliver, (0, 1, 0, 1)))
    for index, project in enumerate(projects):
        replies = [reply for selected, reply in results if selected == index]
        require(sorted(status for status, _ in replies) == [200, 201], "cross-project key collision or duplicate creation")
        records = []
        for status, response in replies:
            result = response["result"]
            require(result["replayed"] == (status == 200), "workspace replay marker differs")
            suite.assert_workspace(result["workspace"], project, "primary", roots[index])
            records.append(result["workspace"])
        require(records[0] == records[1], "duplicate delivery returned a different workspace")
        for table in ("workspaces", "workspace_requests", "audit_events"):
            require(suite.count(table, f"project_id='{project}'" +
                                (" AND workspace_id='primary'" if table == "audit_events" else "")) == 1,
                    "duplicate registration changed row/receipt/audit count")
        status, reply = suite.post_workspace(project, {**payloads[index], "display_name": "Changed"}, "same-workspace-key")
        require(status == 409 and reply["error"]["code"] == "idempotency-conflict", "same-domain changed body replayed")
        status, _ = suite.post_workspace(project, payloads[index], "same-workspace-key", role="reader")
        require(status == 403, "reader received a registration receipt")

    # Replay is a historical acknowledgement and must not probe a removed root.
    suite.stop_server(expect_clean=True)
    suite.start_server()
    for index, root in enumerate(roots):
        moved = root.with_name(root.name + "-moved")
        root.rename(moved)
        try:
            status, response = suite.post_workspace(projects[index], payloads[index], "same-workspace-key")
            require(status == 200 and response["result"]["replayed"], "reopened scoped receipt did not replay")
            require(response["result"]["workspace"]["project_id"] == projects[index], "replay crossed project")
        finally:
            moved.rename(root)
