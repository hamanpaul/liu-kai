"""pass_criteria 判定：field 為 `<step id>.<captured 路徑>`，逐條產生可寫入報告的說明。"""
from __future__ import annotations

from typing import Any, Callable

_OPERATORS: dict[str, Callable[[Any, Any], bool]] = {
    "equals": lambda actual, value: actual == value,
    "not_equals": lambda actual, value: actual != value,
    "contains": lambda actual, value: value in actual,
    "not_contains": lambda actual, value: value not in actual,
    "startswith": lambda actual, value: str(actual).startswith(value),
    "length": lambda actual, value: len(actual) == value,
    "in": lambda actual, value: actual in value,
}


def resolve(results: dict[str, Any], field: str) -> Any:
    step_id, _, path = field.partition(".")
    steps = results["steps"]
    if step_id not in steps:
        raise KeyError(f"找不到 step {step_id}")
    node: Any = steps[step_id]["captured"]
    for part in path.split("."):
        if part not in node:
            raise KeyError(f"找不到欄位 {field}")
        node = node[part]
    return node


def evaluate(criteria: list[dict[str, Any]], results: dict[str, Any]) -> tuple[bool, list[str]]:
    if not criteria:
        return False, ["FAIL 沒有 pass_criteria"]
    details = []
    for c in criteria:
        field, operator, value = c["field"], c["operator"], c["value"]
        head = f"{field} {operator} {value!r}"
        try:
            actual = resolve(results, field)
        except KeyError as e:
            details.append(f"FAIL {head}（{e.args[0]}）")
            continue
        if operator not in _OPERATORS:
            details.append(f"FAIL {head}（未知的 operator {operator}）")
            continue
        verdict = "PASS" if _OPERATORS[operator](actual, value) else "FAIL"
        details.append(f"{verdict} {head}（實際 {actual!r}）")
    return all(d.startswith("PASS") for d in details), details
