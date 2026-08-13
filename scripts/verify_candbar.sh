#!/usr/bin/env bash
#
# verify_candbar.sh — 候選列的**畫面／行為**驗證:
#                     左端不印按鍵代碼、翻得到第 2 頁而且選得下去
#
# ── 為什麼需要這支 ──────────────────────────────────────────────────────
#
#   兩條都是使用者拿真機打九宮格時看到的:
#
#     A-2  候選列最左邊那一格印著 `MG GAM` —— 那是雙編碼方案的按鍵代碼。
#          使用者鍵面上按的是 `mno`/`ghi`,畫面上冒出來的卻是大寫代號,
#          而且會讓人以為「我打出來的是這個」。
#     A-4  「候選詞只有 5 個,下一頁就沒了」。引擎那一側是好的
#          (rs_change_page 實測翻得到第 4 頁),缺的是**畫面上的入口**。
#
#   純函式那一半在 CandidateBarModelTest。這一支補的是那組測試驗不到的:
#   **東西有沒有真的畫出來、點不點得到、點下去有沒有用**。
#   這個專案已經吃過七次「單元測試綠、使用者打開看不到」的虧。
#
# ── 兩道關 ──────────────────────────────────────────────────────────────
#   1  打完 MGGAM 之後,**第一個候選左邊沒有任何墨跡**。
#      定位不靠寫死的座標,而是先找到高亮候選那一塊(整條上唯一的飽和色塊),
#      再看它左邊那一段有沒有東西。有東西就是那一格組字串又冒出來了。
#
#      ⚠ 第一版是「把整條帶子裁下來 OCR,讀不到 GAM 就算過」,而它在 CI 上
#      **紅得莫名其妙**:CI 的模擬器是 1080x2400、開發機是 1440x3120,
#      同一段 OCR 前處理在兩邊讀出來的東西完全不同,CI 那次只讀到「ee」。
#      有沒有墨跡是像素等級的事實,兩邊一樣;而且「找不到高亮塊」本身就是
#      「這一關沒有東西可驗」的訊號,不會靜靜地變成綠燈。
#   2  **翻得到第 2 頁,而且第 2 頁的候選選得下去**:
#        第一輪  打 MGGAM → 直接選高亮那一個 → 記下上屏的詞 T1
#        第二輪  打 MGGAM → 點下一頁 →(a)輸入框的內容不可以變
#                         → 選高亮那一個 →(b)上屏的詞 T2 != T1
#
#      ⚠ **(a) 那一條是這一關的重點,不是附帶的。** 第一版只有 (b),而把
#      翻頁鍵整個拿掉之後它**照樣是綠的** —— 因為候選列最右端本來就有候選,
#      點下去上屏的是「米高」,一樣 != 你好。也就是說那一版驗的是
#      「右邊有東西可以點」,不是「有翻頁鍵」。
#      翻頁鍵與候選的差別在於:**點翻頁鍵不會上屏任何東西**。所以先比一次
#      輸入框:點了之後內容變了 = 那裡是候選不是翻頁鍵。
#
# ── 2026-08-13:同一個假綠燈,倒過來發生了一次 ────────────────────────────
#
#   `cand` 那一批把**展開鍵**加進候選列右端之後,這支腳本紅了一條,而紅的
#   位置很有意思:
#
#       [PASS] 點了最右端那一點,沒有任何東西上屏(翻頁鍵的行為)   ← 正控照樣綠
#       [FAIL] 點了下一頁再選第一個,拿到的還是「你好」
#
#   成因:`NEXT_X=$(( SCREEN_W - $(dp 20) ))` —— 「最右邊那顆 40dp 方塊的
#   中心」。展開鍵搬進去之後,**那一點現在是展開鍵**。整輪點的都是展開鍵,
#   而上面那道正控**照樣綠**,因為展開鍵也不上屏任何東西。
#   鐵證:artifact 的檔名叫 `2-page2.png`,畫面上卻是第 1 頁的展開面板。
#
#   也就是說「點了不上屏」同時符合翻頁鍵**與**展開鍵 —— 檔頭警告過的那個
#   假綠燈,倒過來又發生了一次。兩件事一起改:
#
#   甲、**座標用算的**,不再是「最右端」。scripts/lib/candbar_geom.py 從
#       KeyboardView.kt 的 `CANDIDATE_BAR_BUTTON_DP` 與主題的
#       `page_indicator` / `expand_button` 算出每一顆的中心 —— 連「第 1 頁
#       不畫上一頁」「只有一頁時整組不畫」都算得出來。讀不到常數就報錯,
#       不給預設值。
#   乙、**正控要分得出翻頁鍵與展開鍵**。分得出來的不是輸入框(兩者都不上屏),
#       是畫面:
#         · 翻頁鍵  只換候選列的內容,**鍵盤格線區一個像素都不動**
#         · 展開鍵  把一片面板蓋在格線區上,那一塊大面積改變
#       所以第 2 關現在問三件事:輸入框沒變(不是候選)、格線區沒變
#       (不是展開鍵)、**候選列真的變了**(不是點在一塊按不動的地方)。
#       第 3 關再拿展開鍵當**反向對照**:同一個判準,格線區必須真的變 ——
#       否則「格線區沒變」這句話只是因為那個判準什麼都量不到。
#
#      為什麼還要「選下去」:`rs_select_candidate` 吃的是**頁內索引**,
#      翻頁之後畫面與索引一旦脫鉤,使用者點第二個會選到別的字 —— 而畫面
#      完全正常。上屏的字是唯一能同時驗到「翻頁」與「索引沒錯位」的東西。
#
#      也刻意**不寫死**第 2 頁應該是哪幾個詞:那會隨詞庫與使用者詞典漂移,
#      漂了之後紅的是這支腳本,不是產品。
#
# ── 反向驗證 ────────────────────────────────────────────────────────────
#   這一支沒有 --plant:它斷言的是 **APK 裡的行為**,植入要植在程式碼裡再重建。
#     · 把 InlinePreedit.forDisplay 改成 `return preedit`  → 第 1 關必須紅
#     · 把 Pager.state 的 show 改成 false                   → 第 2 關必須紅
#   實測結果寫在 commit 訊息裡。
#
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
# shellcheck source=lib/product.sh
. "$HERE/lib/product.sh"
# ⚠ 不可以寫成 `logcat | grep -q`:pipefail 之下命中會變成 141(上游 SIGPIPE),
#   於是「有命中」被判成「沒命中」。
. "$HERE/lib/logmatch.sh"

