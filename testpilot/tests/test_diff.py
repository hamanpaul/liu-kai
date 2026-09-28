import json
from datetime import datetime
from types import SimpleNamespace

import pytest

from liu_kai_testpilot import diff
from liu_kai_testpilot.diff import DiffRunner, Target, load_diff_cases, summarize_steps
from liu_kai_testpilot.steps import DeviceConfig

from .conftest import UI_XML, FakeAdb, ime_window_block

OFFICIAL = Target("official", "com.boshiamy.x/.Ime")
LIUKAI = Target("liu-kai", "com.hamanpaul.liukai/.ime.LiuKaiImeService")
CASE = {"id": "space", "name": "空白上屏首選", "checklist": 1, "expect": "首選字", "steps": [{"action": "keys", "keys": ["d", "o", "SPACE"]}]}


def bound(component, host="com.hamanpaul.liukai.testhost"):
    """`dumpsys input_method`：目前輸入法已接上 testhost 的輸入欄。"""
    return f"  mCurId={component} mHaveConnection=true mBoundToMethod=true mVisibleBound=false\n    packageName={host} autofillId=0\n"


def ui(text):
    return UI_XML.format(plain=text)


def make_runner(fake, tmp_path):
    return DiffRunner(fake, DeviceConfig(settle_ms=0), tmp_path, sleep=lambda s: None)


def test_target_package_and_steps_summary():
    assert OFFICIAL.package == "com.boshiamy.x"
    steps = [{"action": "keys", "keys": ["d", "o"]}, {"action": "text", "text": "c*"}, {"action": "combo", "keys": ["SHIFT_LEFT", "A"]}]
    assert summarize_steps(steps) == "d o ／ text:c* ／ SHIFT_LEFT+A"


def test_bundled_diff_cases_are_valid():
    cases = load_diff_cases()
    ids = [c["id"] for c in cases]
    assert len(ids) >= 20
    assert len(ids) == len(set(ids))
    for c in cases:
        assert c["name"] and c["steps"], c["id"]
        assert all(s["action"] in {"keys", "text", "combo"} for s in c["steps"]), c["id"]


def test_prepare_restarts_ime_and_waits_until_field_is_bound(tmp_path):
    fake = FakeAdb()
    fake.queue("dumpsys input_method", "mCurId=other", bound(OFFICIAL.component))
    fake.queue("uiautomator dump", ui(""))
    make_runner(fake, tmp_path).prepare(OFFICIAL)
    shells = [c[1] for c in fake.of("shell")]
    assert shells[:3] == ["am force-stop com.boshiamy.x", "ime enable com.boshiamy.x/.Ime", "ime set com.boshiamy.x/.Ime"]
    assert any(s.startswith("am start -W -S -n com.hamanpaul.liukai.testhost") for s in shells)
    assert fake.of("tap") == [("tap", 540, 180)]
    assert shells.count("dumpsys input_method") == 2


def test_prepare_gives_up_when_ime_never_binds(tmp_path):
    fake = FakeAdb()
    fake.queue("dumpsys input_method", bound(OFFICIAL.component, host="com.other"))
    fake.queue("uiautomator dump", ui(""))
    with pytest.raises(RuntimeError, match="official 未接上輸入欄"):
        make_runner(fake, tmp_path).prepare(OFFICIAL)


def test_prepare_fails_when_field_cannot_be_tapped(tmp_path):
    fake = FakeAdb()
    fake.queue("uiautomator dump", '<hierarchy rotation="0"></hierarchy>')
    with pytest.raises(RuntimeError, match="tap_field"):
        make_runner(fake, tmp_path).prepare(OFFICIAL)


def test_run_case_records_field_text_window_visibility_and_optional_screenshot(tmp_path):
    fake = FakeAdb()
    fake.queue("dumpsys input_method", bound(OFFICIAL.component))
    fake.queue("dumpsys window windows", ime_window_block(True))
    fake.queue("uiautomator dump", ui(""), ui("日"))
    result = make_runner(fake, tmp_path).run_case({**CASE, "screenshot": True}, OFFICIAL)
    assert result == {"text": "日", "visible": True, "error": None, "screenshot": "space-official.png"}
    assert (tmp_path / "space-official.png").read_bytes() == b"PNG"
    assert fake.of("keyevent") == [("keyevent", ("KEYCODE_D", "KEYCODE_O", "KEYCODE_SPACE"))]
    assert make_runner(fake, tmp_path).run_case(CASE, OFFICIAL) == {"text": "日", "visible": True, "error": None, "screenshot": None}
    assert len(fake.of("screencap")) == 1


def test_run_case_reports_step_errors_instead_of_raising(tmp_path):
    fake = FakeAdb()
    fake.queue("dumpsys input_method", bound(OFFICIAL.component))
    fake.queue("uiautomator dump", ui(""))
    fake.fail_on = "input keyevent"
    result = make_runner(fake, tmp_path).run_case(CASE, OFFICIAL)
    assert result["text"] is None
    assert result["visible"] is None
    assert result["error"].startswith("keys：AdbError")
    assert fake.of("screencap") == []


