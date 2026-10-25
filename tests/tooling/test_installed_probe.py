#!/usr/bin/env python3
import unittest
from probe_installed_local_service import locator


class LocatorTests(unittest.TestCase):
    def test_source_locator_precedes_documents(self):
        self.assertEqual(locator({
            'source_context': 'SHIN {"locator_columns":["path"]}\nSOURCE 1 ["src/a.cpp"]',
            'document_context': {'evidence': [{'path': 'docs/a.md'}]},
        }), ('src/a.cpp', 'source'))

    def test_document_only_project(self):
        self.assertEqual(locator({
            'source_context': 'SHIN {"locator_columns":["path"]}',
            'document_context': {'evidence': [{'path': 'docs/contract.json'}]},
        }), ('docs/contract.json', 'document'))

    def test_no_evidence_is_not_success(self):
        with self.assertRaisesRegex(RuntimeError, 'no-source-or-document-locator'):
            locator({'source_context': '', 'document_context': {'evidence': []}})


if __name__ == '__main__':
    unittest.main()
