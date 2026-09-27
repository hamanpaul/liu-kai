"""解析 liu-kai IME 在 `dumpsys activity service` 輸出的 `LIUKAI_STATE <base64 JSON>`。"""
from __future__ import annotations

import base64
import json
from dataclasses import dataclass, field


@dataclass(frozen=True)
class Rect:
    x: int
    y: int
    w: int
    h: int

    @property
    def center(self) -> tuple[int, int]:
        return self.x + self.w // 2, self.y + self.h // 2

    def center_visible(self, screen_width: int) -> bool:
        return 0 <= self.center[0] < screen_width


def _rect(d: dict) -> Rect:
    return Rect(d["x"], d["y"], d["w"], d["h"])


@dataclass(frozen=True)
class CandidateView:
    index: int
    text: str
    annotation: str | None
    rect: Rect


@dataclass(frozen=True)
class ImeState:
    mode: str
    table_loaded: bool
    composing: str
    homophone_of: str | None
    window_shown: bool
    keyboard_visible: bool
    failure_hint: bool
    candidate_row: Rect
    candidates: list[CandidateView] = field(default_factory=list)
    keys: dict[str, Rect] = field(default_factory=dict)
    # 螢幕鍵盤：目前的層（letters／symbols／alt）、各排按鍵 id、Enter 鍵標籤、長按彈出的按鍵 id
    layer: str = "letters"
    rows: list[list[str]] = field(default_factory=list)
    enter_label: str = ""
    popup: list[str] = field(default_factory=list)
    # 各排按鍵格的螢幕範圍 [x, y, w, h] 與底色（#RRGGBB），供與官方量測值比對
    row_rects: list[list[list[int]]] = field(default_factory=list)
    row_colors: list[list[str]] = field(default_factory=list)
    # 探測點 (x, y, #RRGGBB)：輸入法畫面上一定是這個顏色的位置，用來確認鍵盤真的畫在螢幕上
    probe: tuple[int, int, str] = (0, 0, "#000000")
    # 輸入法已處理完的觸控次數：點擊後等它增加，確認輸入法處理完才進行下一步
    touches: int = 0
    # 各排按鍵顯示的標籤（以圖示顯示的鍵為其代表字元）、候選列的常用標點（punct:X）
    row_labels: list[list[str]] = field(default_factory=list)
    strip: list[str] = field(default_factory=list)

    def candidate(self, text: str | None = None, index: int | None = None) -> CandidateView:
        for c in self.candidates:
            if (text is None or c.text == text) and (index is None or c.index == index):
                return c
        raise LookupError(f"找不到候選 text={text} index={index}；目前候選：{self.candidate_texts()}")

    def candidate_texts(self) -> list[str]:
        return [c.text for c in self.candidates]

    def key(self, key_id: str) -> Rect:
        if key_id not in self.keys:
            raise LookupError(f"找不到按鍵 key:{key_id}")
        return self.keys[key_id]


def parse_dump(output: str) -> ImeState:
    for line in output.splitlines():
        marker, _, payload = line.strip().partition(" ")
        if marker == "LIUKAI_STATE":
            d = json.loads(base64.b64decode(payload))
            return ImeState(
                mode=d["mode"],
                table_loaded=d["tableLoaded"],
                composing=d["composing"],
                homophone_of=d["homophoneOf"],
                # 輸入法視窗實際是否顯示（View 的狀態可能與視窗不一致）
                window_shown=d["windowShown"],
                keyboard_visible=d["keyboardVisible"],
                failure_hint=d["failureHint"],
                candidate_row=_rect(d["candidateRow"]),
                candidates=[CandidateView(c["index"], c["text"], c["annotation"], _rect(c)) for c in d["candidates"]],
                keys={k: _rect(v) for k, v in d["keys"].items()},
                layer=d["layer"],
                rows=d["rows"],
                enter_label=d["enterLabel"],
                popup=d["popup"],
                row_rects=d["rowRects"],
                row_colors=d["rowColors"],
                probe=(d["probe"]["x"], d["probe"]["y"], d["probe"]["color"]),
                touches=d["touches"],
                row_labels=d["rowLabels"],
                strip=d["strip"],
            )
    raise ValueError("dumpsys 輸出中沒有 LIUKAI_STATE（輸入法未啟動或不是 liu-kai？）")
