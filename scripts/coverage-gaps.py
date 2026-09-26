#!/usr/bin/env python3
"""列出 JaCoCo XML 報表中未覆蓋的行與分支。

用法：scripts/coverage-gaps.py <jacoco.xml>...
每個來源檔印出：未覆蓋行（mi>0）與部分覆蓋分支（mb>0）的行號，最後印總計。
全部 100% 時結束碼為 0，否則為 1。
"""
import sys
import xml.etree.ElementTree as ET


def gaps(path: str) -> tuple[int, int, int, int]:
    root = ET.parse(path).getroot()
    total = {"LINE": [0, 0], "BRANCH": [0, 0]}
    for counter in root.findall("counter"):
        kind = counter.get("type")
        if kind in total:
            total[kind] = [int(counter.get("missed")), int(counter.get("covered"))]
    for package in root.findall("package"):
        for source in package.findall("sourcefile"):
            missed_lines = [ln.get("nr") for ln in source.findall("line") if int(ln.get("mi")) > 0]
            missed_branches = [
                f"{ln.get('nr')}({ln.get('mb')}/{int(ln.get('mb')) + int(ln.get('cb'))})"
                for ln in source.findall("line")
                if int(ln.get("mb")) > 0
            ]
            if missed_lines or missed_branches:
                name = f"{package.get('name')}/{source.get('name')}"
                print(f"{name}\n  lines: {' '.join(missed_lines) or '-'}\n  branches: {' '.join(missed_branches) or '-'}")
    (lm, lc), (bm, bc) = total["LINE"], total["BRANCH"]
    return lm, lc, bm, bc


def main() -> int:
    ok = True
    for path in sys.argv[1:]:
        lm, lc, bm, bc = gaps(path)
        line_pct = 100.0 * lc / max(lm + lc, 1)
        branch_pct = 100.0 * bc / max(bm + bc, 1)
        print(f"== {path}: LINE {line_pct:.1f}% ({lm} missed)  BRANCH {branch_pct:.1f}% ({bm} missed)")
        ok = ok and lm == 0 and bm == 0
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
