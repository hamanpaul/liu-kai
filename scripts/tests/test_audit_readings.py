import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "audit-readings.py"
INPUT_PATHS = {
    "canonical": Path("core") / "src" / "main" / "resources" / "readings.tsv",
    "windows_copy": Path("windows") / "assets" / "readings.tsv",
    "candidates": Path("windows") / "assets" / "phonetic-candidates.tsv",
    "dictionary": Path("windows") / "assets" / "dictionary.tsv",
    "decisions": Path("docs") / "data" / "readings-decisions.tsv",
}
INPUT_ROLES = tuple(INPUT_PATHS)
CLASSIFICATIONS = {
    "primary_match",
    "listed_secondary",
    "bopomofo_literal",
    "unresolved_relation",
    "unexpected_missing_character",
    "explicit_empty",
}
R1 = "ㄚ"
R2 = "ㄛ"
R3 = "ㄜ"
UNKNOWN_READING = "ㄞ"
EMPTY_READING = "ㄨ"
DECISIONS_HEADER = "char\treading\tstatus\tsource\treason\n"


def decision_bytes(*rows):
    return (DECISIONS_HEADER + "".join("\t".join(row) + "\n" for row in rows)).encode("utf-8")


class AuditReadingsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not SCRIPT.is_file():
            raise AssertionError(
                "EXPECTED RED: scripts/audit-readings.py is missing; "
                "run this suite again only after the minimal production implementation exists"
            )
        spec = importlib.util.spec_from_file_location("audit_readings", SCRIPT)
        if spec is None or spec.loader is None:
            raise AssertionError(f"cannot load {SCRIPT.name}")
        cls.audit_module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.audit_module)

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.repo = Path(self.temporary.name)
        self.paths = self.write_inputs(self.repo)

    @staticmethod
    def readings_bytes():
        return (
            "# synthetic readings\n"
            f"A\t{R1} {R2}\n"
            f"B\t{R2}\n"
            f"C\t{R3}\n"
        ).encode("utf-8")

    @staticmethod
    def candidate_bytes(include_missing=True):
        rows = [
            f"{R1}\tA",
            f"{R2}\tA",
            f"{R2}\tB",
            f"{R1}\tㄚ",
            f"{UNKNOWN_READING}\tC",
            f"{EMPTY_READING}\t",
        ]
        if include_missing:
            rows.append(f"{R1}\t双")
        return ("reading\ttext\n" + "\n".join(rows) + "\n").encode("utf-8")

    @staticmethod
    def dictionary_bytes():
        return (
            "section\tcode\ttext\tfreq\torder\n"
            "TRADITIONAL\ta\tA\t100\t0\n"
            "TRADITIONAL\tb\tB\t99\t1\n"
            "TRADITIONAL\tc\tC\t98\t2\n"
            "TRADITIONAL\td\tㄚ\t97\t3\n"
            "TRADITIONAL\te\t雙\t96\t4\n"
            "SIMPLIFIED\ts\t双\t100\t5\n"
        ).encode("utf-8")

    def write_inputs(self, repo, include_missing=True, decisions=None):
        paths = {role: repo / relative for role, relative in INPUT_PATHS.items()}
        for path in paths.values():
            path.parent.mkdir(parents=True, exist_ok=True)
        readings = self.readings_bytes()
        paths["canonical"].write_bytes(readings)
        paths["windows_copy"].write_bytes(readings)
        paths["candidates"].write_bytes(self.candidate_bytes(include_missing))
        paths["dictionary"].write_bytes(self.dictionary_bytes())
        paths["decisions"].write_bytes(decision_bytes() if decisions is None else decisions)
        return paths

    def snapshot_inputs(self, repo_paths=None):
        repo_paths = self.paths if repo_paths is None else repo_paths
        return {
            role: path.read_bytes() if path.is_file() else None
            for role, path in repo_paths.items()
        }

    def audit(self):
        return self.audit_module.audit_readings(
            canonical=self.paths["canonical"],
            windows_copy=self.paths["windows_copy"],
            candidates=self.paths["candidates"],
            dictionary=self.paths["dictionary"],
            decisions=self.paths["decisions"],
        )

    def run_cli(self, repo=None, report=None):
        repo = self.repo if repo is None else repo
        report = repo / "build" / "readings-audit.json" if report is None else report
        return subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                "--repo",
                str(repo),
                "--report",
                str(report),
                "--check",
            ],
            cwd=repo,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=30,
            check=False,
        )

    def assert_check_fails(self, tag):
        report_path = self.repo / "build" / f"{tag}.json"
        completed = self.run_cli(report=report_path)
        self.assertNotEqual(completed.returncode, 0, completed.stderr or completed.stdout)
        self.assertTrue(report_path.is_file(), completed.stderr or completed.stdout)
        report = json.loads(report_path.read_text(encoding="utf-8"))
        self.assertFalse(report["invariants"]["passed"])
        expected = (
            f"invariants=FAIL; unresolved_relations={report['unresolved_relations']}; "
            f"pronunciation_review_complete={str(report['pronunciation_review_complete']).lower()}"
        )
        self.assertEqual(completed.stdout.strip(), expected)
        return completed, report

    def assert_diagnostic(self, report, source, code, line=None):
        match = [
            diagnostic
            for diagnostic in report["invariants"]["diagnostics"]
            if diagnostic["source"] == source and diagnostic["code"] == code
        ]
        if line is not None:
            match = [diagnostic for diagnostic in match if diagnostic["line"] == line]
        self.assertTrue(match, report["invariants"]["diagnostics"])
        self.assertTrue(all(diagnostic["message"] for diagnostic in match))
        messages = " ".join(diagnostic["message"] for diagnostic in match)
        self.assertNotIn(str(self.repo), messages)
        self.assertNotIn(self.repo.as_posix(), messages)
        self.assertNotRegex(
            messages,
            re.compile(r"\b\d{4}-\d{2}-\d{2}[T ]\d{2}:\d{2}"),
        )

    def test_schema_classifies_relations_without_changing_primary_rules_or_inputs(self):
        before = self.snapshot_inputs()
        report = self.audit()
        self.assertEqual(
            set(report),
            {
                "schema_version",
                "inputs",
                "invariants",
                "classification_counts",
                "relations",
                "dictionary_projection",
                "decisions",
                "unresolved_relations",
                "pronunciation_review_complete",
            },
        )
        self.assertEqual(report["schema_version"], 1)
        self.assertEqual(set(report["inputs"]), set(INPUT_ROLES))
        for source in report["inputs"].values():
            self.assertEqual(set(source), {"sha256"})
            self.assertRegex(source["sha256"], re.compile(r"^[0-9a-f]{64}$"))
        self.assertEqual(
            report["inputs"]["canonical"]["sha256"],
            report["inputs"]["windows_copy"]["sha256"],
        )
        self.assertEqual(
            set(report["invariants"]),
            {"passed", "diagnostics"},
        )
        self.assertEqual(set(report["classification_counts"]), CLASSIFICATIONS)
        self.assertEqual(
            report["classification_counts"],
            {
                "primary_match": 2,
                "listed_secondary": 1,
                "bopomofo_literal": 1,
                "unresolved_relation": 1,
                "unexpected_missing_character": 1,
                "explicit_empty": 1,
            },
        )

        by_relation = {(row["char"], row["reading"]): row for row in report["relations"]}
        self.assertEqual(by_relation[("A", R1)]["classification"], "primary_match")
        self.assertEqual(by_relation[("A", R2)]["classification"], "listed_secondary")
        self.assertEqual(by_relation[("B", R2)]["classification"], "primary_match")
        self.assertEqual(by_relation[("ㄚ", R1)]["classification"], "bopomofo_literal")
        self.assertEqual(
            by_relation[("C", UNKNOWN_READING)]["classification"],
            "unresolved_relation",
        )
        self.assertTrue(by_relation[("C", UNKNOWN_READING)]["in_dictionary"])
        self.assertEqual(
            by_relation[("双", R1)]["classification"],
            "unexpected_missing_character",
        )
        self.assertFalse(by_relation[("双", R1)]["in_dictionary"])
        self.assertEqual(
            by_relation[("", EMPTY_READING)]["classification"],
            "explicit_empty",
        )
        self.assertTrue(
            all(
                set(row) == {"char", "reading", "classification", "in_dictionary"}
                for row in report["relations"]
            )
        )
        self.assertEqual(
            [(row["reading"], row["char"]) for row in report["relations"]],
            sorted((row["reading"], row["char"]) for row in report["relations"]),
        )
        self.assertEqual(
            report["dictionary_projection"],
            {
                "section": "TRADITIONAL",
                "candidate_texts": ["A", "B", "C", "ㄚ"],
            },
        )
        self.assertEqual(report["decisions"], [])
        self.assertEqual(report["unresolved_relations"], 1)
        self.assertFalse(report["pronunciation_review_complete"])
        self.assertFalse(report["invariants"]["passed"])
        self.assert_diagnostic(report, "candidates", "unexpected_missing_character")
        for diagnostic in report["invariants"]["diagnostics"]:
            self.assertEqual(
                set(diagnostic),
                {"code", "source", "line", "message"},
            )
        self.assertEqual(before, self.snapshot_inputs())

    def test_windows_copy_must_be_byte_identical_without_normalization(self):
        self.write_inputs(self.repo, include_missing=False)
        canonical = self.paths["canonical"].read_bytes()
        lines = canonical.splitlines(keepends=True)
        variants = {
            "one-byte": canonical + b"\n",
            "bom": b"\xef\xbb\xbf" + canonical,
            "line-endings": canonical.replace(b"\n", b"\r\n"),
            "row-order": b"".join([lines[0], lines[3], lines[2], lines[1]]),
        }
        for label, different_bytes in variants.items():
            with self.subTest(label=label):
                self.write_inputs(self.repo, include_missing=False)
                self.paths["windows_copy"].write_bytes(different_bytes)
                report = self.audit()
                self.assertFalse(report["invariants"]["passed"])
                self.assert_diagnostic(report, "windows_copy", "copy_byte_mismatch")
                self.assert_check_fails(f"copy-{label}")

    def test_repository_readings_are_byte_identical_lf_assets(self):
        repo = Path(__file__).resolve().parents[2]
        canonical = (repo / INPUT_PATHS["canonical"]).read_bytes()
        windows_copy = (repo / INPUT_PATHS["windows_copy"]).read_bytes()
        with self.subTest(contract="canonical-lf"):
            self.assertFalse(
                b"\r" in canonical,
                str(INPUT_PATHS["canonical"]) + " contains CR bytes",
            )
        with self.subTest(contract="windows-copy-lf"):
            self.assertFalse(
                b"\r" in windows_copy,
                str(INPUT_PATHS["windows_copy"]) + " contains CR bytes",
            )
        with self.subTest(contract="byte-identical"):
            self.assertTrue(
                canonical == windows_copy,
                "canonical and Windows readings bytes differ",
            )

    def test_header_only_sources_and_dictionary_without_traditional_are_invalid(self):
        cases = (
            (
                "candidate-header-only",
                "candidates",
                b"reading\ttext\n",
                "empty_candidates",
            ),
            (
                "dictionary-header-only",
                "dictionary",
                b"section\tcode\ttext\tfreq\torder\n",
                "empty_dictionary",
            ),
            (
                "dictionary-no-traditional",
                "dictionary",
                b"section\tcode\ttext\tfreq\torder\n"
                b"SIMPLIFIED\ts\tA\t100\t0\n",
                "empty_dictionary",
            ),
        )
        for label, source, contents, code in cases:
            with self.subTest(label=label):
                self.paths = self.write_inputs(self.repo, include_missing=False)
                self.paths[source].write_bytes(contents)
                report = self.audit()
                self.assertFalse(report["invariants"]["passed"])
                self.assertFalse(report["pronunciation_review_complete"])
                self.assert_diagnostic(report, source, code, line=1)
                self.assert_check_fails(f"empty-{label}")

        self.paths = self.write_inputs(
            self.repo,
            include_missing=False,
            decisions=decision_bytes(),
        )
        report = self.audit()
        self.assertTrue(report["invariants"]["passed"])
        self.assertFalse(report["pronunciation_review_complete"])
        self.assertEqual(report["decisions"], [])

    def test_tsv_headers_require_the_exact_declared_order(self):
        cases = (
            (
                "candidates",
                b"text\treading\n"
                + f"A\t{R1}\n".encode("utf-8"),
            ),
            (
                "dictionary",
                b"code\tsection\ttext\tfreq\torder\n"
                b"a\tTRADITIONAL\tA\t100\t0\n",
            ),
            (
                "decisions",
                b"reading\tchar\tstatus\tsource\treason\n"
                + (
                    f"{UNKNOWN_READING}\tC\tconsistent\tevidence\tfixture\n"
                ).encode("utf-8"),
            ),
        )
        for source, contents in cases:
            with self.subTest(source=source):
                self.paths = self.write_inputs(self.repo, include_missing=False)
                self.paths[source].write_bytes(contents)
                report = self.audit()
                self.assertFalse(report["invariants"]["passed"])
                self.assertFalse(report["pronunciation_review_complete"])
                self.assert_diagnostic(
                    report,
                    source,
                    "missing_required_field",
                    line=1,
                )
                self.assert_check_fails(f"header-order-{source}")

    def test_readings_character_is_exactly_one_non_whitespace_scalar(self):
        invalid_characters = (
            ("multiple-codepoints", "AB"),
            ("whitespace-only", " "),
            ("character-plus-space", "A "),
        )
        for label, char in invalid_characters:
            with self.subTest(label=label):
                self.paths = self.write_inputs(self.repo, include_missing=False)
                invalid = self.readings_bytes() + f"{char}\t{R1}\n".encode("utf-8")
                self.paths["canonical"].write_bytes(invalid)
                self.paths["windows_copy"].write_bytes(invalid)
                report = self.audit()
                self.assertFalse(report["invariants"]["passed"])
                self.assertFalse(report["pronunciation_review_complete"])
                for source in ("canonical", "windows_copy"):
                    self.assert_diagnostic(
                        report,
                        source,
                        "invalid_character",
                        line=5,
                    )
                self.assert_check_fails(f"invalid-character-{label}")

        self.paths = self.write_inputs(self.repo, include_missing=False)
        extension_han = self.readings_bytes() + f"{chr(0x20000)}\t{R1}\n".encode("utf-8")
        self.paths["canonical"].write_bytes(extension_han)
        self.paths["windows_copy"].write_bytes(extension_han)
        report = self.audit()
        self.assertTrue(report["invariants"]["passed"])
        self.assertFalse(report["pronunciation_review_complete"])
        self.assertNotIn(
            "invalid_character",
            {
                diagnostic["code"]
                for diagnostic in report["invariants"]["diagnostics"]
            },
        )

    def test_invalid_utf8_reports_its_physical_line(self):
        self.paths = self.write_inputs(self.repo, include_missing=False)
        self.paths["candidates"].write_bytes(
            b"reading\ttext\n"
            + f"{R1}\tA\n".encode("utf-8")
            + b"\xff\tA\n"
        )
        report = self.audit()
        self.assertFalse(report["invariants"]["passed"])
        self.assertFalse(report["pronunciation_review_complete"])
        self.assert_diagnostic(report, "candidates", "invalid_utf8", line=3)
        self.assert_check_fails("invalid-utf8-line-3")

    def test_invalid_inputs_are_diagnosed_and_fail_check(self):
        def invalid_utf8(paths):
            paths["candidates"].write_bytes(paths["candidates"].read_bytes() + b"\xff\n")

        def missing_reading_field(paths):
            malformed = "# synthetic readings\nA\nB\tㄛ\nC\tㄜ\n".encode("utf-8")
            paths["canonical"].write_bytes(malformed)
            paths["windows_copy"].write_bytes(malformed)

        def missing_candidate_field(paths):
            paths["candidates"].write_bytes(
                b"reading\tcandidate\n"
                + self.candidate_bytes(False).split(b"\n", 1)[1]
            )

        def missing_dictionary_field(paths):
            paths["dictionary"].write_bytes(
                b"section\tcode\ttext\tfreq\nTRADITIONAL\ta\tA\t100\n"
            )

        def missing_decision_field(paths):
            paths["decisions"].write_bytes(b"char\treading\tstatus\tsource\n")

        def duplicate_character(paths):
            duplicate = self.readings_bytes() + f"A\t{R1}\n".encode("utf-8")
            paths["canonical"].write_bytes(duplicate)
            paths["windows_copy"].write_bytes(duplicate)

        def duplicate_relation(paths):
            paths["candidates"].write_bytes(
                paths["candidates"].read_bytes() + f"{R1}\tA\n".encode("utf-8")
            )

        def empty_readings(paths):
            empty = self.readings_bytes() + b"D\t\n"
            paths["canonical"].write_bytes(empty)
            paths["windows_copy"].write_bytes(empty)

        def mixed_empty_group(paths):
            paths["candidates"].write_bytes(
                paths["candidates"].read_bytes() + f"{EMPTY_READING}\tA\n".encode("utf-8")
            )

        def unreadable(paths):
            paths["decisions"].unlink()

        cases = (
            ("utf8", "candidates", "invalid_utf8", invalid_utf8),
            ("reading-fields", "canonical", "missing_required_field", missing_reading_field),
            ("candidate-fields", "candidates", "missing_required_field", missing_candidate_field),
            ("dictionary-fields", "dictionary", "missing_required_field", missing_dictionary_field),
            ("decision-fields", "decisions", "missing_required_field", missing_decision_field),
            ("duplicate-char", "canonical", "duplicate_character", duplicate_character),
            ("duplicate-relation", "candidates", "duplicate_relation", duplicate_relation),
            ("empty-readings", "canonical", "empty_readings", empty_readings),
            ("mixed-empty", "candidates", "mixed_empty_group", mixed_empty_group),
            ("read-error", "decisions", "read_error", unreadable),
        )
        for label, source, code, mutate in cases:
            with self.subTest(label=label):
                self.paths = self.write_inputs(self.repo, include_missing=False)
                mutate(self.paths)
                report = self.audit()
                self.assertFalse(report["invariants"]["passed"])
                self.assert_diagnostic(report, source, code)
                self.assert_check_fails(f"invalid-{label}")

    def test_bopomofo_literal_is_only_one_symbol_equal_to_its_reading(self):
        self.write_inputs(self.repo, include_missing=False)
        literal_rows = (
            "reading\ttext\n"
            "ㄆ\tㄆ\n"
            "ㄚ\tㄅ\n"
            "ㄅㄆ\tㄅㄆ\n"
            "ㄚ\tˇ\n"
        ).encode("utf-8")
        self.paths["candidates"].write_bytes(literal_rows)
        report = self.audit()
        by_relation = {(row["char"], row["reading"]): row for row in report["relations"]}
        self.assertEqual(by_relation[("ㄆ", "ㄆ")]["classification"], "bopomofo_literal")
        for char, reading in (("ㄅ", "ㄚ"), ("ㄅㄆ", "ㄅㄆ"), ("ˇ", "ㄚ")):
            self.assertEqual(
                by_relation[(char, reading)]["classification"],
                "unexpected_missing_character",
            )
        self.assertEqual(
            report["classification_counts"]["bopomofo_literal"],
            1,
        )
        self.assertEqual(
            report["classification_counts"]["unexpected_missing_character"],
            3,
        )
        self.assertFalse(report["invariants"]["passed"])
        self.assert_check_fails("literal-boundary")

    def test_decisions_use_the_closed_contract_and_gate_review_completion(self):
        for status in ("consistent", "intentional_difference"):
            with self.subTest(status=status):
                decisions = decision_bytes(
                    ("C", UNKNOWN_READING, status, "synthetic-evidence", "candidate fixture"),
                    ("C", R3, "consistent", "synthetic-evidence", "canonical-only fixture"),
                )
                self.paths = self.write_inputs(
                    self.repo,
                    include_missing=False,
                    decisions=decisions,
                )
                report = self.audit()
                self.assertTrue(report["invariants"]["passed"])
                self.assertTrue(report["pronunciation_review_complete"])
                self.assertEqual(report["unresolved_relations"], 1)
                self.assertEqual(
                    report["decisions"],
                    [
                        {
                            "char": "C",
                            "reading": R3,
                            "status": "consistent",
                            "source": "synthetic-evidence",
                            "reason": "canonical-only fixture",
                        },
                        {
                            "char": "C",
                            "reading": UNKNOWN_READING,
                            "status": status,
                            "source": "synthetic-evidence",
                            "reason": "candidate fixture",
                        },
                    ],
                )
                completed = self.run_cli()
                self.assertEqual(completed.returncode, 0, completed.stderr or completed.stdout)

        for status in ("confirmed_error", "unresolved"):
            with self.subTest(status=status):
                decisions = decision_bytes(
                    ("C", UNKNOWN_READING, status, "synthetic-evidence", "fixture")
                )
                self.paths = self.write_inputs(
                    self.repo,
                    include_missing=False,
                    decisions=decisions,
                )
                report = self.audit()
                self.assertTrue(report["invariants"]["passed"])
                self.assertFalse(report["pronunciation_review_complete"])

        duplicate = ("C", UNKNOWN_READING, "consistent", "evidence", "fixture")
        cases = (
            (
                "status",
                decision_bytes(("C", UNKNOWN_READING, "accepted", "evidence", "fixture")),
                "invalid_decision_status",
            ),
            (
                "source",
                decision_bytes(("C", UNKNOWN_READING, "consistent", "", "fixture")),
                "empty_decision_source",
            ),
            (
                "duplicate",
                decision_bytes(duplicate, duplicate),
                "duplicate_decision",
            ),
            (
                "orphan",
                decision_bytes(("D", UNKNOWN_READING, "consistent", "evidence", "fixture")),
                "orphan_decision",
            ),
            (
                "missing-char",
                decision_bytes(("", UNKNOWN_READING, "consistent", "evidence", "fixture")),
                "missing_required_field",
            ),
        )
        for label, decisions, code in cases:
            with self.subTest(invalid=label):
                self.paths = self.write_inputs(
                    self.repo,
                    include_missing=False,
                    decisions=decisions,
                )
                report = self.audit()
                self.assert_diagnostic(report, "decisions", code)
                self.assertFalse(report["invariants"]["passed"])
                self.assertFalse(report["pronunciation_review_complete"])
                self.assert_check_fails(f"decision-{label}")

    def test_decisions_cannot_mask_missing_characters_or_format_errors(self):
        missing_decision = decision_bytes(
            ("双", R1, "consistent", "synthetic-evidence", "does not add readings")
        )
        self.paths = self.write_inputs(
            self.repo,
            include_missing=True,
            decisions=missing_decision,
        )
        report = self.audit()
        self.assertFalse(report["invariants"]["passed"])
        self.assertFalse(report["pronunciation_review_complete"])
        self.assertEqual(
            report["classification_counts"]["unexpected_missing_character"],
            1,
        )
        self.assert_diagnostic(report, "candidates", "unexpected_missing_character")
        self.assert_check_fails("masked-missing")

        self.paths = self.write_inputs(
            self.repo,
            include_missing=False,
            decisions=decision_bytes(
                ("A", R1, "consistent", "synthetic-evidence", "format stays invalid")
            ),
        )
        self.paths["candidates"].write_bytes(
            self.paths["candidates"].read_bytes() + f"{R1}\tA\n".encode("utf-8")
        )
        report = self.audit()
        self.assert_diagnostic(report, "candidates", "duplicate_relation")
        self.assertFalse(report["invariants"]["passed"])
        self.assertFalse(report["pronunciation_review_complete"])
        self.assert_check_fails("masked-format")

    def test_check_allows_unknown_relations_but_reports_incomplete_review(self):
        self.paths = self.write_inputs(self.repo, include_missing=False)
        before = self.snapshot_inputs()
        report_path = self.repo / "build" / "readings-audit.json"
        completed = self.run_cli(report=report_path)
        self.assertEqual(completed.returncode, 0, completed.stderr or completed.stdout)
        self.assertEqual(completed.stderr, "")
        self.assertEqual(
            completed.stdout.strip(),
            "invariants=PASS; unresolved_relations=1; "
            "pronunciation_review_complete=false",
        )
        report = json.loads(report_path.read_text(encoding="utf-8"))
        self.assertEqual(report, self.audit())
        self.assertTrue(report["invariants"]["passed"])
        self.assertEqual(report["unresolved_relations"], 1)
        self.assertFalse(report["pronunciation_review_complete"])
        self.assertEqual(before, self.snapshot_inputs())

    def test_report_json_is_deterministic_across_roots_and_mtimes(self):
        with tempfile.TemporaryDirectory() as first, tempfile.TemporaryDirectory() as second:
            first_repo = Path(first)
            second_repo = Path(second)
            first_paths = self.write_inputs(first_repo, include_missing=False)
            second_paths = self.write_inputs(second_repo, include_missing=False)
            first_before = self.snapshot_inputs(first_paths)
            second_before = self.snapshot_inputs(second_paths)
            for path in first_paths.values():
                os.utime(path, (1_700_000_000, 1_700_000_000))
            for path in second_paths.values():
                os.utime(path, (1_800_000_000, 1_800_000_000))
            first_report = first_repo / "out" / "audit.json"
            second_report = second_repo / "out" / "audit.json"
            first_run = self.run_cli(repo=first_repo, report=first_report)
            second_run = self.run_cli(repo=second_repo, report=second_report)
            self.assertEqual(first_run.returncode, 0, first_run.stderr or first_run.stdout)
            self.assertEqual(second_run.returncode, 0, second_run.stderr or second_run.stdout)
            first_json = first_report.read_bytes()
            second_json = second_report.read_bytes()
            self.assertEqual(first_json, second_json)
            report = json.loads(first_json)
            self.assertEqual(
                first_json,
                (json.dumps(report, ensure_ascii=False, sort_keys=True, indent=2) + "\n").encode(
                    "utf-8"
                ),
            )
            report_text = first_json.decode("utf-8")
            for root in (first_repo, second_repo):
                self.assertNotIn(str(root), report_text)
                self.assertNotIn(root.as_posix(), report_text)
            self.assertNotIn("generated_at", report_text)
            self.assertNotIn("timestamp", report_text)
            self.assertEqual(first_before, self.snapshot_inputs(first_paths))
            self.assertEqual(second_before, self.snapshot_inputs(second_paths))

    def test_report_rejects_every_input_path_including_resolved_aliases(self):
        self.paths = self.write_inputs(self.repo, include_missing=False)
        before = self.snapshot_inputs()
        alias_parent = self.repo / "alias"
        alias_parent.mkdir()
        for role, input_path in self.paths.items():
            with self.subTest(role=role):
                report_alias = alias_parent / ".." / input_path.relative_to(self.repo)
                self.assertEqual(report_alias.resolve(), input_path.resolve())
                completed = self.run_cli(report=report_alias)
                self.assertNotEqual(completed.returncode, 0)
                self.assertIn("report", (completed.stdout + completed.stderr).lower())
                self.assertEqual(before, self.snapshot_inputs())


if __name__ == "__main__":
    unittest.main()
