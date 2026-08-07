#!/usr/bin/env bash
#
# emu.sh — RIME 專案的 Android 模擬器控制腳本(無頭 / CI 友善)
#
# 用法:
#   emu.sh start [--cold]        啟動模擬器並等待開機完成
#   emu.sh stop                  乾淨關閉模擬器
#   emu.sh status                顯示模擬器狀態
#   emu.sh install <apk> [...]   安裝(覆蓋安裝)APK
#   emu.sh shot <out.png>        截圖
#   emu.sh ime-enable <ime_id>   啟用並設為預設輸入法
#   emu.sh ime-list              列出所有可用輸入法
#   emu.sh logcat [tag] [-- ...] 抓日誌(不給 tag 就抓全部)
#   emu.sh shell [...]           轉發 adb shell
#   emu.sh adb [...]             轉發 adb(已帶 -s <serial>)
#
# 可用環境變數:
#   ANDROID_SDK_ROOT  Android SDK 路徑(預設 $HOME/Android/Sdk)
#   RIME_AVD          AVD 名稱(預設 rime_test)
#   RIME_EMU_PORT     模擬器 console port(預設 5554,serial 為 emulator-<port>)
#   RIME_EMU_LOG      模擬器 stdout/stderr 日誌路徑
#   RIME_BOOT_TIMEOUT 等待開機的秒數上限(預設 600)
#   RIME_EMU_GPU      GPU 模式(預設 swiftshader_indirect)

set -euo pipefail

# ---------------------------------------------------------------- 環境設定 ---

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Android/Sdk}}"
export ANDROID_SDK_ROOT
export ANDROID_HOME="$ANDROID_SDK_ROOT"

AVD_NAME="${RIME_AVD:-rime_test}"
EMU_PORT="${RIME_EMU_PORT:-5554}"
SERIAL="emulator-${EMU_PORT}"
BOOT_TIMEOUT="${RIME_BOOT_TIMEOUT:-600}"
EMU_GPU="${RIME_EMU_GPU:-swiftshader_indirect}"

RUN_DIR="${RIME_RUN_DIR:-$PROJECT_ROOT/.emulator}"
EMU_LOG="${RIME_EMU_LOG:-$RUN_DIR/emulator-${EMU_PORT}.log}"
PID_FILE="$RUN_DIR/emulator-${EMU_PORT}.pid"

ADB="$ANDROID_SDK_ROOT/platform-tools/adb"
EMULATOR="$ANDROID_SDK_ROOT/emulator/emulator"
AVDMANAGER="$ANDROID_SDK_ROOT/cmdline-tools/latest/bin/avdmanager"

die()  { echo "[emu.sh] 錯誤: $*" >&2; exit 1; }
info() { echo "[emu.sh] $*" >&2; }

check_tools() {
  [ -x "$ADB" ]      || die "找不到 adb:$ADB(請確認 ANDROID_SDK_ROOT 設定正確、已安裝 platform-tools)"
  [ -x "$EMULATOR" ] || die "找不到 emulator:$EMULATOR(請執行 sdkmanager emulator)"
}

check_avd() {
  [ -d "$HOME/.android/avd/${AVD_NAME}.avd" ] || die "AVD '${AVD_NAME}' 不存在。請先建立:
  $AVDMANAGER create avd -n ${AVD_NAME} -k 'system-images;android-35;google_apis;x86_64' -d pixel_6"
}

# 模擬器是否已在 adb 裝置清單中(不論狀態)
emu_present() {
  "$ADB" devices 2>/dev/null | awk -v s="$SERIAL" '$1==s {found=1} END{exit !found}'
}

# 模擬器是否已 device 狀態(可下指令)
emu_online() {
  "$ADB" devices 2>/dev/null | awk -v s="$SERIAL" '$1==s && $2=="device" {found=1} END{exit !found}'
}

boot_completed() {
  local v
  v="$("$ADB" -s "$SERIAL" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r\n[:space:]')"
  [ "$v" = "1" ]
}

require_running() {
  emu_online || die "模擬器 ${SERIAL} 尚未啟動或尚未就緒。請先執行:$0 start"
}

# ------------------------------------------------------------------ 指令 ---

