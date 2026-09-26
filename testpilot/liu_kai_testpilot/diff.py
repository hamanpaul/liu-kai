"""官方嘸蝦米與 liu-kai 的行為對照：同一批實體鍵序列分別在兩個輸入法上執行，逐項比對輸入欄最後的文字。

以黑箱方式驅動（不讀輸入法內部狀態）：每個案例前結束輸入法行程並重新選用，確保從初始狀態
（中文模式、沒有組字）開始；啟動 testhost、點一般欄，送出按鍵後記錄輸入法視窗是否實際顯示
（候選列有沒有出現），再讀取欄位文字（含組字中尚未上屏的文字）。liu-kai 先匯入真實字表，與官方用同一套字根比較。

案例的 expect_text／expect_visible 是依使用者答案推得的預期結果：報告逐項標示兩邊是否符合。
官方與使用者答案不同時視為官方偏差，不據以修改 liu-kai。

用法：liu-kai-diff --official <官方輸入法的 IME id> [--case <id> ...]
（IME id 以 `adb shell ime list -s` 查詢；報告寫到 testbed 的 reports_dir/diff-<時間>/）
"""
from __future__ import annotations

import argparse
import json
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Any, Callable

import yaml

from liu_kai_testpilot.plugin import Plugin
from liu_kai_testpilot.steps import DeviceConfig, StepExecutor

DIFF_CASES = Path(__file__).resolve().parent / "diff_cases.yaml"
# 重新選用輸入法後，等系統把 testhost 的輸入欄綁到該輸入法：每 0.5 秒檢查一次，最多 10 秒
BIND_POLLS = 20
# 綁定後輸入法還要處理 startInput，稍候再送鍵
READY_DELAY = 1.0


@dataclass(frozen=True)
class Target:
    name: str
    component: str

    @property
    def package(self) -> str:
        return self.component.split("/")[0]


def load_diff_cases(path: Path = DIFF_CASES) -> list[dict[str, Any]]:
    return yaml.safe_load(path.read_text(encoding="utf-8"))


def summarize_steps(steps: list[dict[str, Any]]) -> str:
    def one(step: dict[str, Any]) -> str:
        if step["action"] == "text":
            return f"text:{step['text']}"
        return ("+" if step["action"] == "combo" else " ").join(step["keys"])

    return " ／ ".join(one(s) for s in steps)


def _quote(text: str) -> str:
    return "（空）" if text == "" else "「" + text.replace(" ", "␠") + "」"


def _has_expectation(case: dict[str, Any]) -> bool:
    return "expect_text" in case or "expect_visible" in case


def judge(case: dict[str, Any], result: dict[str, Any]) -> str:
    """依使用者答案推得的預期判定一個輸入法的結果：✓、✗ 附原因、— 表示沒有預期。"""
    if result["error"] is not None:
        return "錯誤"
    if not _has_expectation(case):
        return "—"
    problems = []
    if "expect_text" in case and result["text"] != case["expect_text"]:
        problems.append(f"文字應為{_quote(case['expect_text'])}")
    if "expect_visible" in case and result["visible"] != case["expect_visible"]:
        problems.append("候選列應顯示" if case["expect_visible"] else "候選列應隱藏")
    return "✗ " + "、".join(problems) if problems else "✓"


def _expected(case: dict[str, Any]) -> str:
    parts = []
    if "expect_text" in case:
        parts.append(_quote(case["expect_text"]))
    if "expect_visible" in case:
        parts.append("顯示" if case["expect_visible"] else "隱藏")
    return "·".join(parts)


def _show(result: dict[str, Any], verdict: str) -> str:
    if result["error"] is not None:
        return f"（錯誤：{result['error']}）"
    return f"{_quote(result['text'])}·{'顯示' if result['visible'] else '未顯示'} {verdict}"