def test_run_compares_targets_and_judges_against_user_answers(tmp_path):
    runner = make_runner(FakeAdb(), tmp_path)
    # (文字, 輸入法視窗是否顯示)；None 表示執行錯誤
    outcomes = {
        ("same", "official"): ("日", True), ("same", "liu-kai"): ("日", True),
        ("diff", "official"): ("vx", True), ("diff", "liu-kai"): ("", True),
        ("hidden", "official"): ("vx", False), ("hidden", "liu-kai"): ("vx", True),
        ("err", "official"): None, ("err", "liu-kai"): ("日 ", False),
    }

    def fake_run_case(case, target):
        o = outcomes[(case["id"], target.name)]
        shot = "err-liu-kai.png" if (case["id"], target.name) == ("err", "liu-kai") else None
        if o is None:
            return {"text": None, "visible": None, "error": "boom", "screenshot": None}
        return {"text": o[0], "visible": o[1], "error": None, "screenshot": shot}

    runner.run_case = fake_run_case
    cases = [
        {**CASE, "id": "same", "expect_text": "日"},
        {"id": "diff", "name": "Enter", "steps": [{"action": "keys", "keys": ["v", "x", "ENTER"]}]},
        {"id": "hidden", "name": "候選要出現", "checklist": 2, "expect": "候選要出現", "expect_text": "vx", "expect_visible": True,
         "steps": [{"action": "keys", "keys": ["v", "x"]}]},
        {**CASE, "id": "err"},
    ]
    rows = runner.run(cases, [OFFICIAL, LIUKAI])
    assert [r["verdict"] for r in rows] == ["SAME", "DIFF", "DIFF", "ERROR"]
    assert rows[2]["judgement"] == {"official": "✗ 候選列應顯示", "liu-kai": "✓"}
    assert rows[0]["judgement"] == {"official": "✓", "liu-kai": "✓"}
    assert rows[1]["judgement"] == {"official": "—", "liu-kai": "—"}
    assert rows[3]["judgement"]["official"] == "錯誤"
    path = runner.write(rows, [OFFICIAL, LIUKAI], datetime(2026, 9, 26, 20, 0))
    md = path.read_text(encoding="utf-8")
    assert "- 結果：4 個案例；相同 1、不同 2、錯誤 1" in md
    assert "- 符合你的答案（有預期的 2 個案例）：official 1、liu-kai 2" in md
    assert "| same | 空白上屏首選 | 1 | d o SPACE | 「日」 | 「日」·顯示 ✓ | 「日」·顯示 ✓ | SAME |" in md
    assert "| diff | Enter |  | v x ENTER |  | 「vx」·顯示 — | （空）·顯示 — | DIFF |" in md
    assert "| hidden | 候選要出現 | 2 | v x | 「vx」·顯示 | 「vx」·未顯示 ✗ 候選列應顯示 | 「vx」·顯示 ✓ | DIFF |" in md
    assert "（錯誤：boom）" in md and "「日␠」" in md and "[截圖](err-liu-kai.png)" in md
    assert "你的答案：候選要出現" in md
    data = json.loads((tmp_path / "report.json").read_text(encoding="utf-8"))
    assert data["summary"] == {"total": 4, "same": 1, "diff": 2, "error": 1, "expected": 2, "match": {"official": 1, "liu-kai": 2}}
    assert data["targets"] == {"official": OFFICIAL.component, "liu-kai": LIUKAI.component}


def test_judgement_reports_text_mismatch():
    from liu_kai_testpilot.diff import judge

    case = {"expect_text": "日", "expect_visible": True}
    assert judge(case, {"text": "曰", "visible": False, "error": None}) == "✗ 文字應為「日」、候選列應顯示"
    assert judge({"expect_visible": False}, {"text": "", "visible": True, "error": None}) == "✗ 候選列應隱藏"


def _plugin(fake, tmp_path):
    table_dir = tmp_path / "tables"
    table_dir.mkdir(exist_ok=True)
    for name in ("liu_ibus_final.txt", "lime_liu7.txt"):
        (table_dir / name).write_text("x", encoding="utf-8")
    config = DeviceConfig(settle_ms=0, real_table_dir=str(table_dir))
    return SimpleNamespace(adb=fake, config=config, testbed={"reports_dir": str(tmp_path / "reports")})


def test_main_imports_real_table_and_runs_selected_cases(monkeypatch, tmp_path, capsys):
    fake = FakeAdb()
    monkeypatch.setattr(diff, "Plugin", lambda: _plugin(fake, tmp_path))
    monkeypatch.setattr(
        DiffRunner, "run_case", lambda self, case, target: {"text": "日", "visible": True, "error": None, "screenshot": None}
    )
    assert diff.main(["--official", OFFICIAL.component, "--case", "space-first"]) == 0
    out = capsys.readouterr().out
    assert "相同 1、不同 0、錯誤 0" in out
    assert "report.md" in out
    assert fake.of("broadcast")[0][3] == {"source": "files"}
    assert "settings put secure show_ime_with_hard_keyboard 0" in [c[1] for c in fake.of("shell")]


def test_main_exit_code_reflects_errors_and_import_failures(monkeypatch, tmp_path, capsys):
    fake = FakeAdb()
    monkeypatch.setattr(diff, "Plugin", lambda: _plugin(fake, tmp_path))
    monkeypatch.setattr(
        DiffRunner, "run_case", lambda self, case, target: {"text": None, "visible": None, "error": "x", "screenshot": None}
    )
    assert diff.main(["--official", OFFICIAL.component, "--case", "space-first"]) == 1
    fake.shell_outputs["#broadcast"] = [(0, "FAIL 壞掉")]
    assert diff.main(["--official", OFFICIAL.component]) == 1
    assert "liu-kai 匯入真實字表失敗" in capsys.readouterr().out


def test_main_rejects_unknown_case(monkeypatch, tmp_path):
    monkeypatch.setattr(diff, "Plugin", lambda: _plugin(FakeAdb(), tmp_path))
    with pytest.raises(SystemExit):
        diff.main(["--official", OFFICIAL.component, "--case", "nope"])
