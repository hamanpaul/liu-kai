from pathlib import Path

import pytest

from liu_kai_testpilot.steps import DeviceConfig, StepExecutor, keycode

from .conftest import UI_XML, cand, ime_window_block, state_dump, window_dump

DUMPSYS = "dumpsys activity service"
UI_DUMP = "uiautomator dump"


@pytest.fixture
def ex(adb, tmp_path):
    config = DeviceConfig(repo_root=str(tmp_path), real_table_dir=str(tmp_path / "data"), settle_ms=0)
    return StepExecutor(adb, config, sleep=lambda s: None)


def run(ex, **step):
    return ex.execute({"id": "s", "target": "EMU", **step})


def test_keycode_names():
    assert keycode("b") == "KEYCODE_B"
    assert keycode("7") == "KEYCODE_7"
    assert keycode(",") == "KEYCODE_COMMA"
    assert keycode("`") == "KEYCODE_GRAVE"
    assert keycode("PAGE_DOWN") == "KEYCODE_PAGE_DOWN"


def test_keys_text_and_combo(ex, adb):
    assert run(ex, action="keys", keys=["b", "a", "SPACE"])["success"] is True
    run(ex, action="text", text="a?")
    run(ex, action="combo", keys=["CTRL_LEFT", "J"])
    assert adb.of("keyevent") == [("keyevent", ("KEYCODE_B", "KEYCODE_A", "KEYCODE_SPACE"))]
    assert adb.of("text") == [("text", "a?")]
    assert adb.of("keycombination") == [("keycombination", ("KEYCODE_CTRL_LEFT", "KEYCODE_J"))]


def test_launch_host_and_settings_do_not_force_stop_ime(ex, adb):
    run(ex, action="launch_host")
    run(ex, action="launch_settings")
    shells = [c[1] for c in adb.of("shell")]
    assert shells[0] == "am start -W -S -n com.hamanpaul.liukai.testhost/.HostActivity"
    assert shells[1] == "am start -W -f 0x10008000 -n com.hamanpaul.liukai/.settings.SettingsActivity"


def test_tap_field_and_read_field(ex, adb):
    adb.queue(UI_DUMP, UI_XML.format(plain="日月"))
    run(ex, action="tap_field", field="plain")
    result = run(ex, action="read_field", field="plain")
    assert adb.of("tap") == [("tap", 540, 180)]
    assert result["captured"] == {"text": "日月"}
    assert result["output"] == "plain=日月"


def test_read_text_and_tap_text(ex, adb):
    adb.queue(UI_DUMP, UI_XML.format(plain=""))
    assert run(ex, action="read_text", contains="輸入法：")["captured"] == {"text": "輸入法：已啟用／使用中"}
    run(ex, action="tap_text", text="清除字表")
    assert adb.of("tap") == [("tap", 540, 660)]


def test_ime_state_captures_summary(ex, adb):
    adb.queue(DUMPSYS, state_dump(composing="ba", candidates=[cand(0, "日", 200), cand(1, "月", 300, "ㄩㄝˋ")]))
    result = run(ex, action="ime_state")
    assert result["captured"] == {
        "mode": "CHINESE",
        "table_loaded": True,
        "composing": "ba",
        "homophone_of": None,
        "window_shown": True,
        "keyboard_visible": False,
        "failure_hint": False,
        "candidates": ["日", "月"],
        "annotations": [None, "ㄩㄝˋ"],
    }
    assert "日" in result["output"]


def test_tap_candidate_by_text_and_index(ex, adb):
    adb.queue(DUMPSYS, state_dump(candidates=[cand(0, "日", 200), cand(1, "月", 300)]))
    run(ex, action="tap_candidate", text="月")
    run(ex, action="tap_candidate", index=0)
    assert adb.of("tap") == [("tap", 350, 2275), ("tap", 250, 2275)]
    # 長按候選沒有功能（對齊官方），不提供這個動作
    assert run(ex, action="long_press_candidate", text="日")["output"] == "未知的 action：long_press_candidate"


