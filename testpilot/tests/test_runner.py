import json
from datetime import datetime

from liu_kai_testpilot.plugin import Plugin
from liu_kai_testpilot.runner import LiuKaiRunner

from .conftest import UI_XML, state_dump
from .test_plugin import TESTBED, case


def make_plugin(adb, tmp_path, cases):
    p = Plugin(testbed={**TESTBED, "reports_dir": str(tmp_path / "reports")}, adb=adb, sleep=lambda s: None)
    p._testbed = dict(p._testbed)
    p.discover_cases = lambda: cases
    adb.queue("uiautomator dump", UI_XML.format(plain="日"))
    adb.queue("dumpsys activity service", state_dump())
    return p


def fixed_clock():
    return datetime(2026, 9, 26, 12, 34, 56)


def test_runner_runs_cases_and_writes_reports(adb, tmp_path):
    good = case(id="good", name="讀到日", pass_criteria=[{"field": "read.text", "operator": "equals", "value": "日"}])
    bad = case(id="bad", name="讀不到月", pass_criteria=[{"field": "read.text", "operator": "equals", "value": "月"}])
    p = make_plugin(adb, tmp_path, [good, bad])
    result = LiuKaiRunner(p, clock=fixed_clock).run(None, "liu_kai", None, None, None)
    assert result["overall"] == "FAIL"
    assert [(r["case_id"], r["verdict"]) for r in result["results"]] == [("good", True), ("bad", False)]
    report_dir = tmp_path / "reports" / "20260926-123456"
    assert result["report"] == str(report_dir / "report.md")
    data = json.loads((report_dir / "report.json").read_text(encoding="utf-8"))
    assert data["summary"] == {"total": 2, "passed": 1, "failed": 1, "overall": "FAIL"}
    bad_case = data["cases"][1]
    assert bad_case["comment"] == "pass_criteria not satisfied"
    assert bad_case["criteria"] == ["FAIL read.text equals '月'（實際 '日'）"]
    assert bad_case["steps"][0]["output"] == "plain=日"
    md = (report_dir / "report.md").read_text(encoding="utf-8")
    assert "| good | 讀到日 | PASS |" in md
    assert "| bad | 讀不到月 | FAIL | pass_criteria not satisfied |" in md
    assert "FAIL read.text equals '月'" in md


def test_runner_filters_case_ids_and_passes_when_all_pass(adb, tmp_path):
    good = case(id="good", pass_criteria=[{"field": "read.text", "operator": "equals", "value": "日"}])
    other = case(id="other")
    p = make_plugin(adb, tmp_path, [good, other])
    result = LiuKaiRunner(p, clock=fixed_clock).run(None, "liu_kai", ["good"], None, None)
    assert result["overall"] == "PASS"
    assert [r["case_id"] for r in result["results"]] == ["good"]


def test_runner_with_no_cases_fails(adb, tmp_path):
    result = LiuKaiRunner(make_plugin(adb, tmp_path, []), clock=fixed_clock).run(None, "liu_kai", ["x"], None, None)
    assert result["overall"] == "FAIL"


def test_runner_default_clock(adb, tmp_path):
    result = LiuKaiRunner(make_plugin(adb, tmp_path, [])).run(None, "liu_kai", None, None, None)
    assert result["report"].endswith("report.md")

