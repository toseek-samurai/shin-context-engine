#!/usr/bin/env python3
"""Explicit one-way import of a completed pilot, never a query-time dependency."""

import argparse
import importlib.util
import json
from pathlib import Path
import sys

from run_local_semantic_pilot import fingerprint, load_completed_run, load_input
from semantic_source_hop import read_bound_links

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("semantic_entry", ROOT / "tools/shin_semantic.py")
entry = importlib.util.module_from_spec(spec)
spec.loader.exec_module(entry)
from shin_semantic.artifact import Artifact, encode, publication_id, root_identity, sha
from shin_semantic.model import MODEL


def write(path, content):
    with path.open("xb") as stream:
        stream.write(content)
    path.chmod(0o600)


def bind_current_profile(output, profile):
    config = json.loads((output / "route.json").read_text())
    if profile.name != config["profile"] + ".json" or fingerprint(profile) != config["profile_sha256"]:
        raise ValueError("Current profile must exactly match the frozen profile bytes")
    config["profile_path"] = str(profile)
    Artifact(Path(config["artifact_dir"]), config["artifact_sha256"], config)
    write(output / "current-route.json", encode(config) + b"\n")
    return {"profile_sha256": config["profile_sha256"], "artifact_sha256": config["artifact_sha256"],
            "current_profile_bound": True, "source_freshness": "not-assessed-requires-core-admission"}


def materialize(run, publications, profiles, project_id, output, core, model_dir):
    import numpy as np

    report, _, manifest, source = load_completed_run(run)
    _, _, policy, all_units = load_input(source)
    projects = [row for row in manifest["projects"] if row["id"] == project_id]
    if len(projects) != 1 or manifest.get("projection_version") != "core-bound-range-v1":
        raise ValueError("One bound parent-body project is required")
    project = projects[0]
    if (fingerprint(core) != manifest["binary"]["sha256"] or report["runtime_versions"] != MODEL["runtime"]
            or report["model_manifest_sha256"] != MODEL["manifest_sha256"] or policy["dimensions"] != 384):
        raise ValueError("Importer cannot change the model, Core or runtime binding")
    generated = publications / project_id / "index/docs/kb/generated"
    _, links = read_bound_links(generated, project["publication_sha256"])
    publication = (generated / "manifest.json").read_bytes()
    published = json.loads(publication)
    profile = profiles / project_id / (published["profile"] + ".json")
    if fingerprint(profile) != project["profile_sha256"]:
        raise ValueError("Frozen profile differs")
    indexes = [i for i, row in enumerate(all_units) if row["project"] == project_id]
    units = []
    for index in indexes:
        original = all_units[index]
        if original.get("unit_kind") == "semantic-subrange" or original["source_sha256"] != project["sources"][original["path"]]["sha256"]:
            raise ValueError("Unbound or unsupported unit")
        unit = {key: original[key] for key in ("project", "path", "start_line", "end_line", "source_sha256", "record_ids")}
        unit["text_sha256"] = sha(original["text"].encode())
        units.append(unit)
    matrix = np.load(run / "vectors.npy", mmap_mode="r", allow_pickle=False)
    if matrix.shape != (len(all_units), 384) or matrix.dtype != np.float32:
        raise ValueError("Pilot matrix layout differs")
    output.mkdir(parents=True, exist_ok=False, mode=0o700)
    artifact = output / "artifact"
    artifact.mkdir(mode=0o700)
    write(artifact / "publication.json", publication)
    link_bytes = (generated / "links.jsonl").read_bytes()
    if sha(link_bytes) != links["sha256"]:
        raise ValueError("Published links changed during import")
    write(artifact / "links.jsonl", link_bytes)
    write(artifact / "units.jsonl", b"".join(encode(unit) + b"\n" for unit in units))
    np.save(artifact / "vectors.npy", matrix[indexes], allow_pickle=False)
    (artifact / "vectors.npy").chmod(0o600)
    binding = {"project": project_id, "root": project["root"], "root_identity": root_identity(Path(project["root"])),
               "profile": published["profile"], "profile_sha256": project["profile_sha256"],
               "profile_config_hash": published["profile_config_hash"], "snapshot_id": publication_id(publication),
               "publication_sha256": project["publication_sha256"], "generator": published["generator_version"]}
    descriptor = {"schema_version": "shin.semantic-artifact.v1", "binding": binding, "model": MODEL,
                  "projection": "core-bound-range-v1", "selection": "code-anchor-source-resolution-v1", "units": len(units),
                  "files": {name: {"sha256": fingerprint(artifact / name), "bytes": (artifact / name).stat().st_size}
                            for name in ("publication.json", "links.jsonl", "units.jsonl", "vectors.npy")}}
    data = encode(descriptor) + b"\n"
    write(artifact / "manifest.json", data)
    config = {"schema_version": "shin.semantic-route.v1", **{key: binding[key] for key in
              ("project", "root", "root_identity", "profile", "profile_sha256")},
              "core_binary": str(core), "core_sha256": fingerprint(core), "profile_path": str(profile),
              "out_dir": str(publications / project_id / "index"), "artifact_dir": str(artifact),
              "artifact_sha256": sha(data), "model_dir": str(model_dir)}
    Artifact(artifact, sha(data), config)
    write(output / "route.json", encode(config) + b"\n")
    receipt = {"schema_version": "shin.semantic-pilot-import.v1", "project": project_id,
               "artifact_sha256": sha(data), "input_manifest_sha256": fingerprint(source / "input-manifest.json"),
               "run_report_sha256": fingerprint(run / "report.json"), "original_indexes": indexes,
               "document_vectors_computed": 0, "units": len(units),
               "bytes": sum(row["bytes"] for row in descriptor["files"].values()) + len(data),
               "source_freshness": "not-assessed-import-is-not-currentness", "query_time_eval_files_required": False}
    write(output / "import.json", encode(receipt) + b"\n")
    return receipt


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for field in ("run", "publications", "profiles", "core", "model-dir", "bind-current-profile"):
        parser.add_argument("--" + field, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--project")
    args = parser.parse_args()
    if args.bind_current_profile is not None:
        result = bind_current_profile(args.output.resolve(), args.bind_current_profile.resolve())
    else:
        if not all((args.run, args.publications, args.profiles, args.project, args.core, args.model_dir)):
            parser.error("import requires --run, --publications, --profiles, --project, --core and --model-dir")
        result = materialize(args.run.resolve(), args.publications.resolve(), args.profiles.resolve(), args.project, args.output.resolve(),
                             args.core.resolve(), args.model_dir.resolve())
    print(json.dumps({k: v for k, v in result.items() if k != "original_indexes"}, indent=2))