def test_tap_candidate_scrolls_row_until_visible(ex, adb):
    far = state_dump(candidates=[cand(0, "日", 200), cand(15, "丑", 1500)])
    near = state_dump(candidates=[cand(0, "日", -900), cand(15, "丑", 600)])
    adb.queue(DUMPSYS, far, near)
    run(ex, action="tap_candidate", text="丑")
    assert adb.of("swipe") == [("swipe", 864, 2275, 216, 2275, 300)]
    assert adb.of("tap") == [("tap", 650, 2275)]


def test_tap_candidate_gives_up_after_max_scrolls(ex, adb):
    adb.queue(DUMPSYS, state_dump(candidates=[cand(15, "丑", 5000)]))
    result = run(ex, action="tap_candidate", text="丑")
    assert result["success"] is False
    assert "捲動後仍不在畫面內" in result["output"]
    assert len(adb.of("swipe")) == 8


def test_soft_keys(ex, adb):
    keys = {"b": {"x": 500, "y": 2300, "w": 100, "h": 100}, "backspace": {"x": 900, "y": 2300, "w": 100, "h": 100}}
    adb.queue(DUMPSYS, state_dump(keyboardVisible=True, keys=keys))
    run(ex, action="tap_key", key="b")
    run(ex, action="long_press_key", key="backspace", duration_ms=1500)
    assert adb.of("tap") == [("tap", 550, 2350)]
    assert adb.of("long_press") == [("long_press", 950, 2350, 1500)]


ROTATED = UI_XML.format(plain="").replace('rotation="0"', 'rotation="1"')
FOCUSED = 'content-desc="plain" focused="true"'


def test_setting_rotate_wait_and_shell(ex, adb):
    adb.queue("echo", "hi\n")
    adb.queue(UI_DUMP, ROTATED.replace('content-desc="plain"', FOCUSED))
    run(ex, action="setting", namespace="secure", key="show_ime_with_hard_keyboard", value="1")
    assert run(ex, action="rotate", rotation=1)["output"] == "rotation=1"
    run(ex, action="wait", ms=10)
    assert run(ex, action="shell", command="echo hi")["captured"] == {"output": "hi"}
    shells = [c[1] for c in adb.of("shell")]
    assert shells[:3] == [
        "settings put secure show_ime_with_hard_keyboard 1",
        "settings put system accelerometer_rotation 0",
        "settings put system user_rotation 1",
    ]


def test_import_table_demo_and_unknown_source(ex, adb):
    demo = run(ex, action="import_table", source="demo")
    assert demo["captured"] == {"result": "OK TRADITIONAL raw=31"}
    assert adb.of("broadcast")[0][3] == {"source": "demo"}
    bogus = run(ex, action="import_table", source="bogus")
    assert adb.of("broadcast")[1][3] == {"source": "bogus"}
    assert bogus["success"] is True


def test_import_table_real_and_listed_files(ex, adb, tmp_path):
    data = tmp_path / "data"
    data.mkdir()
    (data / "liu_ibus_final.txt").write_bytes(b"IBUS")
    (data / "lime_liu7.txt").write_bytes(b"LIME")
    (tmp_path / "only.txt").write_bytes(b"ONE")
    run(ex, action="import_table", source="real")
    run(ex, action="import_table", source="files", files=["only.txt"])
    assert adb.written == {
        "files/import/liu_ibus_final.txt": b"IBUS",
        "files/import/lime_liu7.txt": b"LIME",
        "files/import/only.txt": b"ONE",
    }
    assert [c[3] for c in adb.of("broadcast")] == [{"source": "files"}, {"source": "files"}]


def test_import_table_empty_files_list_broadcasts_without_writing(ex, adb):
    run(ex, action="import_table", source="files", files=[])
    assert adb.written == {}
    assert adb.of("broadcast")[0][3] == {"source": "files"}


def test_corrupt_and_remove_table(ex, adb):
    assert run(ex, action="import_table", source="corrupt")["captured"] == {"result": "corrupt"}
    assert adb.written == {"files/table.liutb": b"not a liu-kai table"}
    assert run(ex, action="import_table", source="none")["captured"] == {"result": "removed"}
    assert adb.of("shell")[-1][1] == "run-as com.hamanpaul.liukai rm -f files/table.liutb"