SDK="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Android/Sdk}}"
ADB="$SDK/platform-tools/adb"
SERIAL="${RIME_SERIAL:-${ANDROID_SERIAL:-emulator-${RIME_EMU_PORT:-5554}}}"
IME_ID="${RIME_IME_ID:-$RS_ANDROID_IME_ID}"
IME_PKG="${IME_ID%%/*}"
TARGET_PKG=dev.rime.inputmatrix
TARGET_ACT="$TARGET_PKG/.MainActivity"
THEME="${RIME_THEME:-default-light}"
SCHEMA=t9_pinyin
LAYOUT="${RIME_LAYOUT:-cn-t9-pinyin}"
OUT_DIR="$ROOT/build/verify-candbar"
APK=""
# 抗鋸齒會在候選塊邊緣留下幾個灰像素,而裁切邊界就貼著它。留一點餘裕,
# 但遠小於一個字元的墨跡量(`MG GAM` 在 1080 寬的螢幕上是數千個)。
INK_TOLERANCE="${RIME_INK_TOLERANCE:-40}"
TESSERACT="${RIME_TESSERACT:-$(command -v tesseract || true)}"
TESSDATA="${TESSDATA_PREFIX:-}"

while [ $# -gt 0 ]; do
  case "$1" in
    --serial) SERIAL="$2"; shift 2 ;;
    --apk)    APK="$2"; shift 2 ;;
    --layout) LAYOUT="$2"; shift 2 ;;
    --out)    OUT_DIR="$2"; shift 2 ;;
    -h|--help) sed -n '2,/^set -uo pipefail$/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "未知參數: $1" >&2; exit 2 ;;
  esac
done

mkdir -p "$OUT_DIR"
adbs() { "$ADB" -s "$SERIAL" "$@"; }
info() { echo "[candbar] $*" >&2; }
pass() { echo "  [PASS] $*"; }
FAILURES=0
fail() { echo "  [FAIL] $*" >&2; FAILURES=$((FAILURES + 1)); }
step() { echo; echo "── $* ──"; }

