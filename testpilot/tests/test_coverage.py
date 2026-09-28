from liu_kai_testpilot import coverage
from liu_kai_testpilot.plugin import Plugin

from .test_plugin import TESTBED


def test_dump_coverage_broadcasts_twice_and_pulls_file(adb, tmp_path):
    adb.files["files/coverage.ec"] = b"\x01\x02exec"
    out = tmp_path / "out" / "e2e.ec"
    size = coverage.dump_coverage(adb, "com.hamanpaul.liukai", str(out))
    assert size == 6
    assert out.read_bytes() == b"\x01\x02exec"
    assert [c[1] for c in adb.of("broadcast")] == ["com.hamanpaul.liukai.DUMP_COVERAGE"] * 2
    assert adb.of("broadcast")[0][2] == "com.hamanpaul.liukai/.debug.CoverageDumpReceiver"


def test_main_uses_plugin_testbed(adb, tmp_path, monkeypatch, capsys):
    adb.files["files/coverage.ec"] = b"exec"
    monkeypatch.setattr(coverage, "Plugin", lambda: Plugin(testbed=TESTBED, adb=adb))
    out = tmp_path / "e2e.ec"
    assert coverage.main(["--out", str(out)]) == 0
    assert out.read_bytes() == b"exec"
    assert "4 bytes" in capsys.readouterr().out
