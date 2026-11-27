#!/usr/bin/env python3

import argparse
import csv
import hashlib
import io
import json
import os
from pathlib import Path
import sys


INPUT_RELATIVE_PATHS = {
    "canonical": Path("core") / "src" / "main" / "resources" / "readings.tsv",
    "windows_copy": Path("windows") / "assets" / "readings.tsv",
    "candidates": Path("windows") / "assets" / "phonetic-candidates.tsv",
    "dictionary": Path("windows") / "assets" / "dictionary.tsv",
    "decisions": Path("docs") / "data" / "readings-decisions.tsv",
}
INPUT_ROLES = tuple(INPUT_RELATIVE_PATHS)
CLASSIFICATIONS = (
    "primary_match",
    "listed_secondary",
    "bopomofo_literal",
    "unresolved_relation",
    "unexpected_missing_character",
    "explicit_empty",
)
DECISION_STATUSES = (
    "consistent",
    "confirmed_error",
    "intentional_difference",
    "unresolved",
)
REVIEWED_STATUSES = ("consistent", "intentional_difference")


def _add_diagnostic(diagnostics, code, source, line, message):
    diagnostics.append(
        {
            "code": code,
            "source": source,
            "line": line,
            "message": message,
        }
    )


def _read_input(path, source, diagnostics):
    try:
        return Path(path).read_bytes()
    except OSError:
        _add_diagnostic(
            diagnostics,
            "read_error",
            source,
            None,
            "could not read the input file",
        )
        return None


def _decode(data, source, diagnostics):
    if data is None:
        return None
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError as error:
        _add_diagnostic(
            diagnostics,
            "invalid_utf8",
            source,
            data[:error.start].count(b"\n") + 1,
            "input is not valid UTF-8",
        )
        return None
    return text[1:] if text.startswith("\ufeff") else text


def _parse_readings(data, source, diagnostics):
    text = _decode(data, source, diagnostics)
    if text is None:
        return {}

    readings = {}
    data_rows = 0
    reader = csv.reader(
        io.StringIO(text, newline=""),
        delimiter="\t",
        quoting=csv.QUOTE_NONE,
    )
    try:
        for fields in reader:
            line = reader.line_num
            if not fields or (len(fields) == 1 and fields[0] == ""):
                continue
            if fields[0].startswith("#"):
                continue
            data_rows += 1
            if len(fields) not in (2, 3) or not fields[0]:
                _add_diagnostic(
                    diagnostics,
                    "missing_required_field",
                    source,
                    line,
                    "readings row requires char and readings fields, with optional grade",
                )
                continue

            char, raw_readings = fields[:2]
            if len(char) != 1 or char.isspace():
                _add_diagnostic(
                    diagnostics,
                    "invalid_character",
                    source,
                    line,
                    "readings character must be one non-whitespace Unicode scalar",
                )
                continue
            values = raw_readings.split(" ")
            if not raw_readings or any(value == "" for value in values):
                _add_diagnostic(
                    diagnostics,
                    "empty_readings",
                    source,
                    line,
                    "readings field must contain non-empty space-separated readings",
                )
                continue
            if char in readings:
                _add_diagnostic(
                    diagnostics,
                    "duplicate_character",
                    source,
                    line,
                    "character is listed more than once",
                )
                continue
            if len(set(values)) != len(values):
                _add_diagnostic(
                    diagnostics,
                    "duplicate_relation",
                    source,
                    line,
                    "character repeats the same reading",
                )
            readings[char] = tuple(values)
    except csv.Error:
        _add_diagnostic(
            diagnostics,
            "missing_required_field",
            source,
            reader.line_num,
            "readings input contains an invalid TSV row",
        )

    if data_rows == 0:
        _add_diagnostic(
            diagnostics,
            "empty_readings",
            source,
            1,
            "readings input contains no character rows",
        )
    return readings


def _parse_table(data, source, required_fields, diagnostics):
    text = _decode(data, source, diagnostics)
    if text is None:
        return []

    reader = csv.DictReader(
        io.StringIO(text, newline=""),
        delimiter="\t",
        quoting=csv.QUOTE_NONE,
    )
    try:
        fields = reader.fieldnames
    except csv.Error:
        fields = None
    if fields is None or tuple(fields) != tuple(required_fields):
        _add_diagnostic(
            diagnostics,
            "missing_required_field",
            source,
            1,
            "TSV header does not contain exactly the required fields",
        )
        return []

    rows = []
    try:
        for row in reader:
            line = reader.line_num
            if None in row or any(row.get(field) is None for field in required_fields):
                _add_diagnostic(
                    diagnostics,
                    "missing_required_field",
                    source,
                    line,
                    "TSV row does not match the required fields",
                )
                continue
            rows.append((line, {field: row[field] for field in required_fields}))
    except csv.Error:
        _add_diagnostic(
            diagnostics,
            "missing_required_field",
            source,
            reader.line_num,
            "TSV input contains an invalid row",
        )
    return rows


