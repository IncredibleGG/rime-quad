#!/usr/bin/env bash
#
# verify_insets.sh — 鍵盤有沒有把宿主 App 的內容蓋掉
#
# ── 這支腳本存在的理由,以及一個被推翻的推論 ──────────────────────────────
#
# 有一份回報說:`RimeInputMethodService` 沒有覆寫 `onComputeInsets`,
# `dumpsys input_method` 量到 `contentTopInsets=0`、`touchableRegion=SkRegion()`,
# 換成 Gboard 則是 1983 與 (0,1983,1440,2892);結論是「本輸入法宣稱不佔空間,
# 宿主不會縮排版,鍵盤直接蓋住輸入框」。
#
# **實測下來這個結論是錯的,而且照著它去加覆寫會真的做出那個缺陷。**
# 這一段留著,因為下一個人量到同樣的數字時會做出同樣的推論:
#
#   1. `contentTopInsets` 是**相對於輸入法自己那個視窗**的座標,不是螢幕座標。
#      本專案的輸入法視窗是 `gr=BOTTOM (fillxwrap)` —— 貼著螢幕下緣、
#      高度剛好等於鍵盤。`InputMethodService` 預設的 `onComputeInsets` 算的是
#      `mInputFrame.getLocationInWindow()`,輸入區就在那個視窗的最上面,
#      所以 **0 是正確答案**,不是「宣稱佔滿螢幕」。
#      Gboard 量到 1983 是因為**它的視窗是整片螢幕**(frame=[0,76][1440,3120]),
#      同一個欄位在兩種視窗形狀下本來就不可能是同一個數字。
#
#   2. `touchableRegion` 只在 `touchableInsets == TOUCHABLE_INSETS_REGION` 時
#      才會被填。預設是 `TOUCHABLE_INSETS_VISIBLE`,所以那裡印 `SkRegion()`
#      是預期的。**真正生效的可觸區在 `dumpsys window`**,實測是
#      `SkRegion((0,2081,1440,3120))` —— 非空,而且正好等於鍵盤視窗的 frame。
#
#   3. Android 11 之後,宿主拿到的 IME inset 由 WindowManager 從**鍵盤視窗的
#      frame** 推導(`WindowInsets.Type.ime()`),不再走 `contentTopInsets`
#      那條舊路。所以那個欄位是 0 還是 1983,對宿主的排版**沒有影響**。
#
# 也就是說:那三個數字沒有一個能回答「使用者看不看得到自己正在打的字」。
# 能回答的只有一件事 —— **去問宿主**。這支腳本就是去問宿主。
#
# ── 怎麼問 ───────────────────────────────────────────────────────────────
#
# `scripts/build_testapp.sh --ez bottom true` 的靶會多一個貼在畫面下緣的輸入框,
# 並安裝 `OnApplyWindowInsetsListener`,把量到的 `Type.ime()` bottom 寫進標題的
# content-desc。於是有兩個彼此獨立的判準:
#
#   · **數字**:宿主收到的 ime inset 必須等於鍵盤視窗的高度(容許 2px 捨入)。
#   · **幾何**:下緣輸入框的 bottom 不得越過鍵盤視窗的 top。
#
# 兩個都過才算過。只驗數字會漏掉「inset 對但宿主沒套用」;只驗幾何會漏掉
# 「這一版剛好排得下」。
#
# ⚠ **`android:windowSoftInputMode="adjustResize"` 不能當判準。** 這一點是實測
#    換來的:`SOFT_INPUT_ADJUST_RESIZE` 自 API 30 起棄用,targetSdk 35 又強制
#    edge-to-edge,於是不消費 insets 的 Activity 連標題都畫到狀態列底下,
#    下緣框當然被蓋住 —— **而且換成 Gboard 一模一樣**。拿那個畫面去指控輸入法
#    會指控錯對象。所以本腳本的靶一定要裝那個監聽器。
#
# ── 這支腳本自己會不會安靜地不跑 ─────────────────────────────────────────
#
# 會,所以有一道**反向對照**:同一個靶再跑一次 `--ez insets false`(不裝監聽器)。
# 那一次下緣框一定會被蓋住;如果檢查竟然判它過,就是檢查本身壞了,
# 這支腳本會失敗並明說是自己壞了,而不是報一個沒驗到的綠。
#
# 用法:
#   ./verify_insets.sh --ime "$RS_ANDROID_IME_ID" \           # 值見 scripts/lib/product.env
#                      [--apk <path>]... [--out <dir>]
#
# 環境變數:
#   RIME_SERIAL / ANDROID_SERIAL   指定模擬器
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
SDK="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Android/Sdk}}"
ADB="$SDK/platform-tools/adb"