[ -x "$ADB" ] || { echo "找不到 adb:$ADB" >&2; exit 2; }
adbs get-state >/dev/null 2>&1 || { echo "$SERIAL 不在線" >&2; exit 2; }
# ⚠ Pillow 缺席必須在這裡就停:所有斷言都靠它數像素,缺了就等於沒驗
#   ——「工具缺席 → 跳過」跳過的關卡與綠燈長得一模一樣。
python3 -c "import PIL" >/dev/null 2>&1 || { echo "python3 缺 Pillow(數像素要用)" >&2; exit 2; }
# tesseract **不是**必需的:它只在第 1 關紅了之後把那一塊 OCR 出來寫進訊息,
#   讓看日誌的人知道印的是什麼。斷言本身是像素數,沒有它照樣成立、照樣會紅。
[ -n "$TESSERACT" ] && [ -x "$TESSERACT" ] || info "沒有 tesseract,失敗訊息裡不會有 OCR 佐證(斷言不受影響)"

SRC_LAYOUT="$ROOT/core/layouts/$LAYOUT.yaml"
[ -f "$SRC_LAYOUT" ] || { echo "找不到 $SRC_LAYOUT" >&2; exit 2; }

# ═══════════════════════ 裝置準備 ═══════════════════════
if [ -n "$APK" ]; then
  info "安裝 $APK"
  adbs install -r -g -t "$APK" >/dev/null 2>&1 || { echo "安裝失敗" >&2; exit 2; }
fi

# ⚠ pm clear 會把我們踢出「已啟用的輸入法」,系統當場退回別的鍵盤;
#   而 force-stop 待測 IME 會把套件打進 stopped 狀態、再也回不來。
adbs shell pm clear "$IME_PKG" >/dev/null 2>&1
adbs shell pm clear "$TARGET_PKG" >/dev/null 2>&1
sleep 3

adbs shell "run-as $IME_PKG mkdir -p shared_prefs" >/dev/null 2>&1
printf '%s' "<?xml version='1.0' encoding='utf-8' standalone='yes' ?>
<map>
    <string name=\"pending_schema\">$SCHEMA</string>
</map>
" | adbs shell "run-as $IME_PKG sh -c 'cat > shared_prefs/$RS_ANDROID_PREFS_STORE.xml'" >/dev/null 2>&1

adbs shell "run-as $IME_PKG mkdir -p files/rime/user/layouts" >/dev/null 2>&1
sed -e "s/^for_schema:.*/for_schema: [\"$SCHEMA\"]/" \
    -e "s/^auto_for_schema:.*/auto_for_schema: [\"$SCHEMA\"]/" \
    -e "s/^deprecated:.*/deprecated: false/" "$SRC_LAYOUT" \
  | adbs shell "run-as $IME_PKG sh -c 'cat > files/rime/user/layouts/$LAYOUT.yaml'" >/dev/null 2>&1

IME_NOW=""
for _ in $(seq 1 20); do
  adbs shell ime enable "$IME_ID" >/dev/null 2>&1
  adbs shell ime set "$IME_ID" >/dev/null 2>&1
  IME_NOW="$(adbs shell settings get secure default_input_method 2>/dev/null | tr -d '\r')"
  [ "$IME_NOW" = "$IME_ID" ] && break
  sleep 1
done
[ "$IME_NOW" = "$IME_ID" ] || { echo "設不成預設輸入法(現在是 ${IME_NOW:-<空>})" >&2; exit 2; }

dump_ui() {
  adbs shell "uiautomator dump /sdcard/candbar.xml >/dev/null 2>&1; cat /sdcard/candbar.xml" 2>/dev/null | tr -d '\r'
}

# 待測輸入框裡目前的文字。IME 視窗不在 uiautomator 的樹上（實測),
# 但**宿主 app 的輸入框在** —— 上屏的結果讀得到,那正是這一關要的東西。
field_text() {
  dump_ui | python3 -c '
import sys, re, xml.etree.ElementTree as ET
try: root = ET.fromstring(sys.stdin.read())
except Exception: sys.exit(0)
for n in root.iter("node"):
    if n.get("content-desc") == "rime_matrix_input":
        print((n.get("text") or "").strip())
        break
'
}