def _parse_candidates(data, diagnostics):
    parsed = _parse_table(
        data,
        "candidates",
        ("reading", "text"),
        diagnostics,
    )
    candidates = []
    seen = set()
    groups = {}
    for line, row in parsed:
        reading = row["reading"]
        text = row["text"]
        if reading == "":
            _add_diagnostic(
                diagnostics,
                "missing_required_field",
                "candidates",
                line,
                "candidate row requires a reading",
            )
            continue
        relation = (reading, text)
        if relation in seen:
            _add_diagnostic(
                diagnostics,
                "duplicate_relation",
                "candidates",
                line,
                "candidate reading/text relation is duplicated",
            )
            continue
        seen.add(relation)
        candidates.append((line, reading, text))
        groups.setdefault(reading, []).append((line, text))

    for group in groups.values():
        texts = {text for _, text in group}
        if "" in texts and len(texts) > 1:
            line = min(line for line, _ in group)
            _add_diagnostic(
                diagnostics,
                "mixed_empty_group",
                "candidates",
                line,
                "a candidate reading cannot combine an explicit empty group with text",
            )
    if not candidates:
        _add_diagnostic(
            diagnostics,
            "empty_candidates",
            "candidates",
            1,
            "candidate input contains no data rows",
        )
    return candidates


def _parse_dictionary(data, diagnostics):
    parsed = _parse_table(
        data,
        "dictionary",
        ("section", "code", "text", "freq", "order"),
        diagnostics,
    )
    traditional_texts = set()
    for line, row in parsed:
        if any(row[field] == "" for field in ("section", "code", "text", "freq", "order")):
            _add_diagnostic(
                diagnostics,
                "missing_required_field",
                "dictionary",
                line,
                "dictionary row contains an empty required field",
            )
            continue
        if row["section"] == "TRADITIONAL":
            traditional_texts.add(row["text"])
    if not traditional_texts:
        _add_diagnostic(
            diagnostics,
            "empty_dictionary",
            "dictionary",
            1,
            "dictionary input contains no TRADITIONAL text",
        )
    return traditional_texts


def _is_bopomofo_literal(text, reading):
    return (
        len(text) == 1
        and 0x3105 <= ord(text) <= 0x3129
        and reading == text
    )


def _classify_relation(text, reading, canonical):
    if text == "":
        return "explicit_empty"
    readings = canonical.get(text)
    if readings is not None:
        if reading == readings[0]:
            return "primary_match"
        if reading in readings[1:]:
            return "listed_secondary"
        return "unresolved_relation"
    if _is_bopomofo_literal(text, reading):
        return "bopomofo_literal"
    return "unexpected_missing_character"


def _parse_decisions(data, current_pairs, diagnostics):
    parsed = _parse_table(
        data,
        "decisions",
        ("char", "reading", "status", "source", "reason"),
        diagnostics,
    )
    decisions = []
    seen = set()
    reviewed_statuses = {}
    for line, row in parsed:
        decisions.append(dict(row))
        char = row["char"]
        reading = row["reading"]
        status = row["status"]
        source = row["source"]
        required_pair = char != "" and reading != ""
        source_present = bool(source.strip())

        if not required_pair or status == "":
            _add_diagnostic(
                diagnostics,
                "missing_required_field",
                "decisions",
                line,
                "decision requires char, reading, and status",
            )
        if not source_present:
            _add_diagnostic(
                diagnostics,
                "empty_decision_source",
                "decisions",
                line,
                "decision source must be non-empty",
            )
        if status and status not in DECISION_STATUSES:
            _add_diagnostic(
                diagnostics,
                "invalid_decision_status",
                "decisions",
                line,
                "decision status is not supported",
            )

        if required_pair:
            pair = (char, reading)
            if pair in seen:
                _add_diagnostic(
                    diagnostics,
                    "duplicate_decision",
                    "decisions",
                    line,
                    "decision char/reading pair is duplicated",
                )
            else:
                seen.add(pair)
            if pair not in current_pairs:
                _add_diagnostic(
                    diagnostics,
                    "orphan_decision",
                    "decisions",
                    line,
                    "decision does not match a canonical or candidate relation",
                )
            elif (
                source_present
                and status in DECISION_STATUSES
            ):
                reviewed_statuses.setdefault(pair, set()).add(status)

    decisions.sort(
        key=lambda row: (
            row["char"],
            row["reading"],
            row["status"],
            row["source"],
            row["reason"],
        )
    )
    return decisions, reviewed_statuses


def _sort_diagnostics(diagnostics):
    role_order = {role: index for index, role in enumerate(INPUT_ROLES)}
    diagnostics.sort(
        key=lambda item: (
            role_order.get(item["source"], len(role_order)),
            -1 if item["line"] is None else item["line"],
            item["code"],
            item["message"],
        )
    )


