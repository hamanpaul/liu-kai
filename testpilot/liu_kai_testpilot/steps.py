"""TestPilot step 動作：以 adb 黑箱驅動模擬器上的 liu-kai 與 testhost。

每個 step 回傳 `{"success", "output", "captured"}`：output 是寫進報告的證據摘要，
captured 供 pass_criteria 判定。任何例外都轉成失敗的 step（附上錯誤），不中斷整批執行。
"""
from __future__ import annotations

import os
import re
import time
from dataclasses import dataclass
from typing import Any, Callable

from liu_kai_testpilot.adb import AdbError
from liu_kai_testpilot.ime_state import ImeState, parse_dump
from liu_kai_testpilot.ui import find_node, is_focused, node_text, screen_rotation

_SYMBOL_KEYS = {
    ",": "COMMA", ".": "PERIOD", "`": "GRAVE", "'": "APOSTROPHE", ";": "SEMICOLON",
    "[": "LEFT_BRACKET", "]": "RIGHT_BRACKET", "-": "MINUS", "=": "EQUALS", "/": "SLASH",
}
MAX_SCROLLS = 8
ROTATE_POLLS = 20
_WINDOW_HEADER = re.compile(r"^\s*Window #\d+ Window\{\S+ u\d+ (.+)\}:\s*$")
_IME_FRAME = re.compile(r"name=\S+ InputMethod, [^\n]*?\bframe=\[-?\d+,(-?\d+)\]")


def keycode(name: str) -> str:
    """`b` → KEYCODE_B、`7` → KEYCODE_7、`,` → KEYCODE_COMMA、`PAGE_DOWN` → KEYCODE_PAGE_DOWN。"""
    return "KEYCODE_" + _SYMBOL_KEYS.get(name, name.upper())


@dataclass
class DeviceConfig:
    repo_root: str = "."
    real_table_dir: str = ""
    real_files: tuple[str, ...] = ("liu_ibus_final.txt", "lime_liu7.txt")
    app_package: str = "com.hamanpaul.liukai"
    ime_component: str = "com.hamanpaul.liukai/.ime.LiuKaiImeService"
    host_component: str = "com.hamanpaul.liukai.testhost/.HostActivity"
    settings_component: str = "com.hamanpaul.liukai/.settings.SettingsActivity"
    import_receiver: str = "com.hamanpaul.liukai/.debug.DebugImportReceiver"
    screen_width: int = 1080
    settle_ms: int = 400

    def repo_path(self, rel: str) -> str:
        return os.path.join(os.path.expanduser(self.repo_root), rel)

    def real_path(self, name: str) -> str:
        return os.path.join(os.path.expanduser(self.real_table_dir), name)