field_xy() {
  dump_ui | python3 -c '
import sys, re, xml.etree.ElementTree as ET
try: root = ET.fromstring(sys.stdin.read())
except Exception: sys.exit(0)
for n in root.iter("node"):
    if n.get("content-desc") == "rime_matrix_input":
        m = re.match(r"\[(\d+),(\d+)\]\[(\d+),(\d+)\]", n.get("bounds",""))
        if m:
            a,b,c,d = map(int, m.groups()); print((a+c)//2, (b+d)//2)
        break
'
}

open_target() {
  adbs shell am force-stop "$TARGET_PKG" >/dev/null 2>&1
  adbs shell am start -n "$TARGET_ACT" --es field text >/dev/null 2>&1
  sleep 4
  local xy; xy="$(field_xy)"
  # shellcheck disable=SC2086
  [ -n "$xy" ] && adbs shell input tap $xy >/dev/null 2>&1
  sleep 2
}

adbs logcat -c >/dev/null 2>&1
open_target
for _ in $(seq 1 40); do
  log_has "READY" adbs logcat -d -s RimeRuntime:I && break
  sleep 3
done
sleep 3
open_target

# ⚠ 等佈局**真的**換過來再往下走。IME 剛 attach 時畫的是預設的 qwerty,
#   要等 librime 回報方案之後才換成九宮格 —— 中間那幾秒拿九宮格的座標打上去,
#   會打在 qwerty 上(實測打出 `jddyj`),而症狀看起來像產品壞了。
#   CI 的機器比開發機慢,這個空窗更長。
ACTIVE=""
for _ in $(seq 1 30); do
  ACTIVE="$(adbs logcat -d 2>/dev/null | tr -d '\r' | sed -n 's/.*佈局 . \([a-zA-Z0-9_-]*\).*/\1/p' | tail -1)"
  [ "$ACTIVE" = "$LAYOUT" ] && break
  sleep 2
done
[ "$ACTIVE" = "$LAYOUT" ] || { echo "裝置上載入的是 ${ACTIVE:-<無>},不是 $LAYOUT" >&2; exit 2; }
info "裝置確認:佈局=$ACTIVE"
sleep 2

# ═══════════════════════ 幾何 ═══════════════════════
WM_SIZE="$(adbs shell wm size 2>/dev/null | tr -d '\r' | sed -n 's/.*: *\([0-9]*x[0-9]*\).*/\1/p' | tail -1)"
WM_DENS="$(adbs shell wm density 2>/dev/null | tr -d '\r' | sed -n 's/.*: *\([0-9]*\).*/\1/p' | tail -1)"
SCREEN_W="${WM_SIZE%%x*}"
python3 "$ROOT/scripts/layout_geom.py" --root "$ROOT" --layout "$LAYOUT" --theme "$THEME" \
  --screen "$WM_SIZE" --density "$WM_DENS" --json > "$OUT_DIR/keymap.json" 2>"$OUT_DIR/geom.err" \
  || { echo "座標計算失敗,見 $OUT_DIR/geom.err" >&2; exit 2; }
GRID_H="$(python3 -c "import json,sys;print(json.load(open(sys.argv[1]))['grid_height_px'])" "$OUT_DIR/keymap.json")"

# ⚠ 每一次點擊之前都要重讀:打第一個字之後 IME 視窗會長高,舊座標會落在隔壁列。
read_frame() {
  read -r FRAME_TOP FRAME_BOT < <(adbs shell dumpsys window windows 2>/dev/null | tr -d '\r' | python3 -c '
import sys, re
txt = sys.stdin.read()
m = re.search(r"Window\{[^}]*InputMethod\}:(.*?)(?=\n  Window \#|\Z)", txt, re.S)
if not m: sys.exit(0)
f = re.search(r"\bframe=\[(\d+),(\d+)\]\[(\d+),(\d+)\]", m.group(1))
if f: print(f.group(2), f.group(4))
')
  [ -n "${FRAME_BOT:-}" ] || return 1
  GRID_TOP=$((FRAME_BOT - GRID_H))
  BAR_MID=$(( (FRAME_TOP + GRID_TOP) / 2 ))
}

tap_key() {
  read_frame || return 1
  local kid="$1" xy
  xy="$(python3 "$ROOT/scripts/layout_geom.py" --root "$ROOT" --layout "$LAYOUT" --theme "$THEME" \
        --screen "$WM_SIZE" --density "$WM_DENS" --key "$kid" --grid-top "$GRID_TOP" 2>/dev/null)"
  [ -n "$xy" ] || return 1
  # shellcheck disable=SC2086
  adbs shell input tap $xy >/dev/null 2>&1
  sleep 0.8
}

KEY_M="$(grep -m1 -oE '\{ *id: *"[^"]+".*keysym: *"M" *\}' "$SRC_LAYOUT" | sed -n 's/.*id: *"\([^"]*\)".*/\1/p')"
KEY_G="$(grep -m1 -oE '\{ *id: *"[^"]+".*keysym: *"G" *\}' "$SRC_LAYOUT" | sed -n 's/.*id: *"\([^"]*\)".*/\1/p')"
KEY_A="$(grep -m1 -oE '\{ *id: *"[^"]+".*keysym: *"A" *\}' "$SRC_LAYOUT" | sed -n 's/.*id: *"\([^"]*\)".*/\1/p')"
[ -n "$KEY_M" ] && [ -n "$KEY_G" ] && [ -n "$KEY_A" ] || { echo "找不到送 M/G/A 的鍵" >&2; exit 2; }

