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
    }
    state.update(overrides)
    payload = base64.b64encode(json.dumps(state, ensure_ascii=False).encode()).decode()
    return f"SERVICE x\n  LIUKAI_STATE {payload}\n"


def window_dump(top: int) -> str:
    """`dumpsys input` 的視窗清單（觸控派送實際使用的 frame）。"""
    return (
        "  Windows:\n"
        "      4: name=a1b2c3 com.x/.Host, id=127, displayId=0, frame=[0,0][1080,2400], touchableRegion=[0,0][1080,2400]\n"
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


class FakeAdb:
    """假 adb：shell 依指令前綴回傳排好的輸出（佇列），並記錄所有呼叫。"""

    def __init__(self):
        self.calls = []
        self.shell_outputs = {}
        self.files = {}
        self.written = {}
        self.fail_on = None

    def queue(self, prefix, *outputs):
        self.shell_outputs.setdefault(prefix, []).extend(outputs)

    def _check(self, command):
        if self.fail_on and command.startswith(self.fail_on):
            from liu_kai_testpilot.adb import AdbError

            raise AdbError(f"boom: {command}")

    def shell(self, command, timeout=60):
        self.calls.append(("shell", command))
        self._check(command)
        for prefix, outputs in self.shell_outputs.items():
            if command.startswith(prefix) and outputs:
                return outputs.pop(0) if len(outputs) > 1 else outputs[0]
        return ""

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

    def of(self, kind):
        return [c for c in self.calls if c[0] == kind]


@pytest.fixture
def adb():
    fake = FakeAdb()
    # 預設：輸入法視窗 frame 頂端與 state_dump 的候選列一致（位置已穩定）
    fake.queue("dumpsys input", window_dump(2200))
    return fake
