#!/usr/bin/env bash
# liu-kai 端對端測試（WSL2 + Windows 端 Android 模擬器）：以 TestPilot plugin 執行 YAML 案例，
# 匯出 app 覆蓋率並驗證 100% 門檻。
#
# 用法：
#   scripts/emulator-e2e.sh                  建置、安裝、執行全部案例、匯出覆蓋率並驗證
#   scripts/emulator-e2e.sh --case <id>...   只跑指定案例（仍匯出覆蓋率，但不驗證門檻）
#   scripts/emulator-e2e.sh --no-build       略過 Gradle 建置
#
# 環境變數：
#   WIN_SDK   Windows 端 Android SDK（預設 %USERPROFILE%\AppData\Local\Android\Sdk）
#   AVD_NAME  專用 AVD（預設 LiuKai35；不存在時以 API 35 x86_64 映像建立，含實體鍵盤）
#   EMU_PORT  模擬器 console port（預設 5580，serial 為 emulator-<port>）
#   LIU_KAI_DATA  真實字表目錄（預設 ~/prj_pri/liu-kai-data，real-* 案例使用）
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=1
CASES=()
while [ $# -gt 0 ]; do
  case "$1" in
    --no-build) BUILD=0; shift ;;
    --case) CASES+=(--case "${2:?--case 需要 id}"); shift 2 ;;
    -h|--help) sed -n '2,15p' "$0"; exit 0 ;;
    *) echo "未知參數：$1" >&2; exit 2 ;;
  esac
done

WIN_HOME=$(wslpath "$(cmd.exe /c 'echo %USERPROFILE%' 2>/dev/null | tr -d '\r')")
WIN_SDK=${WIN_SDK:-$WIN_HOME/AppData/Local/Android/Sdk}
AVD_NAME=${AVD_NAME:-LiuKai35}
EMU_PORT=${EMU_PORT:-5580}
SERIAL=emulator-$EMU_PORT
ADB_EXE="$WIN_SDK/platform-tools/adb.exe"
EMU_EXE="$WIN_SDK/emulator/emulator.exe"
AVD_DIR="$WIN_HOME/.android/avd"
VENV="$ROOT/testpilot/.venv"
COVERAGE_DIR="$ROOT/app/build/outputs/e2e-coverage"

adb() { "$ADB_EXE" -s "$SERIAL" "$@" | tr -d '\r'; }
log() { printf '[e2e] %s\n' "$*"; }

create_avd() {
  [ -f "$AVD_DIR/$AVD_NAME.ini" ] && return
  log "建立 AVD $AVD_NAME"
  mkdir -p "$AVD_DIR/$AVD_NAME.avd"
  printf 'avd.ini.encoding=UTF-8\r\npath=%s\r\npath.rel=avd\\%s.avd\r\ntarget=android-35\r\n' \
    "$(wslpath -w "$AVD_DIR/$AVD_NAME.avd")" "$AVD_NAME" > "$AVD_DIR/$AVD_NAME.ini"
  cat > "$AVD_DIR/$AVD_NAME.avd/config.ini" <<EOF
avd.ini.encoding=UTF-8
AvdId=$AVD_NAME
avd.ini.displayname=$AVD_NAME
abi.type=x86_64
hw.cpu.arch=x86_64
hw.cpu.ncore=4
image.sysdir.1=system-images\\android-35\\google_apis_playstore\\x86_64\\
tag.id=google_apis_playstore
tag.display=Google Play
PlayStore.enabled=true
hw.keyboard=yes
hw.keyboard.charmap=qwerty2
hw.lcd.density=420
hw.lcd.height=2400
hw.lcd.width=1080
hw.ramSize=4096M
disk.dataPartition.size=6144M
hw.gpu.enabled=yes
hw.gpu.mode=auto
hw.device.name=pixel_6
hw.device.manufacturer=Google
hw.sdCard=yes
sdcard.size=512 MB
showDeviceFrame=no
EOF
}