type_nihao() {
  local k
  for k in "$KEY_M" "$KEY_G" "$KEY_G" "$KEY_A" "$KEY_M"; do
    tap_key "$k" || return 1
  done
  return 0
}

# 候選列右端那幾顆方鍵的座標。
#
# ⚠ **不可以寫成「最右端」**(見檔頭 2026-08-13 那一節):展開鍵搬進來之後,
#   最右端是展開鍵,而「點了不上屏」對兩者都成立 —— 於是整輪點錯地方而全綠。
#   算的依據是 KeyboardView.kt 的 CANDIDATE_BAR_BUTTON_DP 與主題,不是這裡。
dp() { python3 -c "print(int(round($1 * $WM_DENS / 160.0)))"; }
GEOM="$HERE/lib/candbar_geom.py"
[ -f "$GEOM" ] || { echo "找不到 $GEOM" >&2; exit 2; }
candbar_x() {  # candbar_x <prev|next|expand> [頁次] [是不是最後一頁]
  python3 "$GEOM" --root "$ROOT" --theme "$THEME" \
    --screen "$WM_SIZE" --density "$WM_DENS" \
    --page-no "${2:-0}" --last-page "${3:-0}" --which "$1"
}
# 第 1 頁(page_no=0、不是最後一頁)的下一頁鍵。算不出來就停 —— 那代表主題
# 關掉了翻頁指示器,這一關沒有東西可驗,不可以當成通過。
NEXT_X="$(candbar_x next 0 0)" || {
  echo "算不出翻頁鍵的座標(主題 $THEME 關掉了 page_indicator?)—— 這一關沒有東西可驗" >&2
  exit 2
}
# 展開鍵。主題沒有它就是空字串,第 3 關會說明並跳過(那是主題的選擇,不是缺陷)。
EXPAND_X="$(candbar_x expand 0 0 2>/dev/null || true)"
info "候選列右端(算出來的):下一頁 x=$NEXT_X;展開 x=${EXPAND_X:-<這個主題沒有>}"
[ "$NEXT_X" != "${EXPAND_X:-}" ] || {
  echo "算出來的下一頁鍵與展開鍵是同一點 —— candbar_geom.py 壞了" >&2
  exit 2
}

# 兩塊區域的「變了多少」。單位是千分比,回傳值由呼叫端判讀。
# 千分比而不是絕對像素數:CI 的模擬器與開發機解析度不同。
region_permille() {  # region_permille <a.png> <b.png> <y0> <y1> [out.png]
  python3 "$HERE/lib/region_changed.py" "$1" "$2" 0 "$3" "$SCREEN_W" "$4" "${5:-}" \
    | awk '{print $3}'
}
# 判讀門檻。兩邊都留了一個數量級的餘裕(實測值寫在 commit 訊息裡):
#   · 翻頁之後格線區實測 0‰;展開之後實測是數百‰。
#   · 候選列換一頁實測是數十到數百‰。
QUIET_PERMILLE="${RIME_QUIET_PERMILLE:-20}"    # 「這一塊沒變」的上限
CHANGED_PERMILLE="${RIME_CHANGED_PERMILLE:-5}" # 「這一塊真的變了」的下限