class DiffRunner:
    def __init__(
        self, adb: Any, config: DeviceConfig, out_dir: Path, sleep: Callable[[float], None] = time.sleep
    ) -> None:
        self.adb = adb
        self.config = config
        self.out_dir = Path(out_dir)
        self._sleep = sleep
        self.executor = StepExecutor(adb, config, sleep=sleep)

    def _run_step(self, step: dict[str, Any]) -> dict[str, Any]:
        result = self.executor.execute(step)
        if not result["success"]:
            raise RuntimeError(f"{step['action']}：{result['output']}")
        return result

    def _bound(self, target: Target) -> bool:
        dump = self.adb.shell("dumpsys input_method")
        host = self.config.host_component.split("/")[0]
        return f"mCurId={target.component} " in dump and "mBoundToMethod=true" in dump and f"packageName={host} " in dump

    def prepare(self, target: Target) -> None:
        """結束輸入法行程並重新選用（回到初始狀態），啟動 testhost 點一般欄，等輸入欄綁到該輸入法。"""
        self.adb.shell(f"am force-stop {target.package}")
        self.adb.shell(f"ime enable {target.component}")
        self.adb.shell(f"ime set {target.component}")
        self._run_step({"action": "launch_host"})
        self._run_step({"action": "tap_field", "field": "plain"})
        polls = 0
        while not self._bound(target):
            polls += 1
            if polls == BIND_POLLS:
                raise RuntimeError(f"{target.name} 未接上輸入欄")
            self._sleep(0.5)
        self._sleep(READY_DELAY)

    def run_case(self, case: dict[str, Any], target: Target) -> dict[str, Any]:
        try:
            self.prepare(target)
            for step in case["steps"]:
                self._run_step(step)
            shot = None
            if case.get("screenshot"):
                shot = f"{case['id']}-{target.name}.png"
                self.out_dir.mkdir(parents=True, exist_ok=True)
                (self.out_dir / shot).write_bytes(self.adb.screencap())
            visible = self.executor.ime_window_visible()
            text = self._run_step({"action": "read_field", "field": "plain"})["captured"]["text"]
        except Exception as e:  # noqa: BLE001 — 單一案例失敗不中斷整批對照
            return {"text": None, "visible": None, "error": str(e), "screenshot": None}
        return {"text": text, "visible": visible, "error": None, "screenshot": shot}

    def run(self, cases: list[dict[str, Any]], targets: list[Target]) -> list[dict[str, Any]]:
        rows = []
        for case in cases:
            results = {t.name: self.run_case(case, t) for t in targets}
            if any(r["error"] is not None for r in results.values()):
                verdict = "ERROR"
            elif len({(r["text"], r["visible"]) for r in results.values()}) == 1:
                verdict = "SAME"
            else:
                verdict = "DIFF"
            rows.append(
                {
                    "case_id": case["id"],
                    "name": case["name"],
                    "checklist": case.get("checklist"),
                    "expect": case.get("expect"),
                    "expected": _expected(case),
                    "has_expectation": _has_expectation(case),
                    "keys": summarize_steps(case["steps"]),
                    "results": results,
                    "judgement": {name: judge(case, r) for name, r in results.items()},
                    "verdict": verdict,
                }
            )
        return rows

    def write(self, rows: list[dict[str, Any]], targets: list[Target], started: datetime) -> Path:
        self.out_dir.mkdir(parents=True, exist_ok=True)
        summary = {
            "total": len(rows),
            "same": sum(r["verdict"] == "SAME" for r in rows),
            "diff": sum(r["verdict"] == "DIFF" for r in rows),
            "error": sum(r["verdict"] == "ERROR" for r in rows),
            "expected": sum(r["has_expectation"] for r in rows),
            "match": {t.name: sum(r["judgement"][t.name] == "✓" for r in rows) for t in targets},
        }
        (self.out_dir / "report.json").write_text(
            json.dumps(
                {
                    "started": started.isoformat(),
                    "targets": {t.name: t.component for t in targets},
                    "summary": summary,
                    "cases": rows,
                },
                ensure_ascii=False,
                indent=2,
            ),
            encoding="utf-8",
        )
        names = [t.name for t in targets]
        lines = [
            "# 官方嘸蝦米與 liu-kai 行為對照",
            "",
            f"- 開始：{started.isoformat(timespec='seconds')}",
            "- 對照：" + "；".join(f"{t.name} = `{t.component}`" for t in targets),
            f"- 結果：{summary_line(summary)}",
            f"- 符合你的答案（有預期的 {summary['expected']} 個案例）："
            + "、".join(f"{n} {summary['match'][n]}" for n in names),
            "- 做法：每個案例前重新啟動輸入法並開新的 testhost，以實體鍵送出按鍵後記錄輸入法視窗是否顯示（候選列有沒有出現），"
            "再讀取一般欄的文字（含組字中尚未上屏的文字）；␠ 表示空白。題號對應行為確認清單。",
            "- 判定：✓ 符合你的答案、✗ 不符（附原因）、— 沒有預期；官方不符時視為官方偏差，不據以修改 liu-kai。",
            "",
            "| case | 名稱 | 題號 | 按鍵 | 預期 | " + " | ".join(names) + " | 兩邊 |",
            "|---|---|---|---|---|" + "---|" * len(names) + "---|",
        ]
        for r in rows:
            cells = [_show(r["results"][n], r["judgement"][n]) for n in names]
            lines.append(
                f"| {r['case_id']} | {r['name']} | {r['checklist'] or ''} | {r['keys']} | {r['expected']} | "
                + " | ".join(cells)
                + f" | {r['verdict']} |"
            )
        lines += ["", "## 細節"]
        for r in rows:
            lines += ["", f"### {r['case_id']}：{r['name']}", "", f"- 按鍵：{r['keys']}"]
            if r["expect"]:
                lines.append(f"- 你的答案：{r['expect']}")
            if r["expected"]:
                lines.append(f"- 預期：{r['expected']}")
            for n in names:
                result = r["results"][n]
                shot = f" [截圖]({result['screenshot']})" if result["screenshot"] else ""
                lines.append(f"- {n}：{_show(result, r['judgement'][n])}{shot}")
        path = self.out_dir / "report.md"
        path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        return path