boot_emulator() {
  if ! "$ADB_EXE" devices | tr -d '\r' | grep -q "^$SERIAL[[:space:]]*device"; then
    # -no-snapshot：一律冷開機、不存快照，資料分割區（含 Play 登入與已安裝的 App）照常保留；
    # -memory 4096：登入 Google 後 Play 服務常駐，2 GB 會大量 swap，轉場變慢、點擊被丟掉
    log "啟動模擬器 $AVD_NAME（$SERIAL）"
    mkdir -p "$ROOT/e2e-out"
    "$EMU_EXE" -avd "$AVD_NAME" -port "$EMU_PORT" -memory 4096 -no-snapshot -no-audio -no-boot-anim >"$ROOT/e2e-out/emulator.log" 2>&1 &
    "$ADB_EXE" -s "$SERIAL" wait-for-device
  fi
  local waited=0
  until [ "$(adb shell getprop sys.boot_completed 2>/dev/null)" = "1" ]; do
    sleep 5
    waited=$((waited + 5))
    [ $waited -ge 600 ] && { log "等待開機逾時"; exit 1; }
  done
  adb shell input keyevent KEYCODE_WAKEUP >/dev/null || true
  adb shell wm dismiss-keyguard >/dev/null 2>&1 || true
  # 關閉系統動畫（UI 測試慣例）：模擬器的繪圖路徑在大量轉場動畫下曾卡住 WindowManager 68 秒，
  # 被 watchdog 重啟 system_server；關閉後轉場也不再造成點擊落空
  for scale in window_animation_scale transition_animation_scale animator_duration_scale; do
    adb shell settings put global "$scale" 0
  done
  log "模擬器 $SERIAL 已開機"
}

write_testbed() {
  cat > "$ROOT/testpilot/testbed.yaml" <<EOF
adb_binary: $ADB_EXE
path_mapper: wslpath
serial: $SERIAL
repo_root: $ROOT
reports_dir: $ROOT/testpilot/reports
real_table_dir: ${LIU_KAI_DATA:-$HOME/prj_pri/liu-kai-data}
screen_width: 1080
settle_ms: 400
EOF
}

install_apps() {
  local stage="$WIN_HOME/AppData/Local/Temp/liu-kai-e2e"
  mkdir -p "$stage"
  for apk in app/build/outputs/apk/debug/app-debug.apk testhost/build/outputs/apk/debug/testhost-debug.apk; do
    cp "$ROOT/$apk" "$stage/"
    log "安裝 $(basename "$apk")：$(adb install -r -t "$(wslpath -w "$stage/$(basename "$apk")")" | tail -1)"
  done
  # 重新安裝會讓系統改回其他輸入法，重新啟用並切換
  adb shell ime enable com.hamanpaul.liukai/.ime.LiuKaiImeService >/dev/null
  adb shell ime set com.hamanpaul.liukai/.ime.LiuKaiImeService >/dev/null
}

if [ $BUILD -eq 1 ]; then
  log "Gradle 建置（debug 版含 JaCoCo instrumentation）"
  (cd "$ROOT" && ./gradlew --no-daemon -q :app:assembleDebug :testhost:assembleDebug)
fi
if [ ! -x "$VENV/bin/testpilot" ]; then
  log "建立 TestPilot 環境 $VENV"
  uv venv -q "$VENV" --python 3.12
  VIRTUAL_ENV="$VENV" uv pip install -q -e "$ROOT/testpilot[test]"
fi
create_avd
boot_emulator
write_testbed
install_apps

log "執行 TestPilot 案例"
mkdir -p "$ROOT/e2e-out"
touch "$ROOT/e2e-out/.run-start"
set +e
(cd "$ROOT" && LIU_KAI_TESTBED="$ROOT/testpilot/testbed.yaml" "$VENV/bin/testpilot" run liu_kai "${CASES[@]}")
status=$?
set -e
# testpilot 的結束碼不反映案例判定：以本次產生的 report.json 為準，沒有產生報告也算失敗
latest=$(find "$ROOT/testpilot/reports" -mindepth 1 -maxdepth 1 -type d -newer "$ROOT/e2e-out/.run-start" 2>/dev/null | sort | tail -1)
if [ -z "$latest" ]; then
  log "本次執行沒有產生報告"
  status=1
elif [ "$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["summary"]["overall"])' "$latest/report.json")" != PASS ]; then
  status=1
fi

rm -rf "$COVERAGE_DIR"
(cd "$ROOT" && LIU_KAI_TESTBED="$ROOT/testpilot/testbed.yaml" "$VENV/bin/liu-kai-coverage" --out "$COVERAGE_DIR/e2e.ec")
(cd "$ROOT" && ./gradlew --no-daemon -q :app:jacocoE2eReport)
python3 "$ROOT/scripts/coverage-gaps.py" "$ROOT/app/build/reports/jacoco/jacocoE2eReport/jacocoE2eReport.xml" || true
[ -n "$latest" ] && log "報告：$latest/report.md"

[ $status -eq 0 ] || { log "案例未全數通過"; exit $status; }
if [ ${#CASES[@]} -eq 0 ]; then
  (cd "$ROOT" && ./gradlew --no-daemon -q :app:jacocoE2eCoverageVerification)
  log "app 覆蓋率 100% 門檻通過"
fi