def test_ime_enable_disable(ex, adb):
    run(ex, action="ime", enabled=False)
    run(ex, action="ime", enabled=True)
    shells = [c[1] for c in adb.of("shell")]
    assert shells == [
        "ime disable com.hamanpaul.liukai/.ime.LiuKaiImeService",
        "ime enable com.hamanpaul.liukai/.ime.LiuKaiImeService",
        "ime set com.hamanpaul.liukai/.ime.LiuKaiImeService",
    ]


def test_errors_become_failed_step_with_evidence(ex, adb):
    adb.queue(DUMPSYS, "no state here")
    result = run(ex, action="ime_state")
    assert result["success"] is False
    assert "LIUKAI_STATE" in result["output"]
    adb.queue(UI_DUMP, UI_XML.format(plain=""))
    adb.fail_on = "input"
    failed = run(ex, action="tap_text", text="清除字表")
    assert failed["success"] is False
    assert failed["output"].startswith("AdbError: boom")
    assert run(ex, action="launch_host")["success"] is True


def test_unknown_action_fails():
    ex = StepExecutor(None, DeviceConfig(repo_root="."), sleep=lambda s: None)
    result = ex.execute({"id": "s", "action": "fly"})
    assert result == {"success": False, "output": "未知的 action：fly", "captured": {}}


def test_default_sleep_uses_time(monkeypatch, adb):
    slept = []
    monkeypatch.setattr("time.sleep", slept.append)
    ex = StepExecutor(adb, DeviceConfig(repo_root=".", settle_ms=250))
    ex.execute({"id": "s", "action": "wait", "ms": 100})
    assert slept == [0.1]


def test_device_config_paths_are_expanded(tmp_path):
    config = DeviceConfig(repo_root="~", real_table_dir="~/data")
    assert Path(config.repo_path("x")) == Path.home() / "x"
    assert Path(config.real_path("a.txt")) == Path.home() / "data" / "a.txt"


def test_push_file_writes_repo_file_to_device(ex, adb, tmp_path):
    (tmp_path / "bad.txt").write_bytes(b"hello")
    result = run(ex, action="push_file", src="bad.txt", dest="/sdcard/Download/bad.txt")
    assert result["output"] == "push bad.txt -> /sdcard/Download/bad.txt (5 bytes)"
    assert adb.of("write_file") == [("write_file", "/sdcard/Download/bad.txt", b"hello")]


def test_tap_field_at_horizontal_ratio(ex, adb):
    adb.queue(UI_DUMP, UI_XML.format(plain="ba"))
    run(ex, action="tap_field", field="plain", x_ratio=0.02)
    assert adb.of("tap") == [("tap", 21, 180)]


def test_long_press_text(ex, adb):
    adb.queue(UI_DUMP, UI_XML.format(plain=""))
    run(ex, action="long_press_text", text="清除字表", duration_ms=1200)
    assert adb.of("long_press") == [("long_press", 540, 660, 1200)]


def test_ui_dump_is_retried_when_uiautomator_fails(ex, adb):
    adb.queue(UI_DUMP, UI_XML.format(plain="日"))
    adb.fail_on = "uiautomator"
    attempts = []
    original = adb._check

    def flaky(command):
        if command.startswith("uiautomator"):
            attempts.append(command)
            if len(attempts) < 3:
                original(command)

    adb._check = flaky
    assert run(ex, action="read_field", field="plain")["captured"] == {"text": "日"}
    assert len(attempts) == 3


def test_ui_dump_gives_up_after_three_failures(ex, adb):
    adb.fail_on = "uiautomator"
    result = run(ex, action="read_field", field="plain")
    assert result["success"] is False
    assert len([c for c in adb.of("shell") if c[1].startswith("uiautomator")]) == 3


def test_taps_on_ime_fail_fast_when_window_hidden(ex, adb):
    keys = {"b": {"x": 500, "y": 2300, "w": 100, "h": 100}}
    adb.queue(DUMPSYS, state_dump(windowShown=False, keys=keys, candidates=[cand(0, "日", 200)]))
    for step in ({"action": "tap_key", "key": "b"}, {"action": "tap_candidate", "text": "日"}):
        result = run(ex, **step)
        assert result["success"] is False
        assert "輸入法視窗未顯示" in result["output"]
    assert adb.of("tap") == []