IME_ID=""
APKS=()
OUT_DIR="$ROOT/build/verify-insets"
TARGET_APK=""
SETTLE=8

while [ $# -gt 0 ]; do
  case "$1" in
    --ime)     IME_ID="$2"; shift 2 ;;
    --apk)     APKS+=("$2"); shift 2 ;;
    --target)  TARGET_APK="$2"; shift 2 ;;
    --out)     OUT_DIR="$2"; shift 2 ;;
    --settle)  SETTLE="$2"; shift 2 ;;
    -h|--help) sed -n '2,70p' "$0"; exit 0 ;;
    *) echo "未知參數: $1" >&2; exit 2 ;;
  esac
done

[ -n "$IME_ID" ] || { echo "缺少 --ime" >&2; exit 2; }
mkdir -p "$OUT_DIR"

pass() { echo "  [PASS] $*"; }
info() { echo "  [INFO] $*"; }
step() { echo; echo "=== $* ==="; }

# 見 coordination.md §3:錯誤訊息指到的檔案,必須在**這條失敗路徑上**真的存在。
dump_on_fail() {
  [ -n "${SERIAL:-}" ] || return 0
  [ -d "${OUT_DIR:-}" ] || return 0
  "$ADB" -s "$SERIAL" logcat -d               > "$OUT_DIR/logcat.txt"        2>/dev/null || true
  "$ADB" -s "$SERIAL" shell dumpsys input_method > "$OUT_DIR/input_method.txt" 2>/dev/null || true
  "$ADB" -s "$SERIAL" shell dumpsys window windows > "$OUT_DIR/window.txt"   2>/dev/null || true
  echo "  [INFO] 已存 dumpsys 與 logcat:$OUT_DIR" >&2
}
fail() { echo "  [FAIL] $*" >&2; dump_on_fail; echo >&2; echo "artifact 在:$OUT_DIR" >&2; exit 1; }

SERIAL="${RIME_SERIAL:-${ANDROID_SERIAL:-}}"
if [ -z "$SERIAL" ]; then
  # 為什麼不用管線接 grep:見 coordination.md §3,`set -o pipefail` 配
  # 提前結束的下游會把成功變成失敗。先把輸出收進變數再處理。
  devices="$("$ADB" devices)"
  SERIAL="$(printf '%s\n' "$devices" | awk '/emulator-[0-9]+\tdevice/{print $1; exit}')"
fi
[ -n "$SERIAL" ] || fail "找不到已連線的模擬器(可用 RIME_SERIAL 指定)"
adbs() { "$ADB" -s "$SERIAL" "$@"; }
info "裝置:$SERIAL"

# ─────────────────────────────── 安裝 ───────────────────────────────

step "安裝"

for apk in "${APKS[@]:-}"; do
  [ -n "$apk" ] || continue
  [ -f "$apk" ] || fail "找不到 APK:$apk"
  adbs install -r "$apk" >/dev/null 2>&1 || {
    pkg="$(basename "$apk")"
    info "install -r 失敗($pkg),改成先移除再裝"
    adbs uninstall "${IME_ID%%/*}" >/dev/null 2>&1 || true
    adbs install "$apk" >/dev/null || fail "安裝 $apk 失敗"
  }
  info "已安裝 $(basename "$apk")"
done

if [ -z "$TARGET_APK" ]; then
  TARGET_APK="$ROOT/build/imetest/rime-imetest.apk"
  if [ ! -f "$TARGET_APK" ]; then
    info "測試靶不存在,建一份 …"
    bash "$HERE/build_testapp.sh" >/dev/null || fail "build_testapp.sh 失敗"
  fi