cmd_start() {
  local cold_boot="-no-snapshot"
  for a in "$@"; do
    case "$a" in
      --cold) cold_boot="-no-snapshot-load" ;;
    esac
  done

  check_tools
  check_avd

  if emu_online && boot_completed; then
    info "模擬器 ${SERIAL} 已在執行且開機完成,略過啟動。"
    return 0
  fi
  if emu_present; then
    info "模擬器 ${SERIAL} 已存在但尚未就緒,直接等待開機。"
  else
    mkdir -p "$RUN_DIR"
    info "啟動 AVD '${AVD_NAME}'(port ${EMU_PORT}, gpu ${EMU_GPU})…"
    info "模擬器日誌:${EMU_LOG}"
    # -no-window   無頭
    # -no-audio    無音訊(遠端機器通常沒有音效裝置)
    # -no-boot-anim 加快開機
    # -no-snapshot 每次乾淨開機,確保驗證可重現
    nohup "$EMULATOR" -avd "$AVD_NAME" \
      -port "$EMU_PORT" \
      -no-window \
      -no-audio \
      -no-boot-anim \
      $cold_boot \
      -gpu "$EMU_GPU" \
      -accel auto \
      -camera-back none -camera-front none \
      > "$EMU_LOG" 2>&1 &
    echo $! > "$PID_FILE"
    info "模擬器 pid=$(cat "$PID_FILE")"
  fi

  "$ADB" start-server >/dev/null 2>&1 || true

  local start_ts elapsed
  start_ts=$(date +%s)
  info "等待 ${SERIAL} 出現…"
  while ! emu_online; do
    elapsed=$(( $(date +%s) - start_ts ))
    if [ "$elapsed" -ge "$BOOT_TIMEOUT" ]; then
      die "等待 ${SERIAL} 上線逾時(${BOOT_TIMEOUT}s)。請看日誌:${EMU_LOG}"
    fi
    if [ -f "$PID_FILE" ] && ! kill -0 "$(cat "$PID_FILE")" 2>/dev/null; then
      die "模擬器行程已結束。請看日誌:${EMU_LOG}"
    fi
    sleep 2
  done
  info "adb 已連上,等待 sys.boot_completed=1 …"

  while ! boot_completed; do
    elapsed=$(( $(date +%s) - start_ts ))
    if [ "$elapsed" -ge "$BOOT_TIMEOUT" ]; then
      die "等待開機完成逾時(${BOOT_TIMEOUT}s)。請看日誌:${EMU_LOG}"
    fi
    sleep 2
  done

  # 等 package manager 真的可用(boot_completed 之後 pm 可能還在整理)
  local pm_ok=0
  while [ "$pm_ok" -eq 0 ]; do
    if "$ADB" -s "$SERIAL" shell pm path android >/dev/null 2>&1; then pm_ok=1; break; fi
    elapsed=$(( $(date +%s) - start_ts ))
    [ "$elapsed" -ge "$BOOT_TIMEOUT" ] && die "等待 package manager 就緒逾時。"
    sleep 2
  done

  elapsed=$(( $(date +%s) - start_ts ))
  info "開機完成,共花費 ${elapsed} 秒。"

  # 關掉會擋住畫面的動畫,讓截圖穩定
  "$ADB" -s "$SERIAL" shell settings put global window_animation_scale 0 >/dev/null 2>&1 || true
  "$ADB" -s "$SERIAL" shell settings put global transition_animation_scale 0 >/dev/null 2>&1 || true
  "$ADB" -s "$SERIAL" shell settings put global animator_duration_scale 0 >/dev/null 2>&1 || true
  # 解鎖螢幕
  "$ADB" -s "$SERIAL" shell input keyevent KEYCODE_WAKEUP >/dev/null 2>&1 || true
  "$ADB" -s "$SERIAL" shell wm dismiss-keyguard >/dev/null 2>&1 || true
}

cmd_stop() {
  check_tools
  if ! emu_present; then
    info "模擬器 ${SERIAL} 未在執行。"
  else
    info "關閉 ${SERIAL} …"
    "$ADB" -s "$SERIAL" emu kill >/dev/null 2>&1 || true
    local n=0
    while emu_present && [ "$n" -lt 30 ]; do sleep 1; n=$((n+1)); done
  fi
  if [ -f "$PID_FILE" ]; then
    local pid; pid="$(cat "$PID_FILE")"
    if kill -0 "$pid" 2>/dev/null; then
      info "行程 $pid 仍在,送 SIGTERM。"
      kill "$pid" 2>/dev/null || true
      sleep 3
      kill -9 "$pid" 2>/dev/null || true
    fi
    rm -f "$PID_FILE"
  fi
  info "已停止。"
}

cmd_status() {
  check_tools
  echo "AVD          : ${AVD_NAME}"
  echo "Serial       : ${SERIAL}"
  echo "SDK          : ${ANDROID_SDK_ROOT}"
  echo "模擬器日誌   : ${EMU_LOG}"
  if emu_online; then
    if boot_completed; then
      echo "狀態         : 執行中(開機完成)"
    else
      echo "狀態         : 執行中(開機尚未完成)"
    fi
    echo "Android 版本 : $("$ADB" -s "$SERIAL" shell getprop ro.build.version.release 2>/dev/null | tr -d '\r') (API $("$ADB" -s "$SERIAL" shell getprop ro.build.version.sdk 2>/dev/null | tr -d '\r'))"
    echo "預設輸入法   : $("$ADB" -s "$SERIAL" shell settings get secure default_input_method 2>/dev/null | tr -d '\r')"
    return 0
  elif emu_present; then
    echo "狀態         : 已連線但非 device 狀態"
    return 1
  else
    echo "狀態         : 未執行"
    return 1
  fi
}

