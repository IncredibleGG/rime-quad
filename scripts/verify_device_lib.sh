#!/usr/bin/env bash
#
# verify_device_lib.sh — `scripts/lib/device.sh` 的**行為**測試
#
# ── 為什麼需要這一支 ────────────────────────────────────────────────────
# `verify_device_hygiene.sh` 查的是「呼叫點在不在」:誰 source 了、誰呼叫了
# `rs_pick_serial`、誰在破壞性動作前寫了 `rs_assert_destructive_ok`。
# 它**查不到閘有沒有用**。實測(2026-08-13):在 `rs_assert_destructive_ok`
# 第一行插一個 `return 0` —— 也就是讓每一支腳本都能去 `pm clear` 別條線的
# 模擬器 —— 全樹沒有任何東西會叫,`verify_device_hygiene.sh` 照樣綠。
#
# 這一支補的就是那一塊:**閘自己的行為**。不需要任何裝置(adb 用假的)。
#
# ── 兩個模式 ────────────────────────────────────────────────────────────
#   verify_device_lib.sh             跑一遍,任何一條不符就紅
#   verify_device_lib.sh --self-test 反向:把閘拆掉(植入 `return 0`),
#                                    上面那一遍**必須**變紅。沒有這一段,
#                                    「測過了」與「測了個寂寞」長得一模一樣。
#
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

SELF_TEST=0
DEVICE_SH="$ROOT/scripts/lib/device.sh"
while [ $# -gt 0 ]; do
  case "$1" in
    --self-test) SELF_TEST=1; shift ;;
    --lib) DEVICE_SH="$2"; shift 2 ;;
    -h|--help) sed -n '2,/^set -uo pipefail$/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "未知參數:$1" >&2; exit 2 ;;
  esac
done

# ── 假 adb:`devices` 的輸出由 FAKE_DEVICES 決定,其餘一律有問必答 ────────
FAKE_DIR="$(mktemp -d)"
trap 'rm -rf "$FAKE_DIR"' EXIT
cat > "$FAKE_DIR/adb" <<'FAKE'
#!/usr/bin/env bash
# 假 adb。只認得這一支測試會用到的幾種問法。
args=("$@")
if [ "${args[0]:-}" = "devices" ]; then
  echo "List of devices attached"
  printf '%s\n' "${FAKE_DEVICES:-}"
  exit 0
fi
# adb -s <serial> ...
serial=""
if [ "${args[0]:-}" = "-s" ]; then serial="${args[1]:-}"; args=("${args[@]:2}"); fi
case "${args[0]:-}:${args[1]:-}" in
  emu:avd) echo "${FAKE_AVD_NAME:-avd_of_${serial}}"; echo "OK" ;;
  shell:getprop) echo "${FAKE_AVD_NAME:-avd_of_${serial}}" ;;
  *) echo "" ;;
esac
exit 0
FAKE
chmod +x "$FAKE_DIR/adb"
ADB="$FAKE_DIR/adb"

TWO=$'emulator-5554\tdevice\nemulator-5558\tdevice'
ONE=$'emulator-5558\tdevice'
NONE=""

PASS=0; FAIL=0
ok()   { PASS=$((PASS+1)); echo "  [PASS] $*"; }
bad()  { FAIL=$((FAIL+1)); echo "  [FAIL] $*" >&2; }

# 每一條都跑在**子行程**裡:RS_SERIAL / RS_SERIAL_SOURCE 是全域變數,
# 一條測試污染下一條的話,綠燈就不是這一條掙來的。
run_case() {
  local desc="$1" want_rc="$2" want_out="$3"; shift 3
  local out rc
  out="$(env "$@" bash -c '
    set -uo pipefail
    . "$DEVICE_SH"
    eval "$RS_CASE"
  ' 2>/dev/null)"
  rc=$?
  if [ "$rc" != "$want_rc" ]; then
    bad "$desc:RC=$rc(要 $want_rc),stdout=[$out]"; return
  fi
  if [ -n "$want_out" ] && [ "$out" != "$want_out" ]; then
    bad "$desc:stdout=[$out](要 [$want_out])"; return
  fi
  ok "$desc"
}

echo "=== lib/device.sh 的行為(受測檔:$DEVICE_SH)==="

echo
echo "-- rs_pick_serial --"
run_case "指名 5558 而它在線 → 選它" 0 "emulator-5558" \
  DEVICE_SH="$DEVICE_SH" FAKE_DEVICES="$TWO" RIME_SERIAL=emulator-5558 \
  RS_CASE='rs_pick_serial "$ADB"' ADB="$ADB"
