"""Read-only bounded historical observations for the local dashboard."""

import os
from pathlib import Path

from shin_portfolio import private_file, report as portfolio_report, SLUG
from shin_semantic.artifact import canonical, decode, require
from shin_semantic.telemetry import directory, report as journal_report


class Dashboard:
    def __init__(self, config_path):
        path = canonical(str(config_path))
        with directory(path.parent) as parent:
            fd = os.open(path.name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=parent)
            with os.fdopen(fd, "rb") as stream:
                private_file(stream.fileno())
                raw = stream.read(65537)
        require(len(raw) <= 65536, "dashboard-config-budget")
        config = decode(raw)
        require(set(config) == {"schema_version", "projects", "assessments", "journals"}
                and config["schema_version"] == "shin.dashboard-config.v1", "invalid-dashboard-config")
        require(isinstance(config["projects"], list) and 1 <= len(config["projects"]) <= 24, "dashboard-project-budget")
        self.projects, self.routes, self.query_projects = [], {}, {}
        for p in config["projects"]:
            require(set(p) == {"id", "name", "routes", "query_projects"} and SLUG.fullmatch(p["id"]), "invalid-dashboard-project")
            require(isinstance(p["name"], str) and 1 <= len(p["name"]) <= 80
                    and not any(ord(c) < 32 for c in p["name"]), "invalid-dashboard-name")
            require(p["id"] not in {r["id"] for r in self.projects}, "duplicate-dashboard-project")
            self.projects.append({"id": p["id"], "name": p["name"]})
            for field, mapping in (("routes", self.routes), ("query_projects", self.query_projects)):
                require(isinstance(p[field], list) and len(p[field]) <= 24, "dashboard-binding-budget")
                for identity in p[field]:
                    require(isinstance(identity, str) and SLUG.fullmatch(identity) and identity not in mapping,
                            "ambiguous-dashboard-binding")
                    mapping[identity] = p["id"]
        self.assessments = self._paths(config["assessments"], 8)
        self.journals = self._paths(config["journals"], 4)
        require(self.assessments or self.journals, "dashboard-source-required")

    @staticmethod
    def _paths(values, maximum):
        require(isinstance(values, list) and len(values) <= maximum, "dashboard-source-budget")
        paths = [canonical(value) for value in values]
        require(len(set(paths)) == len(paths), "duplicate-dashboard-source")
        # A configured future run may not exist yet. Reads report it unavailable.
        return paths

    def report(self):
        result = {"schema_version": "shin.dashboard.v1", "projects": self.projects, "checks": [], "events": [],
                  "queries": [], "sources": [], "query_coverage": [], "scope": "recorded-observations-not-live-service-status"}
        seen_runs, seen_queries, seen_epochs, covered = set(), set(), set(), set()
        for index, path in enumerate(self.assessments):
            source = {"id": f"assessment-{index + 1}", "kind": "assessment", "state": "unavailable"}
            try:
                batch = portfolio_report(path)
                require(batch["run_id"] not in seen_runs, "duplicate-dashboard-run")
                seen_runs.add(batch["run_id"])
                source.update(state="available", run_id=batch["run_id"], status=batch["status"],
                              updated_at_unix_ms=batch["updated_at_unix_ms"])
                for p in batch["projects"]:
                    project = self.routes.get(p["id"])
                    if project is None or p["status"] in {"pending", "running"}:
                        continue
                    result["checks"].append({"project": project, "route": p["id"], "run_id": batch["run_id"],
                        "captured_at_unix_ms": p["captured_at_unix_ms"], "status": p["status"],
                        "comparable": p["comparable"], "live": p["live"], "lanes": p["lanes"], "engines": batch["engines"]})
                for e in batch["events"]:
                    if e["project"] in self.routes:
                        result["events"].append({**e, "route": e["project"], "project": self.routes[e["project"]],
                            "run_id": batch["run_id"], "updated_at_unix_ms": batch["updated_at_unix_ms"]})
            except Exception:
                pass
            result["sources"].append(source)
        for index, path in enumerate(self.journals):
            source = {"id": f"journal-{index + 1}", "kind": "journal", "state": "unavailable"}
            try:
                journal = journal_report(path, recent_limit=256)
                require(journal["epoch"] not in seen_epochs, "duplicate-dashboard-journal")
                seen_epochs.add(journal["epoch"])
                source.update(state="available", retained=journal["retained"], evicted=journal["evicted"], epoch=journal["epoch"])
                for q in journal["recent"]:
                    project = self.query_projects.get(q["project"])
                    if project is None:
                        continue
                    covered.add(project)
                    identity = (journal["epoch"], q["attempt_id"])
                    if identity in seen_queries:
                        continue
                    seen_queries.add(identity)
                    fields = ("attempt_id", "observed_at_unix_ms", "elapsed_ns", "phase_ns", "profile", "route_key",
                              "snapshot_id", "core_sha256", "client_files_sha256", "ok", "semantic_status")
                    result["queries"].append({**{k: q.get(k) for k in fields}, "project": project,
                        "recorded_project": q["project"], "epoch": journal["epoch"]})
            except Exception:
                pass
            result["sources"].append(source)
        result["query_coverage"] = sorted(covered)
        result["checks"].sort(key=lambda row: (row["captured_at_unix_ms"], row["project"]))
        result["queries"].sort(key=lambda row: (row["observed_at_unix_ms"], row["attempt_id"]))
        return result