# 高亮候選(引擎的第一個)在畫面上的中心。
#
# ⚠ **不可以用固定的 x。** 候選列左端有沒有那一格組字串會讓所有候選整條位移,
#   而寫死的座標在那時候會點在一塊按不動的文字上 —— 於是「翻頁沒用」與
#   「點錯地方」在輸出上長得一模一樣(實測踩過)。改成在那條帶子裡找**高亮塊**:
#   它是整條上唯一的飽和色塊(主題的 item.highlight_background)。
# `x0 y0 x1 y1 cx cy`;找不到就什麼都不印。
highlight_box() {
  python3 "$HERE/lib/find_highlight.py" "$1" "$2" "$3"
}

read_frame || { echo "讀不到 IME 視窗 frame" >&2; exit 2; }

# ═══════════ 第一輪:不翻頁,直接選第一個 ═══════════
step "第一輪:第 1 頁的第一個候選"
type_nihao || { echo "點不到九宮格的鍵" >&2; exit 2; }
read_frame || { echo "讀不到 frame" >&2; exit 2; }
adbs exec-out screencap -p > "$OUT_DIR/1-typed.png" 2>/dev/null
BAR_TOP="$FRAME_TOP"; BAR_BOT="$GRID_TOP"

# ⚠ 先確認那五下真的進了引擎。打不進去的話後面每一關都會紅,而紅的理由
#   會指向產品 —— 實際上是「鍵盤還沒換成九宮格就開始打」(CI 上踩過:
#   IME 剛 attach 時畫的是 qwerty,拿九宮格的座標打上去會打出 jddyj)。
COMPOSING="$(field_text)"
[ -n "$COMPOSING" ] || {
  echo "打完之後輸入框是空的 —— 那五下沒有進到引擎(鍵盤可能還不是 $LAYOUT)" >&2
  exit 2
}
info "打完 MGGAM,輸入框(組字中)=「$COMPOSING」"

# ── 第 1 關:第一個候選左邊不准有東西 ────────────────────────────────
step "1. 候選列左端不印按鍵代碼"
read -r HX0 HY0 HX1 HY1 HX HY <<<"$(highlight_box "$OUT_DIR/1-typed.png" "$BAR_TOP" "$BAR_BOT")"
if [ -z "${HX0:-}" ]; then
  # 找不到高亮塊 = 候選列上沒有候選。這不是「通過」,是這一關沒有東西可驗。
  echo "候選列上找不到高亮候選(帶子 $BAR_TOP..$BAR_BOT)—— 沒有候選可驗,見 $OUT_DIR/1-typed.png" >&2
  exit 2
fi
info "高亮候選 x=$HX0..$HX1 y=$HY0..$HY1"
read -r INK TOTAL <<<"$(python3 "$HERE/lib/count_ink.py" "$OUT_DIR/1-typed.png" \
                        0 "$HY0" "$((HX0 - 4))" "$HY1" "$OUT_DIR/left-of-first.png")"
info "第一個候選左邊:墨跡 $INK / $TOTAL 像素"
if [ "${INK:-0}" -gt "$INK_TOLERANCE" ]; then
  EV=""
  [ -n "$TESSERACT" ] && [ -x "$TESSERACT" ] &&
    EV="$("$TESSERACT" "$OUT_DIR/left-of-first.png" stdout --psm 7 2>/dev/null | tr -d '\r' | tr '\n' ' ')"
  fail "第一個候選左邊有東西($INK 個墨跡像素,OCR:「${EV:-讀不出來}」)—— " \
       "那一格又在印按鍵代碼了。圖:$OUT_DIR/left-of-first.png"
else
  pass "第一個候選左邊乾淨($INK/$TOTAL 個墨跡像素)"
fi

# ── 選第一個,記下上屏的詞 ──────────────────────────────────────────
adbs shell input tap "$HX" "$HY" >/dev/null 2>&1
sleep 1.5
T1="$(field_text)"
info "第 1 頁選第一個 → 上屏「${T1:-<空>}」"

