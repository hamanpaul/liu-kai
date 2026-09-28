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
TOUCH_POLLS = 20
BIND_POLLS = 20
SHOWN_POLLS = 30
FIELD_POLLS = 6
STATE_POLLS = 12
BACK_CHECKS = 3
WINDOW_POLLS = 20
READ_POLLS = 6
ACK_POLLS = 20
UNTIL_CHECKS = 6
TAP_TRIES = 3
UI_DUMP_TRIES = 6
# 只操作／讀取輸入法、不會讓輸入法視窗收起或重新顯示的動作；其他動作之後，下一次點鍵盤要重新確認已畫在畫面上
KEEPS_IME_DRAWN = frozenset({"tap_key", "long_press_key", "tap_candidate", "ime_state", "read_field"})
_FOCUS = re.compile(r"mCurrentFocus=Window\{([0-9a-f]+) ")
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
    pref_receiver: str = "com.hamanpaul.liukai/.debug.DebugPrefReceiver"
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
        # 已確認畫在畫面上的輸入法視窗位置（候選列 y）；截圖很慢，同一次顯示只確認一次
        self._drawn_at: int | None = None
        # rotate 步驟設定的螢幕方向（0 直式）；plugin 的 teardown 轉回直式後歸零
        self.rotation = 0

    # ---- 共用 ----

    def settle(self) -> None:
        self._sleep(self.config.settle_ms / 1000)

    def state(self) -> ImeState:
        """輸入法狀態；旋轉等情況下服務重建中暫時沒有 LIUKAI_STATE，稍候重試（最多 STATE_POLLS 次）。"""
        polls = 0
        while True:
            try:
                return parse_dump(self.adb.shell(f"dumpsys activity service {self.config.ime_component}"))
            except ValueError:
                polls += 1
                if polls == STATE_POLLS:
                    raise
                self._sleep(0.5)

    def ui_xml(self) -> str:
        """uiautomator 在畫面動畫中偶爾取不到閒置狀態或根節點而失敗，每 0.5 秒重試（最多 UI_DUMP_TRIES 次）。"""
        attempt = 0
        while True:
            try:
                # 讀完就刪：uiautomator 偶爾印出錯誤（null root node）卻沒寫檔，留著舊檔會讀到上一個畫面
                return self.adb.shell(
                    "uiautomator dump /sdcard/liukai-ui.xml >/dev/null && cat /sdcard/liukai-ui.xml && rm /sdcard/liukai-ui.xml"
                )
            except AdbError:
                attempt += 1
                if attempt == UI_DUMP_TRIES:
                    raise
                self._sleep(0.5)

    def execute(self, step: dict[str, Any]) -> dict[str, Any]:
        action = str(step.get("action"))
        if action not in KEEPS_IME_DRAWN:
            self._drawn_at = None
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
        # 等 UI 樹換成設定頁（「清除字表」按鈕一定在）；剛切換時擷取到的可能還是前一個畫面
        self._find(text="清除字表")
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

    def _find(self, **query):
        """找畫面上的節點；切換畫面後的 UI 擷取可能還是前一個畫面，稍候重試（最多 FIELD_POLLS 次）。"""
        polls = 0
        while True:
            try:
                return find_node(self.ui_xml(), **query)
            except LookupError:
                polls += 1
                if polls == FIELD_POLLS:
                    raise
                self._sleep(0.5)

    def _do_tap_field(self, step):
        """點輸入欄；指定 x_ratio 時點在欄位寬度的該比例位置（例如 0.02 為最左側，用來移動游標）。"""
        rect = self._find(desc=step["field"]).rect
        x, y = rect.center
        if "x_ratio" in step:
            x = rect.x + int(rect.w * step["x_ratio"])
        self.adb.tap(x, y)
        self.settle()
        return f"tap field {step['field']}", {}

    def _focused_window_touchable(self) -> bool:
        focus = _FOCUS.search(self.adb.shell("dumpsys window | grep mCurrentFocus"))
        if focus is None:
            return False
        for line in self.adb.shell("dumpsys input").splitlines():
            if f"name={focus.group(1)} " in line:
                return "NOT_VISIBLE" not in line and "NOT_TOUCHABLE" not in line
        return False

    def ime_bound(self, component: str) -> bool:
        """`dumpsys input_method`：目前輸入法為 component，且已接上 testhost 的輸入欄。"""
        dump = self.adb.shell("dumpsys input_method")
        host = self.config.host_component.split("/")[0]
        return f"mCurId={component} " in dump and "mBoundToMethod=true" in dump and f"packageName={host} " in dump

    def _do_await_bound(self, step):
        """等系統把 testhost 的輸入欄接上 liu-kai：App 剛啟動時輸入法還沒接上，這時的按鍵送不到輸入法。"""
        polls = 0
        while not self.ime_bound(self.config.ime_component):
            polls += 1
            if polls == BIND_POLLS:
                raise LookupError("輸入法未接上 testhost 的輸入欄")
            self._sleep(0.5)
        return f"ime bound to {self.config.host_component.split('/')[0]}", {}

    def await_touchable(self) -> None:
        """等目前取得焦點的視窗可接收觸控（觸控派送清單中沒有 NOT_VISIBLE／NOT_TOUCHABLE）。
        Activity 剛啟動或剛從其他畫面返回時有轉場期，這時的點擊會被丟掉；模擬器負載高時轉場會拉長。"""
        polls = 0
        while not self._focused_window_touchable():
            polls += 1
            if polls == TOUCH_POLLS:
                raise LookupError("取得焦點的視窗一直無法接收觸控")
            self._sleep(0.5)

    def _do_tap_text(self, step):
        """點畫面上的文字。指定 until 時確認點擊生效（畫面出現 until 文字）：系統忙碌時點擊可能被丟掉
        （InputDispatcher：No new touched window），等不到且原本的文字還在畫面上就再點一次（最多 TAP_TRIES 次）；
        原本的文字已不在畫面上表示畫面已切換，不再點，交給下一步判定。"""
        tries = 0
        while True:
            self.await_touchable()
            x, y = self._find(text=step["text"]).rect.center
            self.adb.tap(x, y)
            self.settle()
            tries += 1
            if "until" not in step or self._poll_text(step["until"], UNTIL_CHECKS) is not None:
                return f"tap text {step['text']}", {}
            if tries == TAP_TRIES:
                raise LookupError(f"點了 {tries} 次「{step['text']}」仍未出現「{step['until']}」")
            if self._poll_text(step["text"]) is None:
                return f"tap text {step['text']}（畫面已切換）", {}

    def _do_long_press_text(self, step):
        self.await_touchable()
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

    def pixel(self, x: int, y: int) -> tuple[int, int, int]:
        """螢幕上一點的顏色（以原始 RGBA 截圖讀取，不需影像函式庫）。"""
        raw = self.adb.screencap_raw()
        width = int.from_bytes(raw[0:4], "little")
        height = int.from_bytes(raw[4:8], "little")
        offset = len(raw) - width * height * 4 + (y * width + x) * 4
        return raw[offset], raw[offset + 1], raw[offset + 2]

    def _drawn(self, state: ImeState) -> bool:
        x, y, color = state.probe
        expected = (int(color[1:3], 16), int(color[3:5], 16), int(color[5:7], 16))
        return all(abs(a - b) <= 8 for a, b in zip(self.pixel(x, y), expected))

    def shown_state(self) -> ImeState:
        """讀取狀態並確認輸入法視窗實際顯示：觸控派送器的 frame 已與畫面排版一致（直式時），且鍵盤真的畫在螢幕上
        （探測點為輸入法畫面的顏色）。視窗剛出現、鍵盤區剛顯示，或剛切換「實體鍵盤時顯示螢幕鍵盤」設定時，
        輸入法視窗可能已「顯示」但畫面上還看不到，這段期間的觸控會送不到輸入法。
        截圖很慢（約 1.6 秒），連續操作鍵盤時只在第一次確認；視窗移動或執行過其他動作後重新確認。"""
        attempts = SHOWN_POLLS
        while True:
            state = self.state()
            if not state.window_shown:
                raise LookupError("輸入法視窗未顯示")
            y = state.candidate_row.y
            # 觸控派送器的 frame 是實體（未旋轉）座標：橫式時不比對，只以截圖探測點確認
            framed = self.rotation != 0 or self.ime_frame_top() == y
            if framed and (self._drawn_at == y or self._drawn(state)):
                self._drawn_at = y
                return state
            attempts -= 1
            if attempts == 0:
                raise LookupError("輸入法視窗位置尚未穩定（觸控派送清單中還沒有輸入法視窗、位置不一致，或尚未畫在畫面上）")
            self._sleep(0.3)

    def _visible_candidate(self, step):
        """候選不在畫面內時往右捲動候選列（最多 MAX_SCROLLS 次）；回傳 (狀態, 候選)。"""
        scrolls = 0
        while True:
            state = self.shown_state()
            c = state.candidate(text=step.get("text"), index=step.get("index"))
            if c.rect.center_within(state.candidate_row):
                return state, c
            if scrolls == MAX_SCROLLS:
                raise LookupError(f"候選 {c.text} 捲動後仍不在畫面內")
            scrolls += 1
            row = state.candidate_row
            y = row.center[1]
            self.adb.swipe(row.x + row.w * 8 // 10, y, row.x + row.w * 2 // 10, y, 300)
            self.settle()

    def _await_touch_handled(self, before: ImeState) -> None:
        """等輸入法處理完這次觸控（狀態中的觸控計數增加，最多 ACK_POLLS 次）。輸入法主執行緒忙碌時點擊還在佇列中，
        若接著就讀狀態或點下一鍵，會讀到舊狀態，或因按下與放開之間隔太久而被當成長按。"""
        polls = 0
        while self.state().touches == before.touches:
            polls += 1
            if polls == ACK_POLLS:
                raise LookupError("輸入法沒有處理這次點擊")
            self._sleep(0.3)
        self.settle()

    def _do_tap_candidate(self, step):
        state, c = self._visible_candidate(step)
        self.adb.tap(*c.rect.center)
        self._await_touch_handled(state)
        return f"tap candidate {c.index}:{c.text}", {}

    def _do_tap_key(self, step):
        """點螢幕鍵盤的鍵並等輸入法處理完；ack: false 時不等（例如語音鍵會切換到其他輸入法）。"""
        state = self.shown_state()
        self.adb.tap(*state.key(step["key"]).center)
        if step.get("ack", True):
            self._await_touch_handled(state)
        else:
            self.settle()
        return f"tap key {step['key']}", {}

    def _do_touch_key(self, step):
        """按住（phase: down）或放開（phase: up）螢幕鍵盤的鍵，用來檢查按住時的畫面（例如按鍵放大預覽）。
        放開時等輸入法處理完。"""
        state = self.shown_state()
        x, y = state.key(step["key"]).center
        if step["phase"] == "down":
            self.adb.motionevent("DOWN", x, y)
            self.settle()
        else:
            self.adb.motionevent("UP", x, y)
            self._await_touch_handled(state)
        return f"touch {step['phase']} {step['key']}", {}

    def _do_swipe_key(self, step):
        """從鍵的中心水平滑動 dx 像素（例如中文模式空白鍵左右滑動切換語言模式），並等輸入法處理完。"""
        state = self.shown_state()
        x, y = state.key(step["key"]).center
        self.adb.swipe(x, y, x + step["dx"], y, step.get("duration_ms", 800))
        self._await_touch_handled(state)
        return f"swipe {step['key']} dx={step['dx']}", {}

    def _do_long_press_key(self, step):
        state = self.shown_state()
        self.adb.long_press(*state.key(step["key"]).center, step.get("duration_ms", 1000))
        self._await_touch_handled(state)
        return f"long press key {step['key']}", {}

    # ---- 讀取證據 ----

    def _do_read_field(self, step):
        """讀輸入欄文字。上屏時編輯器會先刪除組字再插入，UI 擷取可能抓到中間的空字串，
        因此連續兩次讀到相同內容才採用（最多 READ_POLLS 次，逾時採用最後一次）。"""
        previous = None
        reads = 0
        while True:
            text = node_text(self.ui_xml(), desc=step["field"])
            reads += 1
            if text == previous or reads == READ_POLLS:
                return f"{step['field']}={text}", {"text": text}
            previous = text
            self._sleep(0.5)

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

    def _poll_text(self, contains: str, limit: int = BACK_CHECKS):
        """畫面上含指定文字的節點；按鍵後 UI 擷取可能還是前一個畫面，檢查 limit 次仍沒有才回傳 None。"""
        checks = 0
        while True:
            try:
                return find_node(self.ui_xml(), text_contains=contains)
            except LookupError:
                checks += 1
                if checks == limit:
                    return None
                self._sleep(0.5)

    def _do_back_until(self, step):
        """按 BACK 直到畫面出現指定文字（例如選檔畫面的 BACK 會先回上一層資料夾）；每次按之前先確認畫面換好，
        避免因 UI 擷取還是舊畫面而多按一次、離開 App。"""
        presses = 0
        limit = step.get("max_presses", 5)
        while True:
            node = self._poll_text(step["contains"])
            if node is not None:
                return f"found {step['contains']} after {presses} BACK", {"text": node.text}
            if presses == limit:
                raise LookupError(f"按了 {limit} 次 BACK 仍未出現「{step['contains']}」")
            self.adb.keyevent("KEYCODE_BACK")
            presses += 1
            self.settle()

    def _do_read_text(self, step):
        text = find_node(self.ui_xml(), text_contains=step["contains"]).text
        return text, {"text": text}

    def _do_ime_window(self, step):
        """輸入法視窗是否實際顯示；指定 expect 時輪詢到符合或逾時（WINDOW_POLLS 次），回傳最後的狀態。"""
        polls = 0
        while True:
            visible = self.ime_window_visible()
            if "expect" not in step or visible == step["expect"] or polls == WINDOW_POLLS:
                return f"ime window visible={visible}", {"visible": visible}
            polls += 1
            self._sleep(0.5)

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
            "candidate_row_y": s.candidate_row.y,
            "layer": s.layer,
            "rows": s.rows,
            "enter_label": s.enter_label,
            "popup": s.popup,
            "row_rects": s.row_rects,
            "row_colors": s.row_colors,
            "row_labels": s.row_labels,
            "strip": s.strip,
            "language": s.language,
            "code_hint": s.code_hint,
            "shift": s.shift,
            "preview": s.preview,
            "feedback": s.feedback,
            "blank": s.blank,
            "space_underline": s.space_underline,
            "palette": s.palette,
            "font_scale": s.font_scale,
            "row_height": s.row_height,
            "candidate_strip": [s.candidate_row.y, s.candidate_row.h],
            "candidates": s.candidate_texts(),
            "annotations": [c.annotation for c in s.candidates],
            "candidate_bold": [c.bold for c in s.candidates],
            "candidate_colors": [c.color for c in s.candidates],
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
                self.rotation = rotation
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

    def _do_pref(self, step):
        """設定輸入法偏好（debug 版的 DebugPrefReceiver）：key／value 設定一項，reset: true 回到預設值。
        輸入法在下一次顯示鍵盤時讀取設定。"""
        extras = {"reset": "true"} if step.get("reset") else {"key": step["key"], "value": str(step["value"])}
        code, data = self.adb.broadcast("com.hamanpaul.liukai.DEBUG_PREF", self.config.pref_receiver, extras)
        if code != 1:
            raise RuntimeError(f"設定偏好失敗：{data}")
        return data, {}

    def _do_current_ime(self, step):
        """目前的輸入法；指定 expect 時等它成為目前輸入法（例如語音鍵切換到語音輸入，最多 WINDOW_POLLS 次）。"""
        polls = 0
        while True:
            ime = self.adb.shell("settings get secure default_input_method").strip()
            if "expect" not in step or ime == step["expect"]:
                return f"ime={ime}", {"ime": ime}
            polls += 1
            if polls == WINDOW_POLLS:
                raise LookupError(f"目前輸入法一直不是 {step['expect']}（實際 {ime}）")
            self._sleep(0.5)

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
        # 先清空暫存匯入目錄：前一次匯入若中斷，殘留的檔案會混進這次匯入
        self.adb.shell(f"run-as {self.config.app_package} rm -rf files/import")
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
            # 同時標記內建字表已套用過，否則下次載入會自動改用 APK 內建字表
            self.adb.shell(f"run-as {self.config.app_package} sh -c 'rm -f files/table.liutb && touch files/table.seeded'")
            result = "removed"
        elif source == "bundled":
            # 等同全新安裝：下次載入時改用 APK 內建字表
            self.adb.shell(f"run-as {self.config.app_package} rm -f files/table.liutb files/table.seeded")
            result = "reset to bundled"
        else:
            _, result = self.adb.broadcast("com.hamanpaul.liukai.DEBUG_IMPORT", self.config.import_receiver, {"source": source})
        self.settle()
        return f"import {source}: {result}", {"result": result}