run_case "指名 5556 而它不在線 → RC 2" 2 "" \
  DEVICE_SH="$DEVICE_SH" FAKE_DEVICES="$TWO" RIME_SERIAL=emulator-5556 \
  RS_CASE='rs_pick_serial "$ADB"' ADB="$ADB"
run_case "沒指名、恰好一台 → 自動選" 0 "emulator-5558" \
  DEVICE_SH="$DEVICE_SH" FAKE_DEVICES="$ONE" \
  RS_CASE='rs_pick_serial "$ADB"' ADB="$ADB"
run_case "沒指名、兩台在線 → 不猜,RC 2" 2 "" \
  DEVICE_SH="$DEVICE_SH" FAKE_DEVICES="$TWO" \
  RS_CASE='rs_pick_serial "$ADB"' ADB="$ADB"
run_case "沒指名、一台都沒有 → RC 2" 2 "" \
  DEVICE_SH="$DEVICE_SH" FAKE_DEVICES="$NONE" \
  RS_CASE='rs_pick_serial "$ADB"' ADB="$ADB"
run_case "ANDROID_SERIAL 也算指名" 0 "emulator-5554" \
  DEVICE_SH="$DEVICE_SH" FAKE_DEVICES="$TWO" ANDROID_SERIAL=emulator-5554 \
  RS_CASE='rs_pick_serial "$ADB"' ADB="$ADB"

echo
echo "-- rs_select_device:序號的**來源**要記得住 --"
run_case "--serial 指名 → source=flag" 0 "emulator-5554 flag" \
  DEVICE_SH="$DEVICE_SH" FAKE_DEVICES="$TWO" \
  RS_CASE='rs_select_device "$ADB" emulator-5554 && echo "$RS_SERIAL $RS_SERIAL_SOURCE"' ADB="$ADB"
run_case "環境變數指名 → source=env" 0 "emulator-5558 env" \
  DEVICE_SH="$DEVICE_SH" FAKE_DEVICES="$TWO" RIME_SERIAL=emulator-5558 \
  RS_CASE='rs_select_device "$ADB" "" && echo "$RS_SERIAL $RS_SERIAL_SOURCE"' ADB="$ADB"
run_case "都沒指名、恰好一台 → source=auto" 0 "emulator-5558 auto" \
  DEVICE_SH="$DEVICE_SH" FAKE_DEVICES="$ONE" \
  RS_CASE='rs_select_device "$ADB" "" && echo "$RS_SERIAL $RS_SERIAL_SOURCE"' ADB="$ADB"
run_case "--serial 指到不在線的機器 → RC 2" 2 "" \
  DEVICE_SH="$DEVICE_SH" FAKE_DEVICES="$TWO" \
  RS_CASE='rs_select_device "$ADB" emulator-9999' ADB="$ADB"
run_case "--serial 與環境變數指到不同機器 → RC 2" 2 "" \
  DEVICE_SH="$DEVICE_SH" FAKE_DEVICES="$TWO" RIME_SERIAL=emulator-5554 \
  RS_CASE='rs_select_device "$ADB" emulator-5558' ADB="$ADB"
run_case "殘留的 RS_SERIAL_SOURCE 不得冒充這一次的答案" 2 "" \
  DEVICE_SH="$DEVICE_SH" FAKE_DEVICES="$TWO" RS_SERIAL_SOURCE=flag RS_SERIAL=emulator-5554 \
  RS_CASE='rs_select_device "$ADB" "" ; rc=$?; [ "$rc" = 2 ] || exit 9;
           [ -z "$RS_SERIAL_SOURCE" ] || exit 8; exit 2' ADB="$ADB"

echo
echo "-- rs_assert_destructive_ok:閘**本身**的行為 --"
run_case "⛔ 自動選來的那一台不准做破壞性動作" 2 "" \
  DEVICE_SH="$DEVICE_SH" FAKE_DEVICES="$ONE" \
  RS_CASE='rs_select_device "$ADB" "" && rs_assert_destructive_ok "$ADB" "$RS_SERIAL" "pm clear"' ADB="$ADB"
run_case "✅ --serial 指名的那一台准" 0 "" \
  DEVICE_SH="$DEVICE_SH" FAKE_DEVICES="$TWO" \
  RS_CASE='rs_select_device "$ADB" emulator-5558 && rs_assert_destructive_ok "$ADB" "$RS_SERIAL" "pm clear"' ADB="$ADB"
run_case "✅ 環境變數指名的那一台准" 0 "" \
  DEVICE_SH="$DEVICE_SH" FAKE_DEVICES="$TWO" RIME_SERIAL=emulator-5558 \
  RS_CASE='rs_select_device "$ADB" "" && rs_assert_destructive_ok "$ADB" "$RS_SERIAL" "pm clear"' ADB="$ADB"
