import subprocess
import time

import pytest
import yaml

from liu_kai_testpilot.plugin import Plugin, load_testbed
from liu_kai_testpilot.runner import LiuKaiRunner

from .conftest import UI_XML, state_dump

DUMPSYS = "dumpsys activity service"
UI_DUMP = "uiautomator dump"

TESTBED = {"adb_binary": "adb", "serial": "emulator-5580", "path_mapper": "none", "repo_root": ".", "settle_ms": 0}


@pytest.fixture
def plugin(adb):
    p = Plugin(testbed=TESTBED, adb=adb, sleep=lambda s: None)
    adb.queue(UI_DUMP, UI_XML.format(plain=""))
    adb.queue(DUMPSYS, state_dump())
    return p


def case(**extra):
    base = {
        "id": "c1",
        "name": "sample",
        "topology": {"devices": {"EMU": {"role": "dut", "transport": "adb"}}},
        "steps": [{"id": "read", "action": "read_field", "target": "EMU", "field": "plain"}],
        "pass_criteria": [{"field": "read.text", "operator": "equals", "value": ""}],
    }
    base.update(extra)
    return base


def test_plugin_contract():
    p = Plugin(testbed=TESTBED)
    assert (p.name, p.version, p.api_version) == ("liu_kai", "0.1.0", "1.2")
    assert p.report_formats() == ["json", "md"]
    assert isinstance(p.create_runner(), LiuKaiRunner)
    assert p._sleep is time.sleep


def test_bundled_cases_are_schema_valid_and_unique():
    cases = Plugin(testbed=TESTBED).discover_cases()
    ids = [c["id"] for c in cases]
    assert len(cases) >= 20
    assert len(ids) == len(set(ids))
    for c in cases:
        assert c["pass_criteria"], c["id"]
        assert all(step["id"] for step in c["steps"])


def test_load_testbed_from_env_file(tmp_path, monkeypatch):
    path = tmp_path / "tb.yaml"
    path.write_text(yaml.safe_dump({**TESTBED, "screen_width": 720}))
    monkeypatch.setenv("LIU_KAI_TESTBED", str(path))
    tb = load_testbed()
    assert tb["screen_width"] == 720
    assert Plugin().config.screen_width == 720


def test_load_testbed_missing_file_explains(tmp_path, monkeypatch):
    monkeypatch.setenv("LIU_KAI_TESTBED", str(tmp_path / "nope.yaml"))
    with pytest.raises(FileNotFoundError, match="testbed.yaml.example"):
        load_testbed()


def test_default_testbed_path_is_next_to_package(monkeypatch, tmp_path):
    from liu_kai_testpilot import plugin as plugin_module

    assert (plugin_module.DEFAULT_TESTBED.parent.name, plugin_module.DEFAULT_TESTBED.name) == ("testpilot", "testbed.yaml")
    monkeypatch.delenv("LIU_KAI_TESTBED", raising=False)
    monkeypatch.setattr(plugin_module, "DEFAULT_TESTBED", tmp_path / "testpilot" / "testbed.yaml")
    with pytest.raises(FileNotFoundError, match="testbed.yaml"):
        load_testbed()


def test_adb_is_built_from_testbed_with_wslpath_mapper(monkeypatch):
    calls = []

    def fake_check_output(argv):
        calls.append(argv)
        return b"C:\\\\x.apk\n"

    monkeypatch.setattr(subprocess, "check_output", fake_check_output)
    adb = Plugin(testbed={**TESTBED, "path_mapper": "wslpath"}).adb
    assert (adb.binary, adb.serial) == ("adb", "emulator-5580")
    assert adb._path_mapper("/tmp/x.apk") == "C:\\\\x.apk"
    assert calls == [["wslpath", "-w", "/tmp/x.apk"]]
    assert Plugin(testbed=TESTBED).adb._path_mapper("/a") == "/a"