# ═══════════ 第二輪:翻一頁再選第一個 ═══════════
step "2. 翻得到第 2 頁,而且第 2 頁選得下去"
open_target
sleep 2
read_frame || { echo "讀不到 frame" >&2; exit 2; }
type_nihao || { echo "點不到九宮格的鍵" >&2; exit 2; }
read_frame || { echo "讀不到 frame" >&2; exit 2; }
[ -n "$(field_text)" ] || { echo "第二輪打完之後輸入框是空的,按鍵沒進引擎" >&2; exit 2; }
# ⚠ 點下去之前先記下輸入框**與畫面**。三件事一起看才分得出那一點是什麼:
#   · 輸入框變了      → 那是**候選**(實測:拿掉翻頁鍵之後那一點打在第 4 個
#                        候選上,上屏「米高」,而「T2 != T1」照樣成立)
#   · 格線區變了      → 那是**展開鍵**(實測:cand 併進來之後整輪點的都是它,
#                        而「點了不上屏」這道正控照樣綠)
#   · 候選列沒變      → 那裡根本按不動,點在一塊死區上
F_BEFORE="$(field_text)"
adbs exec-out screencap -p > "$OUT_DIR/2-before-page.png" 2>/dev/null
BAR_TOP2="$FRAME_TOP"; BAR_BOT2="$GRID_TOP"
adbs shell input tap "$NEXT_X" "$BAR_MID" >/dev/null 2>&1
sleep 1
F_AFTER="$(field_text)"
adbs exec-out screencap -p > "$OUT_DIR/2-page2.png" 2>/dev/null
if [ "$F_AFTER" != "$F_BEFORE" ]; then
  fail "點算出來的翻頁鍵(x=$NEXT_X),輸入框從「$F_BEFORE」變成「$F_AFTER」—— " \
       "那裡是候選,不是翻頁鍵。使用者永遠停在第 1 頁。"
else
  pass "點了翻頁鍵(x=$NEXT_X),沒有任何東西上屏"
fi

GRID_MOVED="$(region_permille "$OUT_DIR/2-before-page.png" "$OUT_DIR/2-page2.png" \
              "$GRID_TOP" "$FRAME_BOT" "$OUT_DIR/2-grid-diff.png")"
info "翻頁之後鍵盤格線區變了 ${GRID_MOVED}‰"
if [ "${GRID_MOVED:-0}" -gt "$QUIET_PERMILLE" ]; then
  fail "點下去之後鍵盤格線區變了 ${GRID_MOVED}‰(上限 $QUIET_PERMILLE‰)—— " \
       "那是**展開鍵**的行為,不是翻頁鍵:一片面板蓋在格線上了。" \
       "翻頁鍵只換候選列。圖:$OUT_DIR/2-grid-diff.png"
else
  pass "格線區沒動(${GRID_MOVED}‰)—— 不是展開鍵"
fi

BAR_MOVED="$(region_permille "$OUT_DIR/2-before-page.png" "$OUT_DIR/2-page2.png" \
             "$BAR_TOP2" "$BAR_BOT2" "$OUT_DIR/2-bar-diff.png")"
info "翻頁之後候選列變了 ${BAR_MOVED}‰"
if [ "${BAR_MOVED:-0}" -lt "$CHANGED_PERMILLE" ]; then
  fail "點下去之後候選列一點都沒變(${BAR_MOVED}‰,下限 $CHANGED_PERMILLE‰)—— " \
       "那一點按不動。圖:$OUT_DIR/2-bar-diff.png"
else
  pass "候選列真的換了一頁(${BAR_MOVED}‰)"
fi
read -r _ _ _ _ HX2 HY2 <<<"$(highlight_box "$OUT_DIR/2-page2.png" "$FRAME_TOP" "$GRID_TOP")"
if [ -z "${HX2:-}" ]; then
  fail "翻頁之後候選列上沒有高亮候選 —— 翻到了一頁不存在的地方"
  HX2="$NEXT_X"; HY2="$BAR_MID"
fi
adbs shell input tap "$HX2" "$HY2" >/dev/null 2>&1
sleep 1.5
T2="$(field_text)"
info "第 2 頁選第一個 → 上屏「${T2:-<空>}」"

if [ -z "$T1" ]; then
  fail "第一輪就沒有上屏 —— 選字這條路本身壞了,後面比不出東西"
elif [ -z "$T2" ]; then
  fail "翻頁之後選不出東西(上屏是空的)"
elif [ "$T2" = "$COMPOSING" ]; then
  # 輸入框在組字中本來就顯示 preedit(`MG GAM`)。拿它當「上屏的詞」會讓
  # 「根本沒選到字」看起來像「選到了別的字」—— 實測踩過。
  fail "翻頁之後那一下沒有選到任何候選(輸入框還是組字中的「$T2」)"