run_case "✅ 沒走 rs_select_device 的舊呼叫端:環境變數指名就准" 0 "" \
  DEVICE_SH="$DEVICE_SH" FAKE_DEVICES="$TWO" RIME_SERIAL=emulator-5558 \
  RS_CASE='S="$(rs_pick_serial "$ADB")" && rs_assert_destructive_ok "$ADB" "$S" "pm clear"' ADB="$ADB"
run_case "⛔ 沒走 rs_select_device 的舊呼叫端:沒指名就不准" 2 "" \
  DEVICE_SH="$DEVICE_SH" FAKE_DEVICES="$ONE" \
  RS_CASE='S="$(rs_pick_serial "$ADB")" && rs_assert_destructive_ok "$ADB" "$S" "pm clear"' ADB="$ADB"
run_case "⛔ RIME_AVD_EXPECT 對不上就不准(port 會換人)" 2 "" \
  DEVICE_SH="$DEVICE_SH" FAKE_DEVICES="$TWO" FAKE_AVD_NAME=rime_test \
  RIME_AVD_EXPECT=lumina_test2 \
  RS_CASE='rs_select_device "$ADB" emulator-5558 && rs_assert_destructive_ok "$ADB" "$RS_SERIAL" "pm clear"' ADB="$ADB"
run_case "✅ RIME_AVD_EXPECT 對得上就准" 0 "" \
  DEVICE_SH="$DEVICE_SH" FAKE_DEVICES="$TWO" FAKE_AVD_NAME=lumina_test2 \
  RIME_AVD_EXPECT=lumina_test2 \
  RS_CASE='rs_select_device "$ADB" emulator-5558 && rs_assert_destructive_ok "$ADB" "$RS_SERIAL" "pm clear"' ADB="$ADB"
run_case "⛔ 選的是這台、要動的是那台 → 那份「來源」證不了這一台" 2 "" \
  DEVICE_SH="$DEVICE_SH" FAKE_DEVICES="$TWO" \
  RS_CASE='rs_select_device "$ADB" emulator-5558 && rs_assert_destructive_ok "$ADB" emulator-5554 "pm clear"' ADB="$ADB"

echo
echo "-- rs_write_device_stamp:artifact 指得出這一輪打在哪一台 --"
run_case "device.txt 寫得出 serial 與 avd" 0 "serial=emulator-5558 avd=lumina_test2" \
  DEVICE_SH="$DEVICE_SH" FAKE_DEVICES="$TWO" FAKE_AVD_NAME=lumina_test2 \
  RS_CASE='f="$(mktemp)"; rs_write_device_stamp "$ADB" emulator-5558 "$f" "" "" >/dev/null 2>&1;
           printf "%s %s" "$(grep ^serial= "$f")" "$(grep ^avd= "$f")"; rm -f "$f"' ADB="$ADB"

echo
if [ "$SELF_TEST" -eq 0 ]; then
  echo "═══ lib/device.sh 行為測試:$PASS 通過、$FAIL 失敗 ═══"
  [ "$FAIL" -eq 0 ] || exit 1
  echo "✓ 閘不只「有被呼叫」,它真的擋得住"
  exit 0
fi

# ── 反向:把閘拆掉,上面那一整組必須紅 ────────────────────────────────────
echo "=== --self-test:把 rs_assert_destructive_ok 的第一行換成 return 0 ==="
BROKEN="$FAKE_DIR/device_broken.sh"
python3 - "$ROOT/scripts/lib/device.sh" "$BROKEN" <<'PY'
import io, sys
src, dst = sys.argv[1], sys.argv[2]
s = io.open(src, encoding="utf-8").read()
anchor = "rs_assert_destructive_ok() {\n"
assert anchor in s, "找不到 rs_assert_destructive_ok —— 反向測試失效了"
s = s.replace(anchor, anchor + "  return 0  # <<< --self-test 植入:閘被拆掉\n", 1)
io.open(dst, "w", encoding="utf-8").write(s)
PY
if "$HERE/verify_device_lib.sh" --lib "$BROKEN" >/dev/null 2>&1; then
  echo "  [FAIL] 閘被拆掉了而這一支照樣綠 —— 那它什麼都沒在守" >&2
  echo
  echo "═══ 反向驗證:1 項失敗 ═══"
  exit 1
fi
echo "  [PASS] 閘一被拆掉,這一支立刻紅"
echo
echo "═══ 反向驗證:0 項失敗 ═══"
exit 0