fi
[ -f "$TARGET_APK" ] || fail "找不到測試靶 APK:$TARGET_APK"

# 簽章不合時 install -r 會失敗(別的 worktree 用不同的 debug keystore 建過)。
adbs install -r "$TARGET_APK" >/dev/null 2>&1 || {
  adbs uninstall dev.rime.imetest >/dev/null 2>&1 || true
  adbs install "$TARGET_APK" >/dev/null || fail "安裝測試靶失敗:$TARGET_APK"
}
info "已安裝測試靶 $(basename "$TARGET_APK")"

adbs shell ime enable "$IME_ID" >/dev/null || fail "ime enable 失敗:$IME_ID"
adbs shell ime set "$IME_ID"    >/dev/null || fail "ime set 失敗:$IME_ID"
sleep 2

# ──────────────────────────── 量測的兩個原語 ────────────────────────────

# 鍵盤視窗在螢幕上的 frame。**從 dumpsys window 讀,不是從 dumpsys input_method**
# ——後者印的是輸入法自己回報的 Insets 物件(視窗內座標),不是系統實際採用的矩形。
ime_window_top() {
  local w
  w="$(adbs shell dumpsys window windows 2>/dev/null || true)"
  printf '%s\n' "$w" \
    | awk '/u0 InputMethod\}/{f=1} f && /^    Frames:/{print; exit}' \
    | sed -n 's/.*frame=\[\([0-9]*\),\([0-9]*\)\]\[\([0-9]*\),\([0-9]*\)\].*/\2 \4/p'
}

dump_ui() {
  adbs shell uiautomator dump /sdcard/rime-insets-ui.xml >/dev/null 2>&1 || return 1
  adbs shell cat /sdcard/rime-insets-ui.xml
}

# 從 uiautomator 的 XML 抓 content-desc 對應的 bounds / 數字。
py_extract() {
  python3 - "$1" "$2" <<'PY'
import re, sys
xml, what = open(sys.argv[1], encoding="utf-8", errors="replace").read(), sys.argv[2]
if what == "ime_inset":
    m = re.search(r'content-desc="ime_inset_bottom=(-?\d+)"', xml)
    print(m.group(1) if m else "")
else:
    m = re.search(r'content-desc="%s"[^>]*?bounds="\[(-?\d+),(-?\d+)\]\[(-?\d+),(-?\d+)\]"' % what, xml)
    print(" ".join(m.groups()) if m else "")
PY
}

# 跑一次:啟動靶 → 等鍵盤 → 回報 (ime_inset, 下緣框 bottom, 鍵盤視窗 top)
measure() {
  local tag="$1"; shift
  adbs shell am force-stop dev.rime.imetest >/dev/null 2>&1 || true
  sleep 1
  adbs shell am start -n dev.rime.imetest/.MainActivity --ez bottom true "$@" >/dev/null \
    || fail "啟動測試靶失敗"
  sleep "$SETTLE"

  local xml="$OUT_DIR/ui-$tag.xml"
  dump_ui > "$xml" || fail "uiautomator dump 失敗(靶沒起來?)—— 見 $OUT_DIR"
  [ -s "$xml" ] || fail "uiautomator dump 是空的 —— 見 $OUT_DIR"

  adbs shell screencap -p /sdcard/rime-insets-$tag.png >/dev/null 2>&1 || true
  adbs pull /sdcard/rime-insets-$tag.png "$OUT_DIR/$tag.png" >/dev/null 2>&1 || true

  local bounds
  bounds="$(py_extract "$xml" rime_test_input_bottom)"
  [ -n "$bounds" ] || fail "在 $tag 的畫面上找不到下緣輸入框(content-desc=rime_test_input_bottom)。
測試靶太舊或沒有帶 --ez bottom true。重建:scripts/build_testapp.sh。dump 在 $xml"

  MEAS_FIELD_BOTTOM="$(printf '%s\n' "$bounds" | awk '{print $4}')"
  MEAS_INSET="$(py_extract "$xml" ime_inset)"

  local top
  top="$(ime_window_top | awk '{print $1}')"
  [ -n "$top" ] || fail "讀不到鍵盤視窗的 frame —— 鍵盤沒有出現?dump 在 $OUT_DIR/window.txt"
  MEAS_IME_TOP="$top"
  local bot
  bot="$(ime_window_top | awk '{print $2}')"
  MEAS_IME_BOTTOM="$bot"
}

