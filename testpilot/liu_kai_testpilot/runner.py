"""跑完整批案例並產出 test report（JSON＋Markdown），報告附每個 step 的證據與通過條件判定。"""
from __future__ import annotations

import json
from datetime import datetime
from pathlib import Path
from typing import Any, Callable


class LiuKaiRunner:
    def __init__(self, plugin: Any, clock: Callable[[], datetime] = datetime.now) -> None:
        self.plugin = plugin
        self.clock = clock

    def run(
        self,
        orchestrator: Any,
        plugin_name: str,
        case_ids: list[str] | None,
        dut_fw_ver: str | None,
        provider_config: dict[str, Any] | None,
    ) -> dict[str, Any]:
        started = self.clock()
        cases = self.plugin.prepare_run(case_ids).cases
        rows = []
        for case in cases:
            outcome = self.plugin.run_pipeline(case, case["topology"])
            evidence = self.plugin.evidence.get(case["id"], {})
            rows.append(
                {
                    "case_id": case["id"],
                    "name": case["name"],
                    "verdict": bool(outcome["verdict"]),
                    "comment": outcome["comment"],
                    "setup": evidence.get("setup", []),
                    "steps": evidence.get("steps", []),
                    "criteria": evidence.get("criteria", []),
                }
            )
        passed = sum(r["verdict"] for r in rows)
        overall = "PASS" if rows and passed == len(rows) else "FAIL"
        report = self._write(started, rows, passed, overall)
        return {
            "plugin": plugin_name,
            "overall": overall,
            "results": [{"case_id": r["case_id"], "verdict": r["verdict"], "comment": r["comment"]} for r in rows],
            "report": str(report),
        }

    def _write(self, started: datetime, rows: list[dict[str, Any]], passed: int, overall: str) -> Path:
        folder = Path(self.plugin.testbed.get("reports_dir", "testpilot/reports")) / started.strftime("%Y%m%d-%H%M%S")
        folder.mkdir(parents=True, exist_ok=True)
        summary = {"total": len(rows), "passed": passed, "failed": len(rows) - passed, "overall": overall}
        (folder / "report.json").write_text(
            json.dumps({"started": started.isoformat(), "summary": summary, "cases": rows}, ensure_ascii=False, indent=2),
            encoding="utf-8",
        )
        lines = [
            "# liu-kai TestPilot 報告",
            "",
            f"- 開始：{started.isoformat()}",
            f"- 結果：{overall}（{passed}/{len(rows)} 通過）",
            "",
            "| case | 名稱 | 判定 | 說明 |",
            "|---|---|---|---|",
        ]
        lines += [f"| {r['case_id']} | {r['name']} | {'PASS' if r['verdict'] else 'FAIL'} | {r['comment']} |" for r in rows]
        for r in rows:
            lines += ["", f"## {r['case_id']}：{r['name']}", ""]
            lines += [f"- step `{s['id']}`（{s['action']}）：{'OK' if s['success'] else 'FAIL'} — {s['output']}" for s in r["steps"]]
            lines += [f"- {d}" for d in r["criteria"]]
        path = folder / "report.md"
        path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        return path