class StepExecutor:
    def __init__(self, adb: Any, config: DeviceConfig, sleep: Callable[[float], None] | None = None) -> None:
        self.adb = adb
        self.config = config
        self._sleep = sleep or time.sleep

    # ---- 共用 ----

    def settle(self) -> None:
        self._sleep(self.config.settle_ms / 1000)

    def state(self) -> ImeState:
        return parse_dump(self.adb.shell(f"dumpsys activity service {self.config.ime_component}"))

    def ui_xml(self) -> str:
        """uiautomator 在畫面動畫中偶爾取不到閒置狀態而失敗，稍候重試（最多 3 次）。"""
        attempt = 0
        while True:
            try:
                return self.adb.shell("uiautomator dump /sdcard/liukai-ui.xml >/dev/null && cat /sdcard/liukai-ui.xml")
            except AdbError:
                attempt += 1
                if attempt == 3:
                    raise
                self.settle()

    def execute(self, step: dict[str, Any]) -> dict[str, Any]:
        action = str(step.get("action"))
        handler = getattr(self, "_do_" + action, None)
        if handler is None:
            return {"success": False, "output": f"未知的 action：{action}", "captured": {}}
        try:
            output, captured = handler(step)
        except Exception as e:  # noqa: BLE001 — 任何錯誤都成為失敗 step 的證據
            return {"success": False, "output": f"{type(e).__name__}: {e}", "captured": {}}
        return {"success": True, "output": output, "captured": captured}

    # ---- 啟動畫面 ----

    def _do_launch_host(self, step):
        self.adb.shell(f"am start -W -S -n {self.config.host_component}")
        self.settle()
        return "launched testhost", {}

    def _do_launch_settings(self, step):
        # 不可用 -S：liu-kai 是目前的輸入法，force-stop 會讓系統改回其他輸入法
        self.adb.shell(f"am start -W -f 0x10008000 -n {self.config.settings_component}")
        self.settle()
        return "launched settings", {}

    # ---- 按鍵與觸控 ----

    def _do_keys(self, step):
        codes = [keycode(k) for k in step["keys"]]
        self.adb.keyevent(*codes)
        self.settle()
        return " ".join(codes), {}

    def _do_text(self, step):
        self.adb.text(step["text"])
        self.settle()
        return f"text {step['text']}", {}

    def _do_combo(self, step):
        codes = [keycode(k) for k in step["keys"]]
        self.adb.keycombination(*codes)
        self.settle()
        return "+".join(codes), {}

    def _do_tap_field(self, step):
        """點輸入欄；指定 x_ratio 時點在欄位寬度的該比例位置（例如 0.02 為最左側，用來移動游標）。"""
        rect = find_node(self.ui_xml(), desc=step["field"]).rect
        x, y = rect.center
        if "x_ratio" in step:
            x = rect.x + int(rect.w * step["x_ratio"])
        self.adb.tap(x, y)
        self.settle()
        return f"tap field {step['field']}", {}

    def _do_tap_text(self, step):
        x, y = find_node(self.ui_xml(), text=step["text"]).rect.center
        self.adb.tap(x, y)
        self.settle()
        return f"tap text {step['text']}", {}

    def _do_long_press_text(self, step):
        x, y = find_node(self.ui_xml(), text=step["text"]).rect.center
        self.adb.long_press(x, y, step.get("duration_ms", 1000))
        self.settle()
        return f"long press text {step['text']}", {}

    def ime_frame_top(self) -> int | None:
        """觸控派送器（`dumpsys input`）中輸入法視窗的 frame 頂端；尚未列入時回傳 None。
        它比 WindowManager 與 View 都晚更新，是觸控能否送達輸入法的依據。"""
        match = _IME_FRAME.search(self.adb.shell("dumpsys input"))
        return None if match is None else int(match.group(1))

    def ime_window_visible(self) -> bool:
        """WindowManager（`dumpsys window windows`）中輸入法視窗是否實際顯示（isVisible=true）。
        候選列有沒有出現在畫面上以此為準，不依賴輸入法自己的狀態，官方與 liu-kai 都適用。"""
        current = None
        for line in self.adb.shell("dumpsys window windows").splitlines():
            header = _WINDOW_HEADER.match(line)
            if header:
                current = header.group(1)
            elif current == "InputMethod" and "isVisible=" in line:
                return "isVisible=true" in line
        return False

    def shown_state(self) -> ImeState:
        """讀取狀態並確認輸入法視窗實際顯示，且觸控派送器的 frame 已與畫面排版一致；
        視窗剛出現或鍵盤區剛顯示時，這段期間的觸控會送不到輸入法。"""
        attempts = 10
        while True:
            state = self.state()
            if not state.window_shown:
                raise LookupError("輸入法視窗未顯示")
            if self.ime_frame_top() == state.candidate_row.y:
                return state
            attempts -= 1
            if attempts == 0:
                raise LookupError("輸入法視窗位置尚未穩定（觸控派送清單中還沒有輸入法視窗，或位置不一致）")
            self._sleep(0.3)

    def _visible_candidate(self, step):
        """候選不在畫面內時往右捲動候選列（最多 MAX_SCROLLS 次）。"""
        scrolls = 0
        while True:
            state = self.shown_state()
            c = state.candidate(text=step.get("text"), index=step.get("index"))
            if c.rect.center_visible(self.config.screen_width):
                return c
            if scrolls == MAX_SCROLLS:
                raise LookupError(f"候選 {c.text} 捲動後仍不在畫面內")
            scrolls += 1
            row = state.candidate_row
            y = row.center[1]
            self.adb.swipe(row.x + row.w * 8 // 10, y, row.x + row.w * 2 // 10, y, 300)
            self.settle()

    def _do_tap_candidate(self, step):
        c = self._visible_candidate(step)
        self.adb.tap(*c.rect.center)
        self.settle()
        return f"tap candidate {c.index}:{c.text}", {}

    def _do_tap_key(self, step):
        self.adb.tap(*self.shown_state().key(step["key"]).center)
        self.settle()
        return f"tap key {step['key']}", {}

    def _do_long_press_key(self, step):
        self.adb.long_press(*self.shown_state().key(step["key"]).center, step.get("duration_ms", 1000))
        self.settle()
        return f"long press key {step['key']}", {}

    # ---- 讀取證據 ----

    def _do_read_field(self, step):
        text = node_text(self.ui_xml(), desc=step["field"])
        return f"{step['field']}={text}", {"text": text}

    def _do_wait_text(self, step):
        """等畫面出現含指定文字的節點（例如系統選單、選檔畫面載入完成），每 0.5 秒檢查一次。"""
        attempts = max(1, step.get("timeout_ms", 5000) // 500)
        while True:
            try:
                text = find_node(self.ui_xml(), text_contains=step["contains"]).text
                return f"found {step['contains']}", {"text": text}
            except LookupError:
                attempts -= 1
                if attempts == 0:
                    raise LookupError(f"等待畫面出現「{step['contains']}」逾時") from None
                self._sleep(0.5)

    def _do_back_until(self, step):
        """按 BACK 直到畫面出現指定文字（例如選檔畫面的 BACK 會先回上一層資料夾）。"""
        presses = 0
        limit = step.get("max_presses", 5)
        while True:
            try:
                return f"found {step['contains']} after {presses} BACK", {"text": find_node(self.ui_xml(), text_contains=step["contains"]).text}
            except LookupError:
                if presses == limit:
                    raise
                self.adb.keyevent("KEYCODE_BACK")
                presses += 1
                self.settle()

    def _do_read_text(self, step):
        text = find_node(self.ui_xml(), text_contains=step["contains"]).text
        return text, {"text": text}

    def _do_ime_window(self, step):
        visible = self.ime_window_visible()
        return f"ime window visible={visible}", {"visible": visible}

    def _do_ime_state(self, step):
        s = self.state()
        captured = {
            "mode": s.mode,
            "table_loaded": s.table_loaded,
            "composing": s.composing,
            "homophone_of": s.homophone_of,
            "window_shown": s.window_shown,
            "keyboard_visible": s.keyboard_visible,
            "failure_hint": s.failure_hint,
            "candidates": s.candidate_texts(),
            "annotations": [c.annotation for c in s.candidates],
        }
        return f"mode={s.mode} composing={s.composing} candidates={s.candidate_texts()}", captured

    # ---- 環境 ----

    def _do_setting(self, step):
        self.adb.shell(f"settings put {step['namespace']} {step['key']} {step['value']}")
        self.settle()
        return f"{step['namespace']}.{step['key']}={step['value']}", {}

    def _do_rotate(self, step):
        """旋轉後宿主 Activity 會重建，重建完成前送出的按鍵會遺失：等畫面轉到指定方向、
        輸入欄（預設 plain）重新取得焦點才繼續，最多檢查 ROTATE_POLLS 次。"""
        rotation = int(step["rotation"])
        field = step.get("field", "plain")
        self.adb.shell("settings put system accelerometer_rotation 0")
        self.adb.shell(f"settings put system user_rotation {rotation}")
        polls = 0
        while True:
            self.settle()
            xml = self.ui_xml()
            if screen_rotation(xml) == rotation and is_focused(xml, field):
                return f"rotation={rotation}", {}
            polls += 1
            if polls == ROTATE_POLLS:
                raise LookupError(f"旋轉後輸入欄 {field} 未重新取得焦點")

    def _do_push_file(self, step):
        with open(self.config.repo_path(step["src"]), "rb") as f:
            data = f.read()
        self.adb.write_file(step["dest"], data)
        return f"push {step['src']} -> {step['dest']} ({len(data)} bytes)", {}

    def _do_wait(self, step):
        self._sleep(step["ms"] / 1000)
        return f"wait {step['ms']}ms", {}

    def _do_shell(self, step):
        out = self.adb.shell(step["command"]).strip()
        return out, {"output": out}

    def _do_ime(self, step):
        if step["enabled"]:
            self.adb.shell(f"ime enable {self.config.ime_component}")
            self.adb.shell(f"ime set {self.config.ime_component}")
        else:
            self.adb.shell(f"ime disable {self.config.ime_component}")
        self.settle()
        return f"ime enabled={step['enabled']}", {}

    # ---- 字表 ----

    def _import_files(self, paths: list[str]) -> str:
        for path in paths:
            with open(path, "rb") as f:
                self.adb.run_as_write(self.config.app_package, f"files/import/{os.path.basename(path)}", f.read())
        _, data = self.adb.broadcast("com.hamanpaul.liukai.DEBUG_IMPORT", self.config.import_receiver, {"source": "files"})
        return data

    def _do_import_table(self, step):
        source = step["source"]
        if source == "real":
            result = self._import_files([self.config.real_path(n) for n in self.config.real_files])
        elif source == "files":
            result = self._import_files([self.config.repo_path(p) for p in step["files"]])
        elif source == "corrupt":
            self.adb.run_as_write(self.config.app_package, "files/table.liutb", b"not a liu-kai table")
            result = "corrupt"
        elif source == "none":
            self.adb.shell(f"run-as {self.config.app_package} rm -f files/table.liutb")
            result = "removed"
        else:
            _, result = self.adb.broadcast("com.hamanpaul.liukai.DEBUG_IMPORT", self.config.import_receiver, {"source": source})
        self.settle()
        return f"import {source}: {result}", {"result": result}
