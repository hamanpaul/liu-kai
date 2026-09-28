import pytest

from liu_kai_testpilot.ime_state import Rect
from liu_kai_testpilot.ui import find_node, is_focused, node_text, screen_rotation

XML = """<?xml version='1.0' encoding='UTF-8' standalone='yes' ?>
<hierarchy rotation="0">
  <node index="0" text="" resource-id="" class="android.widget.FrameLayout" content-desc="" bounds="[0,0][1080,2400]">
    <node index="0" text="日月" resource-id="" class="android.widget.EditText" content-desc="plain" focused="true" bounds="[0,120][1080,240]" />
    <node index="1" text="" resource-id="" class="android.widget.EditText" content-desc="password" bounds="[0,240][1080,360]" />
    <node index="2" text="清除字表" resource-id="" class="android.widget.Button" content-desc="" bounds="[40,600][1040,720]" />
    <node index="3" text="輸入法：已啟用／使用中&#10;字表：已匯入" resource-id="" class="android.widget.TextView" content-desc="" bounds="[40,60][1040,120]" />
  </node>
</hierarchy>"""


def test_node_text_by_content_desc():
    assert node_text(XML, desc="plain") == "日月"
    assert node_text(XML, desc="password") == ""


def test_find_node_by_exact_or_partial_text():
    assert find_node(XML, text="清除字表").rect == Rect(40, 600, 1000, 120)
    assert "使用中" in find_node(XML, text_contains="輸入法：").text


def test_missing_node_raises_lookup_error():
    with pytest.raises(LookupError, match="desc=nope"):
        node_text(XML, desc="nope")
    with pytest.raises(LookupError, match="text=不存在"):
        find_node(XML, text="不存在")


def test_focus_and_screen_rotation():
    assert is_focused(XML, "plain") is True
    assert is_focused(XML, "password") is False
    assert is_focused(XML, "nope") is False
    assert screen_rotation(XML) == 0
    assert screen_rotation(XML.replace('rotation="0"', 'rotation="1"')) == 1
