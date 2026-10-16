#!/usr/bin/env python3
"""Reference integrity, library tokenization and bounded candidate controls."""

import time
import unittest

from run_standard_retrieval import BM25, rank


def unit(path, text):
    return {"path": path, "text": text}


class BM25Tests(unittest.TestCase):
    def engine(self, rows):
        engine = BM25(rows, time.monotonic() + 10)
        self.addCleanup(engine.close)
        return engine

    def test_library_idf_puts_rare_term_first(self):
        engine = self.engine([unit("common", "retry repeat") for _ in range(10)] +
                             [unit("specific", "retry receipt")])
        self.assertEqual(engine.search("retry receipt")[0]["index"], 10)

    def test_sqlite_handles_cyrillic_and_separators(self):
        engine = self.engine([unit("one", "проверка retry_pending"), unit("two", "unrelated")])
        self.assertEqual(engine.search("ПРОВЕРКА")[0]["index"], 0)
        self.assertEqual(engine.search("retry pending")[0]["index"], 0)

    def test_query_syntax_is_data_not_sql_or_fts_commands(self):
        engine = self.engine([unit("one", "retry"), unit("two", "document")])
        self.assertEqual(engine.search('"retry" OR NOT (') , engine.search("retry OR NOT"))
        engine.search("'; DROP TABLE documents; --")
        self.assertEqual(engine.search("retry")[0]["index"], 0)
        self.assertEqual(engine.search("!!!"), [])

    def test_tie_order_and_original_caps(self):
        engine = self.engine([unit("same", "retry") for _ in range(5)] +
                             [unit(str(i), "retry") for i in range(30)])
        hits = engine.search("retry")
        self.assertEqual(len(hits), 20)
        self.assertEqual([h["index"] for h in hits[:3]], [0, 1, 5])
        self.assertEqual(hits, engine.search("retry"))

    def test_invalid_query_and_no_match(self):
        engine = self.engine([unit("one", "retry")])
        for query in ("", "x\0", "x" * 4097, " ".join("word" + str(i) for i in range(65))):
            with self.assertRaises(ValueError):
                engine.search(query)
        self.assertEqual(engine.search("absent"), [])
        self.assertIsNone(rank([], {"absent"}))

    def test_queries_and_project_indexes_are_independent(self):
        first = self.engine([unit("one", "alpha"), unit("two", "beta")])
        second = self.engine([unit("three", "gamma")])
        self.assertEqual(first.search("alpha")[0]["index"], 0)
        self.assertEqual(first.search("beta")[0]["index"], 1)
        self.assertEqual(second.search("alpha"), [])
        self.assertEqual(first.search("alpha")[0]["index"], 0)


if __name__ == "__main__":
    unittest.main()
