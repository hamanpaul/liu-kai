"""liu-kai 的 TestPilot plugin：以 adb 驅動模擬器上的 liu-kai 輸入法與 testhost，執行 YAML 案例。

測試環境由 testbed.yaml 描述（預設 `testpilot/testbed.yaml`，可用環境變數 LIU_KAI_TESTBED 指定），
範例見 `liu_kai_testpilot/testbed.yaml.example`。
"""
from __future__ import annotations

import os
import subprocess
import time
from functools import cached_property
from pathlib import Path
from typing import Any, Callable

import yaml
from testpilot.api import PluginBase, load_cases_dir

from liu_kai_testpilot.adb import Adb
from liu_kai_testpilot.criteria import evaluate as evaluate_criteria
from liu_kai_testpilot.steps import DeviceConfig, StepExecutor

DEFAULT_TESTBED = Path(__file__).resolve().parent.parent / "testbed.yaml"
_CONFIG_KEYS = set(DeviceConfig.__dataclass_fields__)
# 框架的「實體鍵盤時顯示螢幕鍵盤」設定監聽約延遲 6 秒；每 0.5 秒檢查一次，最多等 15 秒
KEYBOARD_POLLS = 30


def load_testbed() -> dict[str, Any]:
    path = Path(os.environ.get("LIU_KAI_TESTBED", DEFAULT_TESTBED))
    if not path.exists():
        raise FileNotFoundError(
            f"找不到測試環境設定 {path}（testpilot/testbed.yaml）；請參考 liu_kai_testpilot/testbed.yaml.example 建立"
        )
    return yaml.safe_load(path.read_text(encoding="utf-8"))


def _wslpath(path: str) -> str:
    return subprocess.check_output(["wslpath", "-w", path]).decode().strip()


class Plugin(PluginBase):
    api_version = "1.2"

    def __init__(
        self,
        testbed: dict[str, Any] | None = None,
        adb: Any = None,
        sleep: Callable[[float], None] | None = None,
    ) -> None:
        self._testbed = testbed
        self._adb = adb
        self._sleep = sleep or time.sleep
        self._applied_table: str | None = None
        self.evidence: dict[str, dict[str, Any]] = {}

    @property
    def name(self) -> str:
        return "liu_kai"

    @property
    def version(self) -> str:
        return "0.1.0"

    def report_formats(self) -> list[str]:
        return ["json", "md"]

    @cached_property
    def testbed(self) -> dict[str, Any]:
        return self._testbed if self._testbed is not None else load_testbed()

    @cached_property
    def config(self) -> DeviceConfig:
        return DeviceConfig(**{k: v for k, v in self.testbed.items() if k in _CONFIG_KEYS})

    @cached_property
    def adb(self) -> Any:
        if self._adb is not None:
            return self._adb
        mapper = _wslpath if self.testbed["path_mapper"] == "wslpath" else str
        return Adb(self.testbed["adb_binary"], self.testbed["serial"], path_mapper=mapper)

    @cached_property
    def executor(self) -> StepExecutor:
        return StepExecutor(self.adb, self.config, sleep=self._sleep)

    def discover_cases(self) -> list[dict[str, Any]]:
        return load_cases_dir(self.cases_dir)

    def create_runner(self) -> Any:
        from liu_kai_testpilot.runner import LiuKaiRunner

        return LiuKaiRunner(self)

    # ---- pipeline ----

    def _evidence(self, case: dict[str, Any]) -> dict[str, Any]:
        return self.evidence.setdefault(case["id"], {"setup": [], "steps": [], "criteria": []})

    def _setup_step(self, case: dict[str, Any], step: dict[str, Any]) -> bool:
        result = self.executor.execute(step)
        self._evidence(case)["setup"].append({"action": step["action"], **result})
        return result["success"]

    def setup_env(self, case: dict[str, Any], topology: Any) -> bool:
        pre = case.get("preconditions", {})
        steps: list[dict[str, Any]] = []
        table = pre.get("table", "demo")
        if table != self._applied_table:
            steps.append({"action": "import_table", "source": table, "files": pre.get("files", [])})
        soft = "1" if pre.get("keyboard", "hard") == "soft" else "0"
        steps.append({"action": "setting", "namespace": "secure", "key": "show_ime_with_hard_keyboard", "value": soft})
        launch = pre.get("launch", "host")
        # focus=false：只啟動 testhost、不點輸入欄（輸入法畫面不會被要求顯示）
        focus = launch == "host" and pre.get("focus", True)
        if launch == "host":
            steps.append({"action": "launch_host"})
        if focus:
            steps.append({"action": "tap_field", "field": pre.get("field", "plain")})
        elif launch == "settings":
            steps.append({"action": "launch_settings"})
        for step in steps:
            if not self._setup_step(case, step):
                return False
        self._applied_table = table
        if focus:
            mode = self._await_ime_window(case, pre.get("field", "plain"), soft == "1")
            if mode is None:
                return False
            if mode == "ENGLISH":
                self._setup_step(case, {"action": "keys", "keys": ["SHIFT_LEFT"]})
        return True

    def _await_ime_window(self, case: dict[str, Any], field: str, soft: bool) -> str | None:
        """點輸入欄後確認輸入法視窗真的顯示（剛啟動的 App 有 splash 過場，第一下觸控可能被吃掉，
        最多點 3 次），且鍵盤區已跟上「實體鍵盤時顯示螢幕鍵盤」設定（框架的設定監聽會延遲數秒，
        最多等 KEYBOARD_POLLS 次）；回傳目前模式，失敗回傳 None。"""
        taps = 1
        polls = 0
        while True:
            captured = self.executor.execute({"action": "ime_state"})["captured"]
            if not captured.get("window_shown"):
                if taps == 3:
                    return self._setup_failed(case, "輸入法視窗未顯示（已點輸入欄 3 次）")
                self._setup_step(case, {"action": "tap_field", "field": field})
                taps += 1
            elif captured["keyboard_visible"] != soft:
                if polls == KEYBOARD_POLLS:
                    return self._setup_failed(case, f"鍵盤區未跟上設定（預期 keyboard_visible={soft}）")
                polls += 1
                self._sleep(0.5)
            else:
                return captured["mode"]

    def _setup_failed(self, case: dict[str, Any], reason: str) -> None:
        self._evidence(case)["setup"].append({"action": "ime_state", "success": False, "output": reason, "captured": {}})
        return None

    def verify_env(self, case: dict[str, Any], topology: Any) -> bool:
        pre = case.get("preconditions", {})
        if pre.get("launch", "host") != "host" or not pre.get("focus", True):
            return True
        return self._setup_step(case, {"action": "ime_state"})

    def execute_step(self, case: dict[str, Any], step: dict[str, Any], topology: Any) -> dict[str, Any]:
        result = self.executor.execute(step)
        self._evidence(case)["steps"].append({"id": step["id"], "action": step["action"], **result})
        return result

    def evaluate(self, case: dict[str, Any], results: dict[str, Any]) -> bool:
        ok, details = evaluate_criteria(case["pass_criteria"], results)
        self._evidence(case)["criteria"] = details
        return ok

    def teardown(self, case: dict[str, Any], topology: Any) -> None:
        for step in (
            {"action": "keys", "keys": ["ESCAPE"]},
            {"action": "shell", "command": "settings put system user_rotation 0"},
        ):
            self.executor.execute(step)
        if case.get("mutates_table"):
            self._applied_table = None
