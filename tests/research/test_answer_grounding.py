#!/usr/bin/env python3
"""Literal quotation integrity is distinct from claim truth and fresh quality."""

from copy import deepcopy
import unittest
from unittest.mock import patch
from pathlib import Path
import tempfile

from code_context_pair import ANSWER_DISCIPLINE, GUIDE, GROUNDED_GUIDE, answer_guide, audit_messages, check_answer
from shin_semantic.artifact import Rejected, encode
from shin_semantic.compact import number_read


def trace_for(text, start=10, end=13, path="src/flow.cpp", state="complete"):
    return [encode({"native": {"evidence": {"bodies": [{
        "path": path, "start_line": start, "end_line": end, "text": text, "state": state}]}}}).decode()]


def answer_for(quote):
    return {"schema_version": "shin.code-answer.v2", "answer": "Observed failure field.",
            "claims": [{"text": "The second branch writes its own error field.",
                        "evidence": [{"path": "src/flow.cpp", "quote": quote}]}],
            "gaps": [], "tests_executed": False}


class GroundingTests(unittest.TestCase):
    def setUp(self):
        self.text = 'first();\r\nif (failed) {\r\n  row["branch_error"] = code;\r\n}\r\n'
        self.trace = trace_for(self.text)
        self.quote = 'if (failed) {\r\n  row["branch_error"] = code;'

    def check(self, answer=None, trace=None):
        return check_answer(answer or answer_for(self.quote), trace or self.trace, "quoted-v2")

    def test_exact_quote_resolves_physical_lines_without_rewriting(self):
        answer = answer_for(self.quote)
        original = deepcopy(answer)
        result = self.check(answer)
        self.assertEqual(answer, original)
        self.assertEqual(result["resolved_citations"], [{"path": "src/flow.cpp", "start_line": 11,
                         "end_line": 12, "quote": self.quote}])
        self.assertTrue(result["literal_quotes_verified"])
        self.assertEqual(result["semantic_support"], "requires-main-session-source-review")

    def test_typo_whitespace_normalization_and_ellipsis_do_not_match(self):
        for quote in (self.quote.replace("branch_error", "other_error"), self.quote.replace("\r\n", "\n"),
                      'if (failed) { ... row["branch_error"] = code;'):
            with self.subTest(quote=quote), self.assertRaisesRegex(Rejected, "pilot-unread-quote"):
                self.check(answer_for(quote))

    def test_redacted_unread_and_wrong_file_cannot_ground_claim(self):
        for trace in (trace_for(None, state="redacted"), trace_for(self.text, path="src/other.cpp"),
                      [encode({"native": {"records": [{"path": "src/flow.cpp", "excerpt": self.quote}]}}).decode()]):
            with self.assertRaisesRegex(Rejected, "pilot-unread-quote"):
                self.check(trace=trace)

    def test_repeats_are_ambiguous_even_on_same_line(self):
        for text in ("call(); call();", "call();\ncall();"):
            with self.assertRaisesRegex(Rejected, "pilot-ambiguous-quote"):
                self.check(answer_for("call();"), trace_for(text))

    def test_overlapping_or_repeated_deliveries_share_one_physical_location(self):
        trace = self.trace + self.trace + trace_for(self.text.split("\n", 1)[1], start=11)
        self.assertEqual(self.check(trace=trace)["resolved_citations"][0]["start_line"], 11)

    def test_only_lf_counts_as_physical_line_separator(self):
        text = 'prefix\v\r\u2028name();\nlast();'
        result = self.check(answer_for("name();"), trace_for(text, start=7, end=8))
        self.assertEqual(result["resolved_citations"][0]["start_line"], 7)
        result = self.check(answer_for("last();"), trace_for(text, start=7, end=8))
        self.assertEqual(result["resolved_citations"][0]["start_line"], 8)

    def test_ending_newline_does_not_add_an_unquoted_line(self):
        result = self.check(answer_for("first();\r\n"))
        self.assertEqual(result["resolved_citations"][0]["end_line"], 10)

    def test_quote_byte_limit_and_request_shape_are_strict(self):
        for quote in ("", " \n\t", "x" * 513, "\u00e9" * 257):
            with self.assertRaisesRegex(Rejected, "invalid-pilot-quote"):
                self.check(answer_for(quote))
        answer = answer_for(self.quote)
        answer["claims"][0]["evidence"][0]["start_line"] = 11
        with self.assertRaisesRegex(Rejected, "invalid-pilot-quote"):
            self.check(answer)

    def test_missing_claims_and_oversized_answers_are_not_verified(self):
        answer = answer_for(self.quote)
        for changed in ({**answer, "claims": []}, {**answer, "claims": answer["claims"] * 9},
                        {**answer, "answer": "x" * 32768}, {**answer, "tests_executed": True}):
            with self.assertRaises(Rejected):
                self.check(changed)
        empty = {**answer, "answer": "", "claims": [], "gaps": ["No delivered source."]}
        self.assertEqual(self.check(empty)["resolved_citations"], [])
        answer["claims"][0]["evidence"] *= 4
        with self.assertRaises(Rejected):
            self.check(answer)

    def test_literal_match_does_not_certify_semantics(self):
        answer = answer_for(self.quote)
        answer["claims"][0]["text"] = "This branch performs a network request."
        self.assertEqual(self.check(answer)["semantic_support"], "requires-main-session-source-review")

    def test_legacy_contract_is_explicit_not_regraded(self):
        answer = answer_for(self.quote)
        with self.assertRaises(Rejected):
            check_answer(answer, self.trace)
        answer.pop("schema_version")
        answer["claims"][0]["evidence"] = [{"path": "src/flow.cpp", "start_line": 11, "end_line": 12}]
        self.assertTrue(check_answer(answer, self.trace)["citations_delivered"])
        with self.assertRaises(Rejected):
            self.check(answer)

    def test_guide_is_fixed_generic_and_audited(self):
        self.assertEqual(answer_guide("legacy-v1"), GUIDE)
        self.assertEqual(answer_guide("quoted-v2"), GROUNDED_GUIDE)
        for guide in (GUIDE, GROUNDED_GUIDE):
            self.assertEqual(guide.count(ANSWER_DISCIPLINE), 1)
            for name in ("reason_code", "health_reason_code", "session_bootstrap", "pending_"):
                self.assertNotIn(name, guide)
        messages = [{"role": "system", "content": GROUNDED_GUIDE}, {"role": "user", "content": "question"}]
        class Flow:
            trace = []
        audit_messages(messages, Flow(), "question", GROUNDED_GUIDE)
        with self.assertRaisesRegex(Rejected, "pilot-system-context-mismatch"):
            audit_messages(messages, Flow(), "question")

    def test_numbered_delivery_supports_raw_quotes_and_physical_citations(self):
        from shin_semantic.artifact import decode
        raw = decode(self.trace[0])
        raw.update(schema_version="shin.agent-read.v1", ok=True)
        trace = [encode(number_read(raw)).decode()]
        self.assertTrue(self.check(trace=trace)["literal_quotes_verified"])
        answer = answer_for(self.quote)
        answer.pop("schema_version")
        answer["claims"][0]["evidence"] = [{"path": "src/flow.cpp", "start_line": 11, "end_line": 12}]
        self.assertTrue(check_answer(answer, trace)["citations_delivered"])



if __name__ == "__main__":
    unittest.main()
