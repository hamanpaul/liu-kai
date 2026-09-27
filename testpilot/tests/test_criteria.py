import pytest

from liu_kai_testpilot.criteria import evaluate, resolve

RESULTS = {
    "steps": {
        "read": {"success": True, "output": "日", "captured": {"text": "日月"}},
        "state": {"success": True, "output": "", "captured": {"candidates": ["日", "月"], "mode": "CHINESE"}},
    }
}


def test_resolve_step_captured_path():
    assert resolve(RESULTS, "read.text") == "日月"
    assert resolve(RESULTS, "state.candidates") == ["日", "月"]


def test_resolve_reports_missing_step_or_field():
    with pytest.raises(KeyError, match="nostep"):
        resolve(RESULTS, "nostep.text")
    with pytest.raises(KeyError, match="read.nofield"):
        resolve(RESULTS, "read.nofield")


@pytest.mark.parametrize(
    "criterion, expected",
    [
        ({"field": "read.text", "operator": "equals", "value": "日月"}, True),
        ({"field": "read.text", "operator": "equals", "value": "日"}, False),
        ({"field": "read.text", "operator": "not_equals", "value": "日"}, True),
        ({"field": "read.text", "operator": "contains", "value": "月"}, True),
        ({"field": "read.text", "operator": "not_contains", "value": "月"}, False),
        ({"field": "read.text", "operator": "startswith", "value": "日"}, True),
        ({"field": "state.candidates", "operator": "length", "value": 2}, True),
        ({"field": "state.mode", "operator": "in", "value": ["CHINESE", "JAPANESE"]}, True),
        ({"field": "state.candidates", "operator": "contains", "value": "星"}, False),
    ],
)
def test_operators(criterion, expected):
    ok, details = evaluate([criterion], RESULTS)
    assert ok is expected
    assert details[0].startswith("PASS" if expected else "FAIL")


def test_evaluate_requires_all_and_explains_each():
    ok, details = evaluate(
        [
            {"field": "read.text", "operator": "equals", "value": "日月"},
            {"field": "state.mode", "operator": "equals", "value": "ENGLISH"},
        ],
        RESULTS,
    )
    assert ok is False
    assert details == [
        "PASS read.text equals '日月'（實際 '日月'）",
        "FAIL state.mode equals 'ENGLISH'（實際 'CHINESE'）",
    ]


def test_missing_field_and_unknown_operator_fail_with_reason():
    ok, details = evaluate(
        [{"field": "x.y", "operator": "equals", "value": 1}, {"field": "read.text", "operator": "matches", "value": "."}],
        RESULTS,
    )
    assert ok is False
    assert "找不到" in details[0]
    assert "未知的 operator matches" in details[1]


def test_empty_criteria_is_not_a_pass():
    assert evaluate([], RESULTS) == (False, ["FAIL 沒有 pass_criteria"])


def test_equals_field_compares_with_another_captured_field():
    results = {"steps": {
        "a": {"captured": {"y": 1500}}, "b": {"captured": {"y": 1500}}, "c": {"captured": {"y": 200}},
    }}
    ok, details = evaluate([{"field": "b.y", "operator": "equals_field", "value": "a.y"}], results)
    assert ok is True
    assert details == ["PASS b.y equals_field 'a.y'（實際 1500，a.y 為 1500）"]
    ok, details = evaluate([{"field": "c.y", "operator": "equals_field", "value": "a.y"}], results)
    assert ok is False
    ok, details = evaluate([{"field": "c.y", "operator": "equals_field", "value": "zz.y"}], results)
    assert ok is False
    assert "找不到 step zz" in details[0]
