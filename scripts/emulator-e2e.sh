#!/usr/bin/env bash
# liu-kai 模擬器端對端測試（WSL2 + Windows 端 Android 模擬器）。
#
# 用法：
#   scripts/emulator-e2e.sh                 建置、安裝、匯入 demo 合成表並執行 ImeE2eTest
#   scripts/emulator-e2e.sh --real <dir>    建置、安裝，匯入 <dir> 內的正版字表（不跑合成表測試），
#                                           印出匯入統計後保留模擬器供實機驗收
#   scripts/emulator-e2e.sh --no-build ...  略過 Gradle 建置
#
# 環境變數：
#   WIN_SDK   Windows 端 Android SDK（預設 %USERPROFILE%\AppData\Local\Android\Sdk）
#   AVD_NAME  專用 AVD 名稱（預設 LiuKai35，不存在時以 API 35 x86_64 映像建立）
#   EMU_PORT  模擬器 console port（預設 5580，serial 為 emulator-<port>）
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
MODE=demo
REAL_DIR=""
BUILD=1
while [ $# -gt 0 ]; do
  case "$1" in
    --real) MODE=real; REAL_DIR=${2:?--real 需要目錄}; shift 2 ;;
    --no-build) BUILD=0; shift ;;
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
OUT="$ROOT/e2e-out"
mkdir -p "$OUT"

adb() { "$ADB_EXE" -s "$SERIAL" "$@" | tr -d '\r'; }
log() { printf '[e2e] %s\n' "$*"; }

create_avd() {
  [ -f "$AVD_DIR/$AVD_NAME.ini" ] && return
  log "建立 AVD $AVD_NAME"
  mkdir -p "$AVD_DIR/$AVD_NAME.avd"
  local win_avd
  win_avd=$(wslpath -w "$AVD_DIR/$AVD_NAME.avd")
  printf 'avd.ini.encoding=UTF-8\r\npath=%s\r\npath.rel=avd\\%s.avd\r\ntarget=android-35\r\n' \
    "$win_avd" "$AVD_NAME" > "$AVD_DIR/$AVD_NAME.ini"
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
hw.ramSize=2048M
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
  if "$ADB_EXE" devices | tr -d '\r' | grep -q "^$SERIAL[[:space:]]*device"; then
    log "模擬器 $SERIAL 已在執行"
  else
    log "啟動模擬器 $AVD_NAME（$SERIAL）"
    "$EMU_EXE" -avd "$AVD_NAME" -port "$EMU_PORT" -no-snapshot-save -no-audio -no-boot-anim >"$OUT/emulator.log" 2>&1 &
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
  log "模擬器已開機"
}

install_apks() {
  local stage="$WIN_HOME/AppData/Local/Temp/liu-kai-e2e"
  mkdir -p "$stage"
  cp "$ROOT/app/build/outputs/apk/debug/app-debug.apk" \
     "$ROOT/testhost/build/outputs/apk/debug/testhost-debug.apk" \
     "$ROOT/testhost/build/outputs/apk/androidTest/debug/testhost-debug-androidTest.apk" "$stage/"
  for apk in app-debug.apk testhost-debug.apk testhost-debug-androidTest.apk; do
    log "安裝 $apk"
    adb install -r -t "$(wslpath -w "$stage/$apk")" | tail -1
  done
  local ime=com.hamanpaul.liukai/.ime.LiuKaiImeService
  adb shell ime enable "$ime" >/dev/null
  adb shell ime set "$ime" >/dev/null
  adb shell settings put secure show_ime_with_hard_keyboard 0
  log "目前輸入法：$(adb shell settings get secure default_input_method)"
}

import_table() {
  local source=$1
  local out
  out=$(adb shell am broadcast -a com.hamanpaul.liukai.DEBUG_IMPORT \
    -n com.hamanpaul.liukai/.debug.DebugImportReceiver --es source "$source")
  echo "$out" > "$OUT/import.txt"
  log "匯入結果：$(grep -o 'data="[^"]*"' <<<"$out" || echo "$out")"
  grep -q 'data="OK' <<<"$out"
}

if [ $BUILD -eq 1 ]; then
  log "Gradle 建置"
  (cd "$ROOT" && ./gradlew --no-daemon -q :app:assembleDebug :testhost:assembleDebug :testhost:assembleDebugAndroidTest)
fi
create_avd
boot_emulator
install_apks

if [ "$MODE" = real ]; then
  remote=/sdcard/Android/data/com.hamanpaul.liukai/files/import
  adb shell rm -rf "$remote" >/dev/null
  adb shell mkdir -p "$remote"
  for f in "$REAL_DIR"/*; do
    [ -f "$f" ] || continue
    local_copy="$WIN_HOME/AppData/Local/Temp/liu-kai-e2e/$(basename "$f")"
    cp "$f" "$local_copy"
    adb push "$(wslpath -w "$local_copy")" "$remote/" | tail -1
    rm -f "$local_copy"
  done
  import_table files
  adb shell rm -rf "$remote" >/dev/null
  log "正版字表已匯入，模擬器保留供驗收"
  exit 0
fi

import_table demo
log "執行 ImeE2eTest"
adb shell am instrument -w -r com.hamanpaul.liukai.testhost.test/androidx.test.runner.AndroidJUnitRunner > "$OUT/instrument.txt" || true
summary=$(grep -E '^(OK \(|FAILURES!!!|Tests run:)' "$OUT/instrument.txt" || true)
log "結果：${summary:-（無輸出，見 e2e-out/instrument.txt）}"
grep -q '^OK (' "$OUT/instrument.txt"