cmd_install() {
  [ $# -ge 1 ] || die "用法:$0 install <apk> [more.apk ...]"
  check_tools
  require_running
  for apk in "$@"; do
    [ -f "$apk" ] || die "APK 不存在:$apk"
    info "安裝 $apk …"
    # -r 覆蓋安裝,-g 自動授權 runtime 權限,-t 允許 test-only build
    "$ADB" -s "$SERIAL" install -r -g -t "$apk"
  done
  info "安裝完成。"
}

cmd_shot() {
  local out="${1:-}"
  [ -n "$out" ] || die "用法:$0 shot <輸出png路徑>"
  check_tools
  require_running
  mkdir -p "$(dirname "$out")"
  # 走裝置端暫存檔再 pull,避免 exec-out 在某些平台的換行汙染
  local remote="/sdcard/.emu_shot_$$.png"
  "$ADB" -s "$SERIAL" shell screencap -p "$remote"
  "$ADB" -s "$SERIAL" pull "$remote" "$out" >/dev/null
  "$ADB" -s "$SERIAL" shell rm -f "$remote" || true
  [ -s "$out" ] || die "截圖失敗,檔案是空的:$out"
  info "截圖已存到 $out ($(wc -c < "$out") bytes)"
}

cmd_ime_list() {
  check_tools
  require_running
  echo "=== 已安裝(全部)==="
  "$ADB" -s "$SERIAL" shell ime list -a -s
  echo "=== 已啟用 ==="
  "$ADB" -s "$SERIAL" shell ime list -s
  echo "=== 目前預設 ==="
  "$ADB" -s "$SERIAL" shell settings get secure default_input_method
}

cmd_ime_enable() {
  local ime="${1:-}"
  [ -n "$ime" ] || die "用法:$0 ime-enable <ime_id>,例如 com.example/.MyInputMethodService"
  check_tools
  require_running

  if ! "$ADB" -s "$SERIAL" shell ime list -a -s 2>/dev/null | tr -d '\r' | grep -qxF "$ime"; then
    echo "[emu.sh] 系統目前看得到的輸入法:" >&2
    "$ADB" -s "$SERIAL" shell ime list -a -s | sed 's/^/  /' >&2
    die "輸入法 '$ime' 不在清單中。請確認 APK 已安裝、且 IME id 為 <package>/<service> 格式。"
  fi

  info "啟用 $ime …"
  "$ADB" -s "$SERIAL" shell ime enable "$ime"
  info "設為預設輸入法 …"
  "$ADB" -s "$SERIAL" shell ime set "$ime"
  sleep 1

  local cur
  cur="$("$ADB" -s "$SERIAL" shell settings get secure default_input_method 2>/dev/null | tr -d '\r')"
  if [ "$cur" != "$ime" ]; then
    die "設定預設輸入法失敗,目前仍是:$cur"
  fi
  info "目前預設輸入法:$cur"
}

cmd_logcat() {
  check_tools
  require_running
  if [ $# -eq 0 ]; then
    "$ADB" -s "$SERIAL" logcat -d -v time
  else
    local tag="$1"; shift
    # 只顯示指定 tag(任何等級),其餘靜音
    "$ADB" -s "$SERIAL" logcat -d -v time "$@" "${tag}:V" "*:S"
  fi
}

usage() {
  sed -n '2,30p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

main() {
  local cmd="${1:-}"
  [ $# -gt 0 ] && shift || true
  case "$cmd" in
    start)       cmd_start "$@" ;;
    stop)        cmd_stop "$@" ;;
    status)      cmd_status "$@" ;;
    install)     cmd_install "$@" ;;
    shot)        cmd_shot "$@" ;;
    ime-enable)  cmd_ime_enable "$@" ;;
    ime-list)    cmd_ime_list "$@" ;;
    logcat)      cmd_logcat "$@" ;;
    shell)       check_tools; require_running; "$ADB" -s "$SERIAL" shell "$@" ;;
    adb)         check_tools; "$ADB" -s "$SERIAL" "$@" ;;
    ""|-h|--help|help) usage ;;
    *)           echo "未知指令:$cmd" >&2; echo >&2; usage >&2; exit 1 ;;
  esac
}

main "$@"
