"""adb 包裝：固定 binary 與 serial，runner 可注入以便在沒有裝置時測試。"""
from __future__ import annotations

import re
import shlex
import subprocess
import time
from typing import Callable, NamedTuple


class CompletedCall(NamedTuple):
    returncode: int
    stdout: bytes
    stderr: bytes


class AdbError(RuntimeError):
    pass


Runner = Callable[[list[str], "bytes | None", float], CompletedCall]

# 模擬器偶發的暫時性錯誤：等待裝置恢復後重試
_TRANSIENT = ("device offline", "' not found", "no devices/emulators found")
_MAX_ATTEMPTS = 3

_BROADCAST_RESULT = re.compile(r'Broadcast completed: result=(-?\d+)(?:, data="(.*)")?')


def _subprocess_runner(argv: list[str], stdin: bytes | None, timeout: float) -> CompletedCall:
    proc = subprocess.run(argv, input=stdin, capture_output=True, timeout=timeout)
    return CompletedCall(proc.returncode, proc.stdout, proc.stderr)


def _quote(text: str) -> str:
    """shell 單引號跳脫；`input text` 以 %s 表示空白。"""
    return "'" + text.replace(" ", "%s").replace("'", "'\"'\"'") + "'"


class Adb:
    def __init__(
        self,
        binary: str,
        serial: str,
        runner: Runner | None = None,
        path_mapper: Callable[[str], str] = str,
        sleep: Callable[[float], None] = time.sleep,
    ) -> None:
        self.binary = binary
        self.serial = serial
        self._runner = runner or _subprocess_runner
        self._path_mapper = path_mapper
        self._sleep = sleep

    def run(self, args: list[str], stdin: bytes | None = None, timeout: float = 60) -> CompletedCall:
        attempt = 0
        while True:
            result = self._runner([self.binary, "-s", self.serial, *args], stdin, timeout)
            if result.returncode == 0:
                return result
            error = result.stderr.decode(errors="replace").strip()
            attempt += 1
            if attempt == _MAX_ATTEMPTS or not any(t in error for t in _TRANSIENT):
                raise AdbError(f"adb {' '.join(args)} 失敗：{error}")
            self._sleep(2)
            self._runner([self.binary, "-s", self.serial, "wait-for-device"], None, 120)

    def shell(self, command: str, timeout: float = 60) -> str:
        out = self.run(["shell", command], timeout=timeout).stdout
        return out.decode("utf-8", errors="replace").replace("\r\n", "\n")

    def keyevent(self, *codes: str) -> None:
        self.shell("input keyevent " + " ".join(codes))

    def keycombination(self, *codes: str) -> None:
        self.shell("input keycombination " + " ".join(codes))

    def text(self, text: str) -> None:
        self.shell("input text " + _quote(text))

    def tap(self, x: int, y: int) -> None:
        self.shell(f"input tap {x} {y}")

    def long_press(self, x: int, y: int, duration_ms: int) -> None:
        self.swipe(x, y, x, y, duration_ms)

    def swipe(self, x1: int, y1: int, x2: int, y2: int, duration_ms: int) -> None:
        self.shell(f"input swipe {x1} {y1} {x2} {y2} {duration_ms}")

    def motionevent(self, action: str, x: int, y: int) -> None:
        """單一觸控事件（DOWN／MOVE／UP），用來按住按鍵檢查按住時的畫面。"""
        self.shell(f"input motionevent {action} {x} {y}")

    def broadcast(self, action: str, component: str, extras: dict[str, str]) -> tuple[int, str | None]:
        args = "".join(f" --es {k} {v}" for k, v in extras.items())
        out = self.shell(f"am broadcast -a {action} -n {component}{args}")
        match = _BROADCAST_RESULT.search(out)
        return int(match.group(1)), match.group(2)

    def run_as_write(self, package: str, path: str, data: bytes) -> None:
        folder = path.rsplit("/", 1)[0]
        self.run(["exec-in", "run-as", package, "sh", "-c", f"mkdir -p {folder} && cat > {path}"], stdin=data)

    def run_as_read(self, package: str, path: str) -> bytes:
        return self.run(["exec-out", "run-as", package, "cat", path]).stdout

    def write_file(self, path: str, data: bytes) -> None:
        """以 shell 身分寫入裝置上的檔案（例如 /sdcard/Download，供 SAF 選檔）。"""
        self.run(["exec-in", "sh", "-c", f"cat > {shlex.quote(path)}"], stdin=data)

    def screencap(self) -> bytes:
        return self.run(["exec-out", "screencap", "-p"]).stdout

    def screencap_raw(self) -> bytes:
        """原始 RGBA 截圖（無 PNG 壓縮）：標頭為寬、高、格式（Android 12+ 另有 dataspace）。"""
        return self.run(["exec-out", "screencap"]).stdout

    def install(self, apk_path: str) -> None:
        self.run(["install", "-r", "-t", self._path_mapper(apk_path)], timeout=300)