def summary_line(summary: dict[str, int]) -> str:
    return f"{summary['total']} 個案例；相同 {summary['same']}、不同 {summary['diff']}、錯誤 {summary['error']}"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="官方嘸蝦米與 liu-kai 的行為對照（實體鍵序列、比對輸入欄文字）")
    parser.add_argument("--official", required=True, help="官方輸入法的 IME id（adb shell ime list -s）")
    parser.add_argument("--case", action="append", default=[], help="只跑指定案例（可重複）")
    args = parser.parse_args(argv)
    cases = load_diff_cases()
    unknown = sorted(set(args.case) - {c["id"] for c in cases})
    if unknown:
        parser.error("未知的案例：" + ", ".join(unknown))
    if args.case:
        cases = [c for c in cases if c["id"] in args.case]

    plugin = Plugin()
    started = datetime.now()
    out_dir = Path(plugin.testbed.get("reports_dir", "testpilot/reports")) / f"diff-{started:%Y%m%d-%H%M%S}"
    runner = DiffRunner(plugin.adb, plugin.config, out_dir)
    imported = runner.executor.execute({"action": "import_table", "source": "real"})
    if not imported["captured"].get("result", "").startswith("OK"):
        print(f"liu-kai 匯入真實字表失敗：{imported['output']}")
        return 1
    plugin.adb.shell("settings put secure show_ime_with_hard_keyboard 0")
    targets = [Target("official", args.official), Target("liu-kai", plugin.config.ime_component)]
    rows = runner.run(cases, targets)
    path = runner.write(rows, targets, started)
    summary = json.loads((out_dir / "report.json").read_text(encoding="utf-8"))["summary"]
    print(f"{summary_line(summary)}\n報告：{path}")
    return 1 if summary["error"] else 0
