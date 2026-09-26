"""解析 `uiautomator dump` 的 XML：依 content-desc 或文字找節點、焦點與畫面方向。"""
from __future__ import annotations

import re
import xml.etree.ElementTree as ET
from dataclasses import dataclass

from liu_kai_testpilot.ime_state import Rect

_BOUNDS = re.compile(r"\[(-?\d+),(-?\d+)\]\[(-?\d+),(-?\d+)\]")


@dataclass(frozen=True)
class UiNode:
    text: str
    desc: str
    rect: Rect
    focused: bool


def _nodes(xml: str) -> list[UiNode]:
    out = []
    for n in ET.fromstring(xml.encode()).iter("node"):
        x1, y1, x2, y2 = map(int, _BOUNDS.match(n.get("bounds")).groups())
        out.append(
            UiNode(n.get("text"), n.get("content-desc"), Rect(x1, y1, x2 - x1, y2 - y1), n.get("focused") == "true")
        )
    return out


def find_node(
    xml: str, desc: str | None = None, text: str | None = None, text_contains: str | None = None
) -> UiNode:
    for node in _nodes(xml):
        if (
            (desc is None or node.desc == desc)
            and (text is None or node.text == text)
            and (text_contains is None or text_contains in node.text)
        ):
            return node
    raise LookupError(f"畫面上找不到節點 desc={desc} text={text} text_contains={text_contains}")


def node_text(xml: str, desc: str) -> str:
    return find_node(xml, desc=desc).text


def is_focused(xml: str, desc: str) -> bool:
    """content-desc 為 desc 的節點存在且取得焦點。"""
    return any(n.desc == desc and n.focused for n in _nodes(xml))


def screen_rotation(xml: str) -> int:
    """dump 當下的畫面方向（0–3）。"""
    return int(ET.fromstring(xml.encode()).get("rotation"))
