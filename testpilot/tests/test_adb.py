import subprocess

import pytest

from liu_kai_testpilot.adb import Adb, AdbError, CompletedCall


class FakeRunner:
    """記錄呼叫並回傳預先排好的結果（依序）。"""

    def __init__(self, *results):
        self.calls = []
        self.results = list(results)

    def __call__(self, argv, stdin, timeout):
        self.calls.append((argv, stdin, timeout))
        return self.results.pop(0) if self.results else CompletedCall(0, b"", b"")


def ok(out=b""):
    return CompletedCall(0, out, b"")


def test_shell_prefixes_binary_and_serial_and_strips_crlf():
    runner = FakeRunner(ok(b"line1\r\nline2\r\n"))
    adb = Adb("adb.exe", "emulator-5580", runner=runner)
    assert adb.shell("getprop sys.boot_completed") == "line1\nline2\n"
    assert runner.calls[0][0] == ["adb.exe", "-s", "emulator-5580", "shell", "getprop sys.boot_completed"]


def test_non_transient_error_raises_immediately_with_stderr():
    runner = FakeRunner(CompletedCall(1, b"", b"run-as: package not debuggable"))
    with pytest.raises(AdbError, match="package not debuggable"):
        Adb("adb", "s", runner=runner).shell("true")
    assert len(runner.calls) == 1


def test_key_and_input_helpers_build_input_commands():
    runner = FakeRunner()
    adb = Adb("adb", "s", runner=runner)
    adb.keyevent("KEYCODE_B", "KEYCODE_A")
    adb.keycombination("KEYCODE_CTRL_LEFT", "KEYCODE_J")
    adb.text("a?*!")
    adb.tap(10, 20)
    adb.long_press(10, 20, 900)
    adb.swipe(100, 20, 10, 20, 300)
    adb.motionevent("DOWN", 10, 20)
    shells = [c[0][-1] for c in runner.calls]
    assert shells == [
        "input keyevent KEYCODE_B KEYCODE_A",
        "input keycombination KEYCODE_CTRL_LEFT KEYCODE_J",
        "input text 'a?*!'",
        "input tap 10 20",
        "input swipe 10 20 10 20 900",
        "input swipe 100 20 10 20 300",
        "input motionevent DOWN 10 20",
    ]


def test_text_escapes_single_quote_and_space():
    runner = FakeRunner()
    Adb("adb", "s", runner=runner).text("it's a")
    assert runner.calls[0][0][-1] == "input text 'it'\"'\"'s%sa'"


def test_broadcast_returns_result_data_and_code():
    out = b'Broadcasting: Intent { act=X }\r\nBroadcast completed: result=1, data="OK TRADITIONAL raw=3"\r\n'
    runner = FakeRunner(ok(out))
    code, data = Adb("adb", "s", runner=runner).broadcast("X", "pkg/.R", {"source": "demo"})
    assert (code, data) == (1, "OK TRADITIONAL raw=3")
    assert runner.calls[0][0][-1] == "am broadcast -a X -n pkg/.R --es source demo"


def test_broadcast_without_data_returns_none():
    runner = FakeRunner(ok(b"Broadcast completed: result=0\r\n"))
    assert Adb("adb", "s", runner=runner).broadcast("X", "pkg/.R", {}) == (0, None)


def test_run_as_write_streams_bytes_via_exec_in_and_read_via_exec_out():
    runner = FakeRunner(ok(), ok(b"\x00\x01binary"))
    adb = Adb("adb", "s", runner=runner)
    adb.run_as_write("com.x", "files/import/a.txt", b"data")
    assert adb.run_as_read("com.x", "files/coverage.ec") == b"\x00\x01binary"
    write_argv, write_stdin, _ = runner.calls[0]
    assert write_argv == ["adb", "-s", "s", "exec-in", "run-as", "com.x", "sh", "-c",
                          "mkdir -p files/import && cat > files/import/a.txt"]
    assert write_stdin == b"data"
    assert runner.calls[1][0] == ["adb", "-s", "s", "exec-out", "run-as", "com.x", "cat", "files/coverage.ec"]


def test_install_maps_path_and_passes_flags():
    runner = FakeRunner(ok(b"Success\r\n"))
    adb = Adb("adb", "s", runner=runner, path_mapper=lambda p: "C:\\\\t\\\\" + p.split("/")[-1])
    adb.install("/tmp/x/app-debug.apk")
    assert runner.calls[0][0] == ["adb", "-s", "s", "install", "-r", "-t", "C:\\\\t\\\\app-debug.apk"]


def test_default_runner_uses_subprocess(monkeypatch):
    seen = {}

    def fake_run(argv, input, capture_output, timeout):
        seen.update(argv=argv, input=input, capture_output=capture_output, timeout=timeout)
        return subprocess.CompletedProcess(argv, 0, b"out", b"err")

    monkeypatch.setattr(subprocess, "run", fake_run)
    result = Adb("adb", "s").run(["devices"], timeout=5)
    assert result == CompletedCall(0, b"out", b"err")
    assert seen == {"argv": ["adb", "-s", "s", "devices"], "input": None, "capture_output": True, "timeout": 5}


def test_write_file_streams_to_device_path_as_shell_user():
    runner = FakeRunner()
    Adb("adb", "s", runner=runner).write_file("/sdcard/Download/a b.txt", b"x")
    argv, stdin, _ = runner.calls[0]
    assert argv == ["adb", "-s", "s", "exec-in", "sh", "-c", "cat > '/sdcard/Download/a b.txt'"]
    assert stdin == b"x"


def test_transient_device_errors_are_retried_after_waiting():
    runner = FakeRunner(CompletedCall(1, b"", b"adb.exe: device offline"), ok(), ok(b"1\n"))
    slept = []
    adb = Adb("adb", "s", runner=runner, sleep=slept.append)
    assert adb.shell("getprop x") == "1\n"
    assert [c[0][3:] for c in runner.calls] == [["shell", "getprop x"], ["wait-for-device"], ["shell", "getprop x"]]
    assert slept == [2]


def test_transient_errors_give_up_after_three_attempts():
    offline = CompletedCall(1, b"", b"error: device 's' not found")
    runner = FakeRunner(offline, ok(), offline, ok(), offline)
    with pytest.raises(AdbError, match="not found"):
        Adb("adb", "s", runner=runner, sleep=lambda s: None).shell("true")
    assert len(runner.calls) == 5


def test_screencap_reads_png_bytes():
    runner = FakeRunner(ok(b"\x89PNG"))
    assert Adb("adb", "s", runner=runner).screencap() == b"\x89PNG"
    assert runner.calls[0][0] == ["adb", "-s", "s", "exec-out", "screencap", "-p"]


def test_screencap_raw_reads_rgba_bytes():
    runner = FakeRunner(ok(b"RAW"))
    assert Adb("adb", "s", runner=runner).screencap_raw() == b"RAW"
    assert runner.calls[0][0] == ["adb", "-s", "s", "exec-out", "screencap"]
