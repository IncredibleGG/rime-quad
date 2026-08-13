# shellcheck shell=bash
#
# device.sh — **唯一**的裝置選擇入口。fail-closed:寧可不跑,也不要跑在別人的機器上。
#
# ── 為什麼需要這一支 ────────────────────────────────────────────────────
# 每一支裝置端腳本原本各自寫著
#
#     SERIAL="${RIME_SERIAL:-${ANDROID_SERIAL:-emulator-${RIME_EMU_PORT:-5554}}}"
#
# 或者「抓 adb devices 的第一台」。這台建置機上長期有三到四台模擬器在跑,
# `adb devices` 以 port 升冪列出,所以**兩種寫法都會落在 emulator-5554 上**。
#
# 猜錯的代價不是「腳本失敗」,是:
#
#   · `pm clear` 清掉**別條線**正在用的那台上的 app 資料(實測發生過一次);
#   · `ime set` 改掉那台的系統預設輸入法;
#   · 量到的是那台上的**舊 APK** —— 於是整輪綠燈與紅燈都是別人的,
#     而 artifact 裡沒有任何一個檔案記著序號,事後看不出來。
#
# ── ⛔ 這裡沒有預設 port,而且不會有 ──────────────────────────────────
# `docs/emulator.md` 曾經寫著「同時只跑一台模擬器時 serial 固定是
# emulator-5554」。那句話從寫下的那天起就不成立,而它正是所有腳本把 5554
# 當預設的正當性來源。**port 不是身分**,實測:
#
#     -avd rime_test    -port 5554   (Aug  8)
#     -avd lumina_test  (無 -port)   (Aug 11) → 落在 5556
#     -avd lumina_test2 -port 5558   (Aug 13)
#
# 任何一台重開,port 就會換人。身分是 AVD 名字,問法:
#
#     adb -s <serial> emu avd name
#     adb -s <serial> shell getprop ro.boot.qemu.avd_name
#
# 用法
# ─────────────────────────────────────────────────────────────────────
#     . "$HERE/lib/device.sh"
#     SERIAL="$(rs_pick_serial "$ADB")" || exit 2
#     ...
#     rs_assert_destructive_ok "$ADB" "$SERIAL" "pm clear / ime set" || exit 2
#     rs_write_device_stamp "$ADB" "$SERIAL" "$OUT_DIR/device.txt" "$APK"

# 選一台裝置。成功時把序號印到 stdout(其餘一律到 stderr)。
#
#   RIME_SERIAL / ANDROID_SERIAL 有值 → 只用那一台,不在線就中止
#   兩個都沒有,而且**恰好一台**在線  → 用它,並在 stderr 說出來
#   兩個都沒有,而且不只一台在線      → **不猜**,中止並印出清單
rs_pick_serial() {
  local adb="${1:?rs_pick_serial 要 adb 路徑}"
  local want="${RIME_SERIAL:-${ANDROID_SERIAL:-}}" listed n
  # ⚠ 不接管線:`set -o pipefail` 之下上游提前結束會變成 141,
  #   於是「有裝置」被判成「指令失敗」。
  listed="$("$adb" devices 2>/dev/null)"
  if [ -n "$want" ]; then
    if printf '%s\n' "$listed" | grep -q "^${want}	device$"; then
      printf '%s\n' "$want"
      return 0
    fi
    {
      echo "指定的 $want 不在線。目前:"
      printf '%s\n' "$listed" | sed 's/^/    /'
    } >&2
    return 2
  fi
  n="$(printf '%s\n' "$listed" | grep -cE '	device$')"
  if [ "$n" -eq 1 ]; then
    printf '%s\n' "$listed" | awk '/\tdevice$/{print $1; exit}'
    echo "[device] 只有一台裝置在線,自動選用。要指定請設 RIME_SERIAL。" >&2
    return 0
  fi
  {
    echo "這台機器上有 $n 台裝置在線,**不猜** —— 猜錯會 pm clear 別人的模擬器,"
    echo "而且量到的是別人的 APK。請顯式指定:"
    printf '%s\n' "$listed" | sed 's/^/    /'
    echo "    RIME_SERIAL=emulator-XXXX $0 ..."
  } >&2
  return 2
}

# 這台裝置的**身分**(AVD 名)。問不到就印空字串。
rs_avd_name() {
  local adb="${1:?}" serial="${2:?}" name
  name="$("$adb" -s "$serial" emu avd name 2>/dev/null | head -1 | tr -d '\r')"
  case "$name" in ''|OK|*error*) name="" ;; esac
  [ -n "$name" ] || name="$("$adb" -s "$serial" shell getprop ro.boot.qemu.avd_name 2>/dev/null | tr -d '\r')"
  printf '%s\n' "$name"
}

# 破壞性動作(pm clear / uninstall / ime set / settings put)之前的閘。
#
# 兩道:
#   1. 序號必須是**明著指定**的。自動選來的那一台不准被清資料 ——
#      `release_check.sh` 已經有這個先例(「未指定 RIME_SERIAL,沒有清空 app 資料」)。
#   2. 設了 RIME_AVD_EXPECT 的話,AVD 名字要對得上(port 會換人)。
rs_assert_destructive_ok() {
  local adb="${1:?}" serial="${2:?}" avd
  shift 2
  if [ -z "${RIME_SERIAL:-${ANDROID_SERIAL:-}}" ]; then
    {
      echo "這一關會做破壞性動作($*)。"
      echo "$serial 是自動選來的,不是你指定的 —— 中止。"
      echo "要跑就明著寫:RIME_SERIAL=$serial $0 ..."
    } >&2
    return 2
  fi
  avd="$(rs_avd_name "$adb" "$serial")"
  if [ -n "${RIME_AVD_EXPECT:-}" ] && [ "$avd" != "$RIME_AVD_EXPECT" ]; then
    echo "$serial 現在是 avd=${avd:-<問不到>},不是你要的 $RIME_AVD_EXPECT —— 中止(port 會換人)。" >&2
    return 2
  fi
  echo "[device] $serial (avd=${avd:-?}) — 即將:$*" >&2
  return 0
}

# 把「這一輪跑在哪一台、量的是哪一份 APK」寫進 artifact。
#
# ⚠ 沒有這一步,「腳本綠了」在事後永遠無法覆核:這台機器上三台 AVD 全是
#   1080×2400 @420dpi,**截圖本身分不出是哪一台**。
rs_write_device_stamp() {
  local adb="${1:?}" serial="${2:?}" out="${3:?}" apk="${4:-}"
  {
    echo "serial=$serial"
    echo "avd=$(rs_avd_name "$adb" "$serial")"
    echo "fingerprint=$("$adb" -s "$serial" shell getprop ro.build.fingerprint 2>/dev/null | tr -d '\r')"
    echo "screen=$("$adb" -s "$serial" shell wm size 2>/dev/null | tr -d '\r' | tail -1)"
    echo "density=$("$adb" -s "$serial" shell wm density 2>/dev/null | tr -d '\r' | tail -1)"
    if [ -n "$apk" ] && [ -f "$apk" ]; then
      echo "apk=$apk"
      echo "apk_sha256=$(sha256sum "$apk" 2>/dev/null | awk '{print $1}')"
    fi
    echo "when=$(date -Iseconds)"
  } > "$out"
  echo "[device] 這一輪的裝置與 APK 記在 $out" >&2
}
