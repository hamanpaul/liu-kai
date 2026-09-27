import base64
import json

import pytest

from liu_kai_testpilot.ime_state import ImeState, Rect, parse_dump


def dump_with(state: dict) -> str:
    payload = base64.b64encode(json.dumps(state, ensure_ascii=False).encode()).decode()
    return f"SERVICE com.hamanpaul.liukai/.ime.LiuKaiImeService pid=1\n  mWindowCreated=true\n  LIUKAI_STATE {payload}\n"


STATE = {
    "mode": "CHINESE",
    "tableLoaded": True,
    "composing": "ba",
    "homophoneOf": None,
    "keyboardVisible": True,
    "failureHint": True,
    "windowShown": True,
    "candidateRow": {"x": 0, "y": 2000, "w": 1080, "h": 150},
    "candidates": [
        {"index": 0, "text": "日", "annotation": None, "x": 200, "y": 2000, "w": 100, "h": 150},
        {"index": 1, "text": "月", "annotation": "ㄩㄝˋ", "x": 300, "y": 2000, "w": 100, "h": 150},
    ],
    "keys": {"b": {"x": 500, "y": 2300, "w": 90, "h": 140}},
    "layer": "symbols",
    "rows": [["1", "2"], ["alt", "backspace"]],
    "enterLabel": "Next",
    "popup": ["popup:!", "popup:?"],
    "rowRects": [[[7, 1722, 95, 140]], [[61, 1879, 95, 140]]],
    "rowColors": [["#FDFDFE"], ["#F4F5F7"]],
    "probe": {"x": 4, "y": 2004, "color": "#000000"},
    "touches": 7,
    "rowLabels": [["1", "2"], ["ALT", "⌫"]],
    "strip": ["punct:!", "punct:?"],
    "language": "JAPANESE",
    "codeHint": "忠 qa",
    "shift": "locked",
    "preview": "g",
    "feedback": {"vibrate": 2, "sound": 1},
    "dimmed": ["b", "c"],
    "hidden": ["homophone"],
    "palette": {"bg": "#ECEFF1", "cells": False},
    "fontScale": 1.2,
    "rowHeight": 96,
    "languages": ["TRADITIONAL", "JAPANESE"],
}


def test_parse_dump_decodes_state_line():
    state = parse_dump(dump_with(STATE))
    assert state.mode == "CHINESE"
    assert state.table_loaded is True
    assert state.composing == "ba"
    assert state.homophone_of is None
    assert state.keyboard_visible is True
    assert state.failure_hint is True
    assert (state.layer, state.rows, state.enter_label, state.popup) == (
        "symbols", [["1", "2"], ["alt", "backspace"]], "Next", ["popup:!", "popup:?"]
    )
    assert state.row_rects == [[[7, 1722, 95, 140]], [[61, 1879, 95, 140]]]
    assert state.row_colors == [["#FDFDFE"], ["#F4F5F7"]]
    assert state.probe == (4, 2004, "#000000")
    assert state.touches == 7
    assert state.row_labels == [["1", "2"], ["ALT", "⌫"]]
    assert state.strip == ["punct:!", "punct:?"]
    assert (state.language, state.languages) == ("JAPANESE", ["TRADITIONAL", "JAPANESE"])
    assert state.code_hint == "忠 qa"
    assert (state.shift, state.preview, state.feedback) == ("locked", "g", {"vibrate": 2, "sound": 1})
    assert (state.dimmed, state.hidden) == (["b", "c"], ["homophone"])
    assert (state.palette, state.font_scale, state.row_height) == ({"bg": "#ECEFF1", "cells": False}, 1.2, 96)
    assert [c.text for c in state.candidates] == ["日", "月"]
    assert state.candidates[1].annotation == "ㄩㄝˋ"
    assert state.candidate_row == Rect(0, 2000, 1080, 150)
    assert state.keys["b"].center == (545, 2370)


def test_candidate_lookup_by_text_or_index():
    state = parse_dump(dump_with(STATE))
    assert state.candidate(text="月").index == 1
    assert state.candidate(index=0).text == "日"
    assert state.candidate(text="月").rect.center == (350, 2075)
    with pytest.raises(LookupError, match="星"):
        state.candidate(text="星")
    with pytest.raises(LookupError, match="index=5"):
        state.candidate(index=5)


def test_key_lookup_reports_missing_key():
    state = parse_dump(dump_with(STATE))
    with pytest.raises(LookupError, match="key:zz"):
        state.key("zz")
    assert state.key("b") == Rect(500, 2300, 90, 140)


def test_parse_dump_without_state_line_raises():
    with pytest.raises(ValueError, match="LIUKAI_STATE"):
        parse_dump("SERVICE something\n  nothing here\n")


def test_candidate_texts_helper():
    assert parse_dump(dump_with(STATE)).candidate_texts() == ["日", "月"]


def test_rect_contains_center_within_screen_width():
    assert Rect(1000, 0, 200, 10).center_visible(1080) is False
    assert Rect(10, 0, 20, 10).center_visible(1080) is True
    assert Rect(-40, 0, 20, 10).center_visible(1080) is False


def test_state_is_immutable():
    state = parse_dump(dump_with(STATE))
    assert isinstance(state, ImeState)
    with pytest.raises(AttributeError):
        state.mode = "ENGLISH"


def test_window_shown_is_parsed():
    assert parse_dump(dump_with({**STATE, "windowShown": False})).window_shown is False