def audit_readings(canonical, windows_copy, candidates, dictionary, decisions):
    paths = {
        "canonical": Path(canonical),
        "windows_copy": Path(windows_copy),
        "candidates": Path(candidates),
        "dictionary": Path(dictionary),
        "decisions": Path(decisions),
    }
    diagnostics = []
    raw = {
        role: _read_input(paths[role], role, diagnostics)
        for role in INPUT_ROLES
    }
    inputs = {
        role: {
            "sha256": (
                hashlib.sha256(raw[role]).hexdigest()
                if raw[role] is not None
                else None
            )
        }
        for role in INPUT_ROLES
    }

    if (
        raw["canonical"] is not None
        and raw["windows_copy"] is not None
        and raw["canonical"] != raw["windows_copy"]
    ):
        _add_diagnostic(
            diagnostics,
            "copy_byte_mismatch",
            "windows_copy",
            None,
            "Windows readings copy is not byte-identical to canonical",
        )

    canonical_readings = _parse_readings(
        raw["canonical"],
        "canonical",
        diagnostics,
    )
    _parse_readings(
        raw["windows_copy"],
        "windows_copy",
        diagnostics,
    )
    candidate_rows = _parse_candidates(raw["candidates"], diagnostics)
    traditional_texts = _parse_dictionary(raw["dictionary"], diagnostics)

    classification_counts = {
        classification: 0 for classification in CLASSIFICATIONS
    }
    relations = []
    unresolved_pairs = set()
    for line, reading, text in candidate_rows:
        classification = _classify_relation(text, reading, canonical_readings)
        classification_counts[classification] += 1
        if classification == "unexpected_missing_character":
            _add_diagnostic(
                diagnostics,
                "unexpected_missing_character",
                "candidates",
                line,
                "candidate text has no readings entry and is not a valid bopomofo literal",
            )
        if classification == "unresolved_relation":
            unresolved_pairs.add((text, reading))
        relations.append(
            {
                "char": text,
                "reading": reading,
                "classification": classification,
                "in_dictionary": text != "" and text in traditional_texts,
            }
        )
    relations.sort(key=lambda row: (row["reading"], row["char"]))

    canonical_pairs = {
        (char, reading)
        for char, values in canonical_readings.items()
        for reading in values
    }
    candidate_pairs = {
        (text, reading)
        for _, reading, text in candidate_rows
        if text != ""
    }
    decision_rows, reviewed_statuses = _parse_decisions(
        raw["decisions"],
        canonical_pairs | candidate_pairs,
        diagnostics,
    )
    _sort_diagnostics(diagnostics)

    unresolved_relations = classification_counts["unresolved_relation"]
    has_incomplete_decision = any(
        status in ("confirmed_error", "unresolved")
        for statuses in reviewed_statuses.values()
        for status in statuses
    )
    all_unresolved_reviewed = all(
        any(status in REVIEWED_STATUSES for status in reviewed_statuses.get(pair, ()))
        for pair in unresolved_pairs
    )
    invariants_passed = not diagnostics
    pronunciation_review_complete = (
        invariants_passed
        and not has_incomplete_decision
        and all_unresolved_reviewed
    )

    return {
        "schema_version": 1,
        "inputs": inputs,
        "invariants": {
            "passed": invariants_passed,
            "diagnostics": diagnostics,
        },
        "classification_counts": classification_counts,
        "relations": relations,
        "dictionary_projection": {
            "section": "TRADITIONAL",
            "candidate_texts": sorted(
                {
                    text
                    for _, _, text in candidate_rows
                    if text != "" and text in traditional_texts
                }
            ),
        },
        "decisions": decision_rows,
        "unresolved_relations": unresolved_relations,
        "pronunciation_review_complete": pronunciation_review_complete,
    }


def _repo_inputs(repo):
    return {
        role: repo / relative_path
        for role, relative_path in INPUT_RELATIVE_PATHS.items()
    }


def _same_path(report_path, input_path):
    if report_path.resolve() == input_path.resolve():
        return True
    return (
        report_path.exists()
        and input_path.exists()
        and os.path.samefile(report_path, input_path)
    )


def _write_report(path, report):
    serialized = (
        json.dumps(report, ensure_ascii=False, sort_keys=True, indent=2)
        + "\n"
    ).encode("utf-8")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(serialized)


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Audit liu-kai readings assets without changing their contents."
    )
    parser.add_argument("--repo", required=True, help="repository root")
    parser.add_argument("--report", required=True, help="deterministic JSON report path")
    parser.add_argument(
        "--check",
        action="store_true",
        help="return non-zero when an audit invariant fails",
    )
    args = parser.parse_args(argv)

    inputs = _repo_inputs(Path(args.repo))
    report_path = Path(args.report)
    try:
        report_collides = any(
            _same_path(report_path, input_path)
            for input_path in inputs.values()
        )
    except OSError:
        print("could not resolve report path safely", file=sys.stderr)
        return 2
    if report_collides:
        print("report path must not resolve to an audit input", file=sys.stderr)
        return 2

    report = audit_readings(**inputs)
    try:
        _write_report(report_path, report)
    except OSError:
        print("could not write audit report", file=sys.stderr)
        return 2

    state = "PASS" if report["invariants"]["passed"] else "FAIL"
    review_complete = str(report["pronunciation_review_complete"]).lower()
    print(
        f"invariants={state}; "
        f"unresolved_relations={report['unresolved_relations']}; "
        f"pronunciation_review_complete={review_complete}"
    )
    return 1 if args.check and not report["invariants"]["passed"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