def test_setup_imports_table_once_and_prepares_host(plugin, adb):
    assert plugin.setup_env(case(preconditions={"table": "demo"}), None) is True
    assert plugin.setup_env(case(id="c2", preconditions={"table": "demo"}), None) is True
    assert len(adb.of("broadcast")) == 1
    shells = [c[1] for c in adb.of("shell")]
    assert "settings put secure show_ime_with_hard_keyboard 0" in shells
    assert any(s.startswith("am start -W -S -n com.hamanpaul.liukai.testhost") for s in shells)
    assert adb.of("tap") == [("tap", 540, 180), ("tap", 540, 180)]


def test_setup_soft_keyboard_and_other_launch_targets(plugin, adb):
    plugin.setup_env(case(preconditions={"table": "demo", "keyboard": "soft", "launch": "settings"}), None)
    plugin.setup_env(case(preconditions={"table": "demo", "launch": "none"}), None)
    shells = [c[1] for c in adb.of("shell")]
    assert "settings put secure show_ime_with_hard_keyboard 1" in shells
    assert any("SettingsActivity" in s for s in shells)
    assert adb.of("tap") == []


def test_setup_restores_chinese_mode(adb):
    p = Plugin(testbed=TESTBED, adb=adb, sleep=lambda s: None)
    adb.queue(UI_DUMP, UI_XML.format(plain=""))
    adb.queue(DUMPSYS, state_dump(mode="ENGLISH"), state_dump())
    p.setup_env(case(), None)
    p.setup_env(case(id="c2"), None)
    assert adb.of("keyevent") == [("keyevent", ("KEYCODE_SHIFT_LEFT",))]


def test_setup_fails_when_a_precondition_step_fails(plugin, adb):
    adb.fail_on = "am start"
    assert plugin.setup_env(case(), None) is False
    assert "AdbError" in plugin.evidence["c1"]["setup"][-1]["output"]


def test_verify_env_checks_ime_is_reachable(plugin, adb):
    assert plugin.verify_env(case(), None) is True
    assert plugin.verify_env(case(preconditions={"launch": "none"}), None) is True
    adb.shell_outputs[DUMPSYS] = ["no state"]
    assert plugin.verify_env(case(), None) is False


def test_execute_evaluate_and_evidence(plugin, adb):
    c = case()
    plugin.setup_env(c, None)
    result = plugin.execute_step(c, c["steps"][0], None)
    assert result["captured"] == {"text": ""}
    assert plugin.evaluate(c, {"steps": {"read": result}}) is True
    evidence = plugin.evidence["c1"]
    assert evidence["steps"][0]["id"] == "read"
    assert evidence["criteria"] == ["PASS read.text equals ''（實際 ''）"]


def test_teardown_resets_environment_and_table_when_mutated(plugin, adb):
    c = case(preconditions={"table": "demo"}, mutates_table=True)
    plugin.setup_env(c, None)
    plugin.executor.rotation = 1
    plugin.teardown(c, None)
    shells = [x[1] for x in adb.of("shell")]
    # 螢幕鍵盤設定由每個案例的 setup 設定；teardown 不改回，避免反覆觸發框架的設定監聽延遲
    assert shells[-1] == "settings put system user_rotation 0"
    assert plugin.executor.rotation == 0
    assert shells.count("settings put secure show_ime_with_hard_keyboard 0") == 1
    assert adb.of("keyevent")[-1] == ("keyevent", ("KEYCODE_ESCAPE",))
    plugin.setup_env(case(id="c2", preconditions={"table": "demo"}), None)
    assert len(adb.of("broadcast")) == 2


def test_teardown_survives_device_errors(plugin, adb):
    adb.fail_on = "settings"
    plugin.teardown(case(), None)


def test_setup_without_focus_skips_tap_and_mode_check(plugin, adb):
    assert plugin.setup_env(case(preconditions={"table": "demo", "focus": False}), None) is True
    assert adb.of("tap") == []
    assert not any(c[1].startswith("dumpsys") for c in adb.of("shell"))
    assert plugin.verify_env(case(preconditions={"focus": False}), None) is True


