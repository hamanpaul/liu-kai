import base64
import json

import pytest


def state_dump(**overrides) -> str:
    state = {
        "mode": "CHINESE",
        "tableLoaded": True,
        "composing": "",
        "homophoneOf": None,
        "keyboardVisible": False,
        "failureHint": False,
        "windowShown": True,
        "candidateRow": {"x": 0, "y": 2200, "w": 1080, "h": 150},
        "candidates": [],
        "keys": {},
        "layer": "letters",
        "rows": [],
        "enterLabel": "↵",
        "popup": [],
        "rowRects": [],
        "rowColors": [],
        "probe": {"x": 4, "y": 2204, "color": "#000000"},
        "rowLabels": [],
        "strip": [],
        "language": "TRADITIONAL",
        "languages": ["TRADITIONAL"],
        "codeHint": None,
    }
    state.update(overrides)
    payload = base64.b64encode(json.dumps(state, ensure_ascii=False).encode()).decode()
    return f"SERVICE x\n  LIUKAI_STATE {payload}\n"


FOCUS = "  mCurrentFocus=Window{a1b2c3 u0 com.x/.Host}\n"


def window_dump(top: int, host_config: str = "0x0") -> str:
    """`dumpsys input` 的視窗清單（觸控派送實際使用的 frame 與 inputConfig）。"""
    return (
        "  Windows:\n"
        f"      4: name=a1b2c3 com.x/.Host, id=127, displayId=0, inputConfig={host_config}, frame=[0,0][1080,2400], touchableRegion=[0,0][1080,2400]\n"
        f"      5: name=18de9a7 InputMethod, id=128, displayId=0, inputConfig=NOT_FOCUSABLE, alpha=1, frame=[0,{top}][1080,2400], touchableRegion=[0,{top}][1080,2400]\n"
        "    535: channelName='18de9a7 InputMethod (server)', status=NORMAL\n"
    )


def ime_window_block(visible: bool) -> str:
    """`dumpsys window windows` 中輸入法視窗與前後視窗的區塊。"""
    flag = "true" if visible else "false"
    return (
        "  Window #5 Window{a1 u0 com.x/.Host}:\n    isOnScreen=true\n    isVisible=true\n"
        f"  Window #6 Window{{ad04fb2 u0 InputMethod}}:\n    mViewVisibility=0x0\n    isOnScreen={flag}\n    isVisible={flag}\n"
        "  Window #7 Window{b2 u0 StatusBar}:\n    isVisible=true\n"
    )


def cand(index, text, x, annotation=None):
    return {"index": index, "text": text, "annotation": annotation, "x": x, "y": 2200, "w": 100, "h": 150}


UI_XML = """<?xml version='1.0' encoding='UTF-8' standalone='yes' ?>
<hierarchy rotation="0">
  <node index="0" text="" content-desc="" bounds="[0,0][1080,2400]">
    <node index="0" text="{plain}" content-desc="plain" bounds="[0,120][1080,240]" />
    <node index="1" text="清除字表" content-desc="" bounds="[40,600][1040,720]" />
    <node index="2" text="輸入法：已啟用／使用中" content-desc="" bounds="[40,60][1040,120]" />
  </node>
</hierarchy>"""


def raw_screen(color, width=8, height=2400, header=16):
    """Android `screencap` 原始格式：寬、高、格式（Android 12+ 另有 dataspace）＋ RGBA 像素。"""
    head = width.to_bytes(4, "little") + height.to_bytes(4, "little") + (1).to_bytes(4, "little")
    head += b"\0" * (header - 12)
    return head + bytes([*color, 255]) * (width * height)


class FakeAdb:
    """假 adb：shell 依指令前綴回傳排好的輸出（佇列），並記錄所有呼叫。"""

    def __init__(self):
        self.calls = []
        self.shell_outputs = {}
        self.files = {}
        self.written = {}
        self.fail_on = None
        self.screens = [raw_screen((0, 0, 0))]

    def queue(self, prefix, *outputs):
        self.shell_outputs.setdefault(prefix, []).extend(outputs)

    def _check(self, command):
        if self.fail_on and command.startswith(self.fail_on):
            from liu_kai_testpilot.adb import AdbError

            raise AdbError(f"boom: {command}")

    def shell(self, command, timeout=60):
        self.calls.append(("shell", command))
        self._check(command)
        # 最長相符的前綴優先（例如 `dumpsys input_method` 不被 `dumpsys input` 搶走）
        matches = [p for p, outputs in self.shell_outputs.items() if command.startswith(p) and outputs]
        if not matches:
            return ""
        outputs = self.shell_outputs[max(matches, key=len)]
        return self._with_touches(outputs.pop(0) if len(outputs) > 1 else outputs[0])

    def _with_touches(self, output):
        """輸入法狀態沒指定 touches 時，填入目前為止的觸控次數（模擬輸入法立即處理完每次點擊）。"""
        head, marker, payload = output.partition("LIUKAI_STATE ")
        if not marker:
            return output
        payload, newline, rest = payload.partition("\n")
        state = json.loads(base64.b64decode(payload))
        state.setdefault("touches", len([c for c in self.calls if c[0] in ("tap", "long_press", "swipe")]))
        encoded = base64.b64encode(json.dumps(state, ensure_ascii=False).encode()).decode()
        return head + marker + encoded + newline + rest

    def keyevent(self, *codes):
        self._check("input keyevent")
        self.calls.append(("keyevent", codes))

    def keycombination(self, *codes):
        self._check("input keycombination")
        self.calls.append(("keycombination", codes))

    def text(self, text):
        self._check("input text")
        self.calls.append(("text", text))

    def tap(self, x, y):
        self._check("input tap")
        self.calls.append(("tap", x, y))

    def long_press(self, x, y, ms):
        self._check("input swipe")
        self.calls.append(("long_press", x, y, ms))

    def swipe(self, x1, y1, x2, y2, ms):
        self._check("input swipe")
        self.calls.append(("swipe", x1, y1, x2, y2, ms))

    def broadcast(self, action, component, extras):
        self.calls.append(("broadcast", action, component, extras))
        return self.shell_outputs.get("#broadcast", [(1, "OK TRADITIONAL raw=31")])[0]

    def run_as_write(self, package, path, data):
        self.calls.append(("run_as_write", package, path))
        self.written[path] = data

    def run_as_read(self, package, path):
        self.calls.append(("run_as_read", package, path))
        return self.files[path]

    def write_file(self, path, data):
        self.calls.append(("write_file", path, data))

    def install(self, apk):
        self.calls.append(("install", apk))

    def screencap(self):
        self.calls.append(("screencap",))
        return b"PNG"

    def screencap_raw(self):
        """`adb exec-out screencap`（原始 RGBA）：依序回傳 self.screens，最後一個重複使用；預設全黑。"""
        self.calls.append(("screencap_raw",))
        if len(self.screens) > 1:
            return self.screens.pop(0)
        return self.screens[0]

    def of(self, kind):
        return [c for c in self.calls if c[0] == kind]


@pytest.fixture
def adb():
    fake = FakeAdb()
    # 預設：輸入法視窗 frame 頂端與 state_dump 的候選列一致（位置已穩定）
    fake.queue("dumpsys window |", FOCUS)
    fake.queue("dumpsys input", window_dump(2200))
    return fake
