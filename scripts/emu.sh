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
#   RIME_EMU_PORT     模擬器 console port(`start` 在已有裝置在線時**必填**;
#                     port 不是身分,身分是 AVD 名 —— adb -s <serial> emu avd name)
#   RIME_SERIAL       要打哪一台(與其餘每一支腳本同一個變數)。唯讀子命令
#                     用它就夠了,不必再給 port。
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
BOOT_TIMEOUT="${RIME_BOOT_TIMEOUT:-600}"
EMU_GPU="${RIME_EMU_GPU:-swiftshader_indirect}"

RUN_DIR="${RIME_RUN_DIR:-$PROJECT_ROOT/.emulator}"

ADB="$ANDROID_SDK_ROOT/platform-tools/adb"
EMULATOR="$ANDROID_SDK_ROOT/emulator/emulator"
AVDMANAGER="$ANDROID_SDK_ROOT/cmdline-tools/latest/bin/avdmanager"

die()  { echo "[emu.sh] 錯誤: $*" >&2; exit 1; }
info() { echo "[emu.sh] $*" >&2; }

# ─────────────────────────────── 這一次打哪一台 ───────────────────────────────
#
# ⚠ **port 不是身分。** 這一支是**啟動者**,它有權決定 port;但它的預設值
#   從前是所有驗證腳本「預設打 5554」的正當性來源,而這台機器上長期有
#   三到四台模擬器在跑(rime_test/5554、lumina_test/5556、lumina_test2/5558),
#   任何一台重開 port 就換人。所以:要啟動時**要求明著指定**。
#
# ⛔ 2026-08-13 的實測:上一版把這一段寫在**子命令派發之前**,於是
#   `status` / `shot` / `logcat` / `shell` / `adb` / `install` / `ime-list`
#   ——連 `--help` 與不帶參數的用法——在任何有裝置在線的機器上一律 exit 2。
#   `scripts/build_schema_store.sh:118` 是
#       emu.sh status >/dev/null 2>&1 || emu.sh start
#   而該檔有 `set -euo pipefail` —— **方案市集的品質閘門因此必死**,
#   訊息還指向 emu.sh、與方案無關。
#
# 現在:序號在**子命令要用到裝置時**才解析,而且 `RIME_SERIAL` /
# `ANDROID_SERIAL`(其餘每一支腳本指名裝置的方式)算數 —— 唯讀子命令因此
# 不必再指定 port。政策與 `lib/device.sh` 同一條:
#
#   RIME_EMU_PORT 有值            → 用它(這是啟動者專屬的指名方式)
#   RIME_SERIAL / ANDROID_SERIAL  → 用它
#   都沒有,而且恰好一台在線       → 用它(`start` 除外:啟動者不准用猜的)
#   都沒有,而且一台都沒有         → port 5554(`start` 的預設;其餘子命令
#                                  也用它,反正接下來一定報「未執行」)
#   都沒有,而且不只一台在線       → **不猜**,中止並印出清單
#
EMU_PORT=""
SERIAL=""
EMU_LOG=""
PID_FILE=""
TARGET_SOURCE=""

# $1 = 子命令名(只影響訊息與 start 的「不准用猜的」)
resolve_target() {
  local cmd="$1" want="${RIME_SERIAL:-${ANDROID_SERIAL:-}}" listed n
  if [ -n "${RIME_EMU_PORT:-}" ]; then
    if [ -n "$want" ] && [ "$want" != "emulator-${RIME_EMU_PORT}" ]; then
      die "RIME_EMU_PORT=${RIME_EMU_PORT} 與 RIME_SERIAL/ANDROID_SERIAL=${want} 指到不同機器。"
    fi
    EMU_PORT="$RIME_EMU_PORT"; SERIAL="emulator-${EMU_PORT}"; TARGET_SOURCE="RIME_EMU_PORT"
  elif [ -n "$want" ]; then
    SERIAL="$want"; TARGET_SOURCE="RIME_SERIAL"
    case "$SERIAL" in
      emulator-*) EMU_PORT="${SERIAL#emulator-}" ;;
      *) [ "$cmd" = "start" ] && die "RIME_SERIAL=$SERIAL 不是模擬器序號,起不動它。"
         EMU_PORT="" ;;
    esac
  else
    listed="$("$ADB" devices 2>/dev/null || true)"
    n="$(printf '%s\n' "$listed" | grep -cE '	device$' || true)"
    if [ "${n:-0}" -eq 0 ]; then
      # ⚠ 序號由 port 推導,不寫死 —— 這一支是唯一有權決定 port 的檔案,
      #   但「emulator-5554」這串字面本身仍然是 verify_device_hygiene.sh 規則 A
      #   要抓的東西(port 不是身分)。
      EMU_PORT=5554; SERIAL="emulator-${EMU_PORT}"; TARGET_SOURCE="預設(場上沒有裝置)"
    elif [ "${n:-0}" -eq 1 ] && [ "$cmd" != "start" ]; then
      SERIAL="$(printf '%s\n' "$listed" | awk '/	device$/{print $1; exit}')"
      case "$SERIAL" in emulator-*) EMU_PORT="${SERIAL#emulator-}" ;; *) EMU_PORT="" ;; esac
      TARGET_SOURCE="自動(只有一台在線)"
    else
      {
        echo "[emu.sh] 這台機器上有 ${n} 台裝置在線,不猜要打哪一台。"
        echo "         明著寫:RIME_SERIAL=emulator-XXXX $0 $cmd ..."
        echo "         要啟動一台新的:RIME_EMU_PORT=<port> RIME_AVD=<avd 名> $0 start"
        printf '%s\n' "$listed" | sed 's/^/    /'
      } >&2
      exit 2
    fi
  fi
  local tag="${EMU_PORT:-$SERIAL}"
  EMU_LOG="${RIME_EMU_LOG:-$RUN_DIR/emulator-${tag}.log}"
  PID_FILE="$RUN_DIR/emulator-${tag}.pid"
}

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
  [ -n "$EMU_PORT" ] || die "起不動:沒有 port。明著寫 RIME_EMU_PORT=<port>。"

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
  # ⚠ 印**這台裝置真的是哪個 AVD**,不是 $RIME_AVD 的預設值。序號由
  #   RIME_SERIAL 指名時,`AVD_NAME` 還停在 `rime_test`,照著印等於騙人 ——
  #   而「身分是 AVD 名不是 port」正是這一支自己寫在檔頭的那句話。
  local avd_live=""
  if emu_online; then
    avd_live="$("$ADB" -s "$SERIAL" emu avd name 2>/dev/null | head -1 | tr -d '\r')"
    case "$avd_live" in ''|OK|*error*) avd_live="" ;; esac
    [ -n "$avd_live" ] || avd_live="$("$ADB" -s "$SERIAL" shell getprop ro.boot.qemu.avd_name 2>/dev/null | tr -d '\r')"
  fi
  echo "AVD          : ${avd_live:-${AVD_NAME}（\$RIME_AVD 的值,裝置未在線問不到)}"
  echo "Serial       : ${SERIAL}  (來源:${TARGET_SOURCE})"
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
  # ⚠ 說明與未知指令**不解析裝置**:它們一個字都不打在任何機器上,
  #   而「連 --help 都 exit 2」正是上一版的形狀。
  case "$cmd" in
    ""|-h|--help|help) usage; return 0 ;;
    start|stop|status|install|shot|ime-enable|ime-list|logcat|shell|adb) resolve_target "$cmd" ;;
    *)           echo "未知指令:$cmd" >&2; echo >&2; usage >&2; exit 1 ;;
  esac
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
  esac
}

main "$@"