elif [ "$T1" = "$T2" ]; then
  fail "點了下一頁再選第一個,拿到的還是「$T1」—— 翻頁鍵不存在或按不動,使用者永遠停在第 1 頁"
else
  pass "第 2 頁的第一個候選是「$T2」,與第 1 頁的「$T1」不同"
fi

# ═══════════ 第三輪:反向對照 —— 展開鍵必須讓格線區真的變 ═══════════
#
# 上面那條「格線區沒動 → 不是展開鍵」只有在**格線區真的量得到變化**時才
# 有意義。量不到的話它永遠是綠的,而那正是這一支腳本這一次栽的跟頭的
# 一般形式:一個什麼都分不出來的判準,長得跟通過一模一樣。
#
# 所以拿展開鍵跑同一個判準,要求它**紅的那一邊**成立。
step "3. 反向對照:展開鍵會讓格線區大幅改變(所以第 2 關那條分得出兩者)"
if [ -z "$EXPAND_X" ]; then
  echo "  主題 $THEME 沒有展開鍵 —— 這一條沒有對照組可跑。" >&2
  echo "  ⚠ 那也代表第 2 關「格線區沒動」這一條在這個主題下沒有被反向驗過。" >&2
else
  open_target
  sleep 2
  read_frame || { echo "讀不到 frame" >&2; exit 2; }
  type_nihao || { echo "點不到九宮格的鍵" >&2; exit 2; }
  read_frame || { echo "讀不到 frame" >&2; exit 2; }
  [ -n "$(field_text)" ] || { echo "第三輪打完之後輸入框是空的,按鍵沒進引擎" >&2; exit 2; }
  adbs exec-out screencap -p > "$OUT_DIR/3-before-expand.png" 2>/dev/null
  E_BEFORE="$(field_text)"
  adbs shell input tap "$EXPAND_X" "$BAR_MID" >/dev/null 2>&1
  sleep 1
  E_AFTER="$(field_text)"
  adbs exec-out screencap -p > "$OUT_DIR/3-expanded.png" 2>/dev/null
  E_GRID="$(region_permille "$OUT_DIR/3-before-expand.png" "$OUT_DIR/3-expanded.png" \
            "$GRID_TOP" "$FRAME_BOT" "$OUT_DIR/3-grid-diff.png")"
  info "展開之後鍵盤格線區變了 ${E_GRID}‰(翻頁那一次是 ${GRID_MOVED}‰)"
  if [ "${E_GRID:-0}" -le "$QUIET_PERMILLE" ]; then
    fail "點展開鍵(x=$EXPAND_X)之後格線區只變了 ${E_GRID}‰ —— " \
         "那條判準什麼都分不出來,第 2 關的「格線區沒動」因此是一句空話。" \
         "圖:$OUT_DIR/3-grid-diff.png"
  elif [ "${GRID_MOVED:-0}" -gt "$QUIET_PERMILLE" ]; then
    # 兩顆都讓格線區大幅改變 = 它們在畫面上是同一件事,最可能的解釋是
    # NEXT_X 根本就落在展開鍵上(2026-08-13 那次就是這樣)。
    fail "翻頁鍵那一下讓格線區變了 ${GRID_MOVED}‰,展開鍵是 ${E_GRID}‰ —— " \
         "兩者在畫面上分不出來。x=$NEXT_X 很可能就是展開鍵,請看 candbar_geom.py 算的東西。"
  else
    pass "展開鍵讓格線區變了 ${E_GRID}‰,翻頁鍵是 ${GRID_MOVED}‰ —— 這條判準分得出兩者"
  fi
  # 順帶把「兩者都不上屏」這件事寫在輸出裡:那正是舊正控分不出來的原因。
  if [ "$E_AFTER" = "$E_BEFORE" ]; then
    info "(展開鍵同樣不上屏任何東西 —— 這就是舊的那道正控分不出兩者的原因)"
  else
    fail "點展開鍵竟然上屏了「$E_AFTER」—— 那裡不是展開鍵,座標算錯了"
  fi
fi

echo
if [ "$FAILURES" -gt 0 ]; then
  echo "✗ $FAILURES 項沒過。artifact:$OUT_DIR"
  exit 1
fi
echo "✓ 候選列左端不印按鍵代碼;翻頁鍵(算出來的座標)真的是翻頁鍵而不是展開鍵,"
echo "  第 2 頁翻得到、選得下去"
echo "   artifact:$OUT_DIR"
