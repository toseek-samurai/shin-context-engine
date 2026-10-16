"""Scoped owner, topical qualification and excerpt-budget regressions."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


TOOL = str(Path(sys.argv.pop(1)).resolve())


class ScopedRetrievalTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="shin-scoped-")
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name)
        self.root = self.base / "root"
        self.out = self.base / "index"
        self.profiles = self.base / "profiles"
        self.profiles.mkdir()
        self.profile = {
            "schemaVersion": "shin.profile.v1", "id": "scoped",
            "sourceLabel": "Synthetic scoped source", "rootFiles": ["README.md"],
            "sourceExtensions": [".md"], "agentStartPaths": ["README.md"],
            "sourceFactRoots": ["apps"], "sourceFactExtensions": [".rsx", ".json"],
            "sourceFactExtractors": ["retool"],
            "sourceFactAttributeKeys": ["label", "hidden"],
            "compoundQueryFirstTermScope": True,
            "compoundQueryPreferredStems": ["main", "functions", "sidebar"],
            "defaultTopic": "fixture", "evalCases": [], "topicRules": [],
        }
        self.write("README.md", "# Scoped fixture\n")

    def write(self, path, content):
        target = self.root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(content)

    def invoke(self, *args):
        process = subprocess.run([
            TOOL, "--profile", "scoped", "--profile-dir", str(self.profiles),
            "--root", str(self.root), "--out-dir", str(self.out), *args,
        ], capture_output=True, text=True, timeout=60)
        self.assertEqual(process.returncode, 0, process.stderr or process.stdout)
        return json.loads(process.stdout)

    def build(self):
        (self.profiles / "scoped.json").write_text(json.dumps(self.profile))
        self.invoke("build")

    def brief(self, query, limit=8, per_doc=1):
        return self.invoke("brief", query, "--limit", str(limit), "--per-doc",
                           str(per_doc), "--require-current")

    def multipage(self, prefix="apps/admin/AtlasPanel"):
        self.write(prefix + "/metadata.json", "{}\n")
        self.write(prefix + "/.positions/.Summary.positions.json", "{}\n")
        self.write(prefix + "/main.rsx", '<App>\n<Include src="./functions.rsx" />\n'
                   '<Include src="./src/Summary.rsx" />\n'
                   '<Include src="./sidebar.rsx" />\n</App>\n')
        self.write(prefix + "/functions.rsx", '<GlobalFunctions>\n'
                   '<State id="Roles" value="[]" />\n</GlobalFunctions>\n')
        self.write(prefix + "/sidebar.rsx", '<SidebarFrame id="Side">\n'
                   '<Include src="./src/LocalMenu.rsx" />\n</SidebarFrame>\n')
        self.write(prefix + "/src/Summary.rsx", '<Screen id="Summary">\n'
                   '<State id="Account" value="{}" label="account" />\n</Screen>\n')
        self.write(prefix + "/src/LocalMenu.rsx", '<Navigation id="LocalMenu">\n'
                   '<Option id="Overview" label="Overview" />\n</Navigation>\n')

    def test_source_owner_wins_over_document_reference_and_keeps_multipage_owners(self):
        self.multipage()
        self.write("docs/apps/admin/AtlasPanel/README.md", "# AtlasPanel\n\n"
                   "operator application pages navigation account roles resources history\n")
        self.profile["aliases"] = [
            {"term": "application", "expands": ["app-root"]},
            {"term": "pages", "expands": ["screen"]},
        ]
        self.build()
        result = self.brief("AtlasPanel operator application pages navigation account roles resources history")
        self.assertEqual(result["retrieval_mode"], "directory-scoped")
        paths = {row["path"] for row in result["ranked_paths"]}
        self.assertTrue({
            "apps/admin/AtlasPanel/main.rsx", "apps/admin/AtlasPanel/functions.rsx", "apps/admin/AtlasPanel/sidebar.rsx",
            "apps/admin/AtlasPanel/src/Summary.rsx", "apps/admin/AtlasPanel/src/LocalMenu.rsx",
        }.issubset(paths))
        self.assertTrue(all(path.startswith("apps/admin/AtlasPanel/") for path in paths))

    def test_two_real_source_owners_fail_closed(self):
        self.multipage("apps/one/AtlasPanel")
        self.multipage("apps/two/AtlasPanel")
        self.write("docs/apps/admin/AtlasPanel/README.md", "# AtlasPanel\n\n"
                   "operator application pages navigation account roles resources history\n")
        self.build()
        result = self.brief("AtlasPanel operator application pages navigation account roles resources history")
        self.assertTrue(result["query_diagnostics"]["directory_owner_scope_used"])
        self.assertEqual(result["ranked_paths"], [])
        self.assertEqual(result["matched_facts"], 0)

    def test_path_and_generated_id_are_not_topical_qualification(self):
        self.multipage()
        self.write("apps/admin/AtlasPanel/account/Neutral.rsx", "\n".join(
            f'<Text id="Neutral{index}" />' for index in range(80)) + "\n")
        self.build()
        result = self.brief("apps/admin/AtlasPanel account", limit=20)
        self.assertNotIn("apps/admin/AtlasPanel/account/Neutral.rsx", result["related_documents"])
        self.assertLess(result["matched_facts"], 20)
        self.assertIn("apps/admin/AtlasPanel/src/Summary.rsx", result["related_documents"])

    def test_scope_alias_in_path_does_not_admit_every_row(self):
        self.multipage()
        self.write("apps/admin/AtlasPanel/src/Unlinked.rsx", "\n".join(
            f'<Text id="Neutral{index}" />' for index in range(80)) + "\n")
        self.profile["aliases"] = [{"term": "sections", "expands": [".rsx"]}]
        self.build()
        result = self.brief("apps/admin/AtlasPanel sections", limit=20)
        self.assertNotIn("apps/admin/AtlasPanel/src/Unlinked.rsx", result["related_documents"])
        self.assertLess(result["matched_facts"], 20)
        self.assertIn("apps/admin/AtlasPanel/main.rsx", result["related_documents"])

    def test_repeated_scope_alias_is_not_an_additional_topic(self):
        self.multipage()
        self.write("apps/admin/AtlasPanel/src/Unlinked.rsx", "\n".join(
            f'<Text id="Neutral{index}" />' for index in range(80)) + "\n")
        self.profile["aliases"] = [{"term": "account", "expands": ["AtlasPanel"]}]
        self.build()
        result = self.brief("apps/admin/AtlasPanel account", limit=20)
        self.assertNotIn("apps/admin/AtlasPanel/src/Unlinked.rsx", result["related_documents"])
        self.assertLess(result["matched_facts"], 20)

    def test_identity_metadata_key_is_not_a_topic(self):
        self.multipage()
        self.write("apps/admin/AtlasPanel/src/Unlinked.rsx", "\n".join(
            f'<Text id="Neutral{index}" />' for index in range(80)) + "\n")
        self.profile["aliases"] = [{"term": "application", "expands": ["app"]}]
        self.build()
        result = self.brief("apps/admin/AtlasPanel application", limit=20)
        self.assertNotIn("apps/admin/AtlasPanel/src/Unlinked.rsx", result["related_documents"])
        self.assertLess(result["matched_facts"], 20)

    def test_document_coverage_precedes_trimming_without_changing_hit_coverage(self):
        self.profile["compoundQueryPreferredStems"] = []
        self.write("apps/admin/AtlasPanel/main.rsx", '<App />\n')
        self.write("apps/admin/AtlasPanel/src/Dense.rsx", '<Text id="First" label="alpha beta" />\n'
                   '<Text id="Second" label="gamma delta" />\n')
        self.write("apps/admin/AtlasPanel/src/Sparse.rsx", '<Text id="Third" label="alpha beta gamma" />\n')
        self.build()
        query = "apps/admin/AtlasPanel alpha beta gamma delta"
        narrow = self.brief(query, limit=1)
        wide = self.brief(query, limit=1, per_doc=8)
        self.assertEqual(narrow["ranked_paths"][0]["path"], "apps/admin/AtlasPanel/src/Dense.rsx")
        self.assertEqual(wide["ranked_paths"][0]["path"], "apps/admin/AtlasPanel/src/Dense.rsx")
        hit = narrow["facts"][0]
        self.assertEqual(hit["score_components"]["scope_coverage_boost"], 4000)
        self.assertEqual(wide["facts"][0]["score_components"]["scope_coverage_boost"], 4000)
        self.assertLess(len(hit["matched_group_indices"]), 5)
        self.assertGreater(narrow["query_coverage"]["uncovered_groups"], 0)
        self.assertEqual(narrow["matched_facts"], wide["matched_facts"])

    def test_qualified_record_count_is_not_the_delivery_limit(self):
        self.write("apps/admin/AtlasPanel/main.rsx", '<App />\n')
        self.write("apps/admin/AtlasPanel/src/Records.rsx", "\n".join(
            f'<Text id="Account{index}" label="account" />' for index in range(40)) + "\n")
        self.build()
        narrow = self.brief("apps/admin/AtlasPanel account", limit=1)
        wide = self.brief("apps/admin/AtlasPanel account", limit=20, per_doc=8)
        self.assertGreaterEqual(narrow["matched_facts"], 40)
        self.assertEqual(narrow["matched_facts"], wide["matched_facts"])


if __name__ == "__main__":
    unittest.main()
