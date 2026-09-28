"""從模擬器上的 liu-kai（debug 版，JaCoCo offline instrumentation）匯出覆蓋率資料。

用法：`python -m liu_kai_testpilot.coverage --out app/build/outputs/e2e-coverage/e2e.ec`
（案例全部跑完後執行；broadcast 兩次，第二次才包含匯出程式本身的執行紀錄）。
"""
from __future__ import annotations

import argparse
from pathlib import Path
from typing import Any

from liu_kai_testpilot.plugin import Plugin


def dump_coverage(adb: Any, package: str, out_path: str) -> int:
    for _ in range(2):
        adb.broadcast(f"{package}.DUMP_COVERAGE", f"{package}/.debug.CoverageDumpReceiver", {})
    data = adb.run_as_read(package, "files/coverage.ec")
    out = Path(out_path)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(data)
    return len(data)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", required=True)
    args = parser.parse_args(argv)
    plugin = Plugin()
    size = dump_coverage(plugin.adb, plugin.config.app_package, args.out)
    print(f"coverage: {size} bytes -> {args.out}")
    return 0