# ────────────────────────────── 1. 正例 ──────────────────────────────

step "1. 宿主消費 insets 時,鍵盤不得蓋住下緣輸入框"

measure ok
info "鍵盤視窗 top=$MEAS_IME_TOP bottom=$MEAS_IME_BOTTOM"
info "下緣輸入框 bottom=$MEAS_FIELD_BOTTOM"
info "宿主收到的 ime inset=${MEAS_INSET:-<沒讀到>}"

[ -n "$MEAS_INSET" ] || fail "宿主沒有回報 ime inset。
靶沒有安裝 OnApplyWindowInsetsListener(是不是誤帶了 --ez insets false?),
或是測試靶是舊版。重建:scripts/build_testapp.sh"

if [ "$MEAS_INSET" -eq 0 ]; then
  fail "宿主收到的 ime inset 是 0 —— 系統認為鍵盤不佔任何空間,
宿主不會替鍵盤讓位,使用者看不到自己正在打的字。
鍵盤視窗實際是 [top=$MEAS_IME_TOP, bottom=$MEAS_IME_BOTTOM]。"
fi

EXPECT_INSET=$(( MEAS_IME_BOTTOM - MEAS_IME_TOP ))
DIFF=$(( MEAS_INSET - EXPECT_INSET )); [ "$DIFF" -lt 0 ] && DIFF=$(( -DIFF ))
if [ "$DIFF" -gt 2 ]; then
  fail "宿主收到的 ime inset($MEAS_INSET)與鍵盤視窗的高度($EXPECT_INSET)對不上。
差 $DIFF px。輸入法回報的佔用空間與它實際畫的範圍不一致 ——
覆寫了 onComputeInsets 而算錯,是最常見的原因。"
fi
pass "ime inset $MEAS_INSET px = 鍵盤視窗高度 $EXPECT_INSET px"

if [ "$MEAS_FIELD_BOTTOM" -gt "$MEAS_IME_TOP" ]; then
  fail "下緣輸入框的底(y=$MEAS_FIELD_BOTTOM)越過了鍵盤的頂(y=$MEAS_IME_TOP),
被蓋住 $(( MEAS_FIELD_BOTTOM - MEAS_IME_TOP )) px。畫面在 $OUT_DIR/ok.png"
fi
pass "下緣輸入框底 y=$MEAS_FIELD_BOTTOM ≤ 鍵盤頂 y=$MEAS_IME_TOP,沒有重疊"

# ─────────────────────── 2. 反向對照:檢查自己會不會紅 ───────────────────────

step "2. 反向對照:宿主不消費 insets 時,上面那兩條必須紅"

measure bad --ez insets false
info "鍵盤視窗 top=$MEAS_IME_TOP"
info "下緣輸入框 bottom=$MEAS_FIELD_BOTTOM"

if [ "$MEAS_FIELD_BOTTOM" -le "$MEAS_IME_TOP" ]; then
  fail "反向對照沒有被抓到:靶明明沒有消費 insets,下緣輸入框(bottom=$MEAS_FIELD_BOTTOM)
卻仍在鍵盤頂(y=$MEAS_IME_TOP)之上。**這代表這支腳本自己壞了** ——
它在該紅的時候不會紅,所以第 1 關那個綠不能相信。
可能原因:靶沒有真的重啟(--ez insets false 沒吃到)、鍵盤根本沒出現、
或是量錯了視窗。畫面在 $OUT_DIR/bad.png"
fi
pass "反向對照如預期被抓到(重疊 $(( MEAS_FIELD_BOTTOM - MEAS_IME_TOP )) px)"

# ───────────────────────────────── 收尾 ─────────────────────────────────

step "結果"
echo "  鍵盤沒有蓋住宿主的內容。ime inset 與鍵盤視窗高度一致。"
echo "  artifact:$OUT_DIR"