def test_setup_retaps_field_until_ime_window_shows(adb):
    p = Plugin(testbed=TESTBED, adb=adb, sleep=lambda s: None)
    adb.queue(UI_DUMP, UI_XML.format(plain=""))
    adb.queue(DUMPSYS, state_dump(windowShown=False), state_dump(windowShown=True))
    assert p.setup_env(case(), None) is True
    assert len(adb.of("tap")) == 2


def test_setup_fails_when_ime_window_never_shows(adb):
    p = Plugin(testbed=TESTBED, adb=adb, sleep=lambda s: None)
    adb.queue(UI_DUMP, UI_XML.format(plain=""))
    adb.queue(DUMPSYS, state_dump(windowShown=False))
    assert p.setup_env(case(), None) is False
    assert len(adb.of("tap")) == 3
    assert "輸入法視窗未顯示" in p.evidence["c1"]["setup"][-1]["output"]


def test_setup_waits_for_keyboard_area_to_follow_setting(adb):
    # 框架的「實體鍵盤時顯示螢幕鍵盤」設定監聽會延遲數秒，鍵盤區跟上之前觸控會點不到按鍵
    slept = []
    p = Plugin(testbed=TESTBED, adb=adb, sleep=slept.append)
    adb.queue(UI_DUMP, UI_XML.format(plain=""))
    adb.queue(DUMPSYS, state_dump(keyboardVisible=False), state_dump(keyboardVisible=False), state_dump(keyboardVisible=True))
    assert p.setup_env(case(preconditions={"keyboard": "soft"}), None) is True
    assert slept.count(0.5) == 2
    assert len(adb.of("tap")) == 1


def test_setup_fails_when_keyboard_area_never_follows_setting(adb):
    p = Plugin(testbed=TESTBED, adb=adb, sleep=lambda s: None)
    adb.queue(UI_DUMP, UI_XML.format(plain=""))
    adb.queue(DUMPSYS, state_dump(keyboardVisible=False))
    assert p.setup_env(case(preconditions={"keyboard": "soft"}), None) is False
    assert "鍵盤區未跟上設定" in p.evidence["c1"]["setup"][-1]["output"]


def test_setup_applies_prefs_before_launch_and_teardown_resets(plugin, adb):
    c = case(preconditions={"table": "demo", "prefs": {"number_row": "true", "theme": "WHITE"}})
    assert plugin.setup_env(c, None) is True
    extras = [b[3] for b in adb.of("broadcast")]
    assert {"key": "number_row", "value": "true"} in extras
    assert {"key": "theme", "value": "WHITE"} in extras
    # 偏好在啟動 testhost 之前設定：輸入法在顯示鍵盤時讀取
    pref_at = max(i for i, c in enumerate(adb.calls) if c[0] == "broadcast")
    launch_at = next(i for i, c in enumerate(adb.calls) if c[0] == "shell" and c[1].startswith("am start -W -S"))
    assert pref_at < launch_at
    plugin.teardown(c, None)
    assert adb.of("broadcast")[-1][3] == {"reset": "true"}


def test_teardown_without_prefs_does_not_reset_prefs(plugin, adb):
    plugin.teardown(case(), None)
    assert all(b[3] != {"reset": "true"} for b in adb.of("broadcast"))


def test_teardown_runs_case_cleanup_steps(plugin, adb):
    # 案例會留下檔案（例如加字加詞）時以 cleanup 清除；即使案例中途失敗 teardown 也會執行
    c = case(cleanup=[{"action": "shell", "command": "run-as com.hamanpaul.liukai rm -f files/user_phrases.tsv"}])
    plugin.teardown(c, None)
    assert adb.of("shell")[-1] == ("shell", "run-as com.hamanpaul.liukai rm -f files/user_phrases.tsv")