def test_wait_text_polls_until_text_appears(ex, adb):
    adb.queue(UI_DUMP, "<hierarchy/>", "<hierarchy/>", UI_XML.format(plain=""))
    result = run(ex, action="wait_text", contains="清除字表", timeout_ms=5000)
    assert result == {"success": True, "output": "found 清除字表", "captured": {"text": "清除字表"}}


def test_wait_text_times_out(ex, adb):
    adb.queue(UI_DUMP, "<hierarchy/>")
    result = run(ex, action="wait_text", contains="不會出現", timeout_ms=1000)
    assert result["success"] is False
    assert "不會出現" in result["output"]


def test_ime_state_captures_window_shown(ex, adb):
    adb.queue(DUMPSYS, state_dump(windowShown=False))
    assert run(ex, action="ime_state")["captured"]["window_shown"] is False


def test_ime_taps_wait_until_window_frame_matches_layout(ex, adb):
    keys = {"b": {"x": 500, "y": 2300, "w": 100, "h": 100}}
    adb.queue(DUMPSYS, state_dump(keys=keys))
    adb.shell_outputs["dumpsys input"] = [window_dump(2264), window_dump(2264), window_dump(2200)]
    run(ex, action="tap_key", key="b")
    assert adb.of("tap") == [("tap", 550, 2350)]
    assert len([c for c in adb.of("shell") if c[1].startswith("dumpsys input")]) == 3


def test_ime_taps_fail_when_window_frame_never_settles(ex, adb):
    adb.queue(DUMPSYS, state_dump(keys={"b": {"x": 500, "y": 2300, "w": 100, "h": 100}}))
    adb.shell_outputs["dumpsys input"] = [window_dump(2264)]
    result = run(ex, action="tap_key", key="b")
    assert result["success"] is False
    assert "位置尚未穩定" in result["output"]
    assert adb.of("tap") == []


def test_ime_frame_missing_from_window_dump_fails(ex, adb):
    adb.queue(DUMPSYS, state_dump(keys={"b": {"x": 500, "y": 2300, "w": 100, "h": 100}}))
    adb.shell_outputs["dumpsys input"] = ["no ime window here"]
    assert "觸控派送清單中還沒有輸入法視窗" in run(ex, action="tap_key", key="b")["output"]


def test_back_until_presses_back_until_text_appears(ex, adb):
    adb.queue(UI_DUMP, "<hierarchy/>", "<hierarchy/>", UI_XML.format(plain=""))
    result = run(ex, action="back_until", contains="清除字表")
    assert result["captured"] == {"text": "清除字表"}
    assert adb.of("keyevent") == [("keyevent", ("KEYCODE_BACK",)), ("keyevent", ("KEYCODE_BACK",))]


def test_back_until_gives_up(ex, adb):
    adb.queue(UI_DUMP, "<hierarchy/>")
    result = run(ex, action="back_until", contains="不會出現", max_presses=2)
    assert result["success"] is False
    assert len(adb.of("keyevent")) == 2


def test_rotate_waits_until_host_is_rebuilt_and_field_refocused(ex, adb):
    # 舊方向仍有焦點 → 新方向重建中（輸入欄尚未取得焦點）→ 完成
    adb.queue(
        UI_DUMP,
        UI_XML.format(plain="").replace('content-desc="plain"', FOCUSED),
        ROTATED,
        ROTATED.replace('content-desc="plain"', FOCUSED),
    )
    assert run(ex, action="rotate", rotation=1)["success"] is True
    assert len([c for c in adb.of("shell") if c[1].startswith(UI_DUMP)]) == 3


def test_rotate_gives_up_when_field_never_refocuses(ex, adb):
    adb.queue(UI_DUMP, ROTATED)
    result = run(ex, action="rotate", rotation=1, field="search")
    assert result["success"] is False
    assert "search 未重新取得焦點" in result["output"]


def test_ime_window_visibility_from_window_manager(ex, adb):
    adb.queue("dumpsys window windows", ime_window_block(True), ime_window_block(False), "  Window #1 Window{x u0 Other}:\n")
    assert run(ex, action="ime_window")["captured"] == {"visible": True}
    assert run(ex, action="ime_window")["output"] == "ime window visible=False"
    assert ex.ime_window_visible() is False
