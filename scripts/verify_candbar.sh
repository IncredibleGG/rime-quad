#!/usr/bin/env bash
#
# verify_candbar.sh — 候選列的**畫面／行為**驗證:左端不印按鍵代碼、
#                     右端那一顆是展開鍵而不是翻頁鍵、翻得到第 2 頁而且
#                     選得下去、一列真的排得下 6 個
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
# ── 四道關 ──────────────────────────────────────────────────────────────
#   1  打完 MGGAM 之後,**第一個候選左邊沒有任何墨跡**。
#      定位不靠寫死的座標,而是先找到高亮候選(整條上唯一的飽和色),
#      再看它左邊那一段有沒有東西。有東西就是那一格組字串又冒出來了。
#
#      ⚠ 第一版是「把整條帶子裁下來 OCR,讀不到 GAM 就算過」,而它在 CI 上
#      **紅得莫名其妙**:CI 的模擬器是 1080x2400、開發機是 1440x3120,
#      同一段 OCR 前處理在兩邊讀出來的東西完全不同,CI 那次只讀到「ee」。
#      有沒有墨跡是像素等級的事實,兩邊一樣;而且「找不到高亮」本身就是
#      「這一關沒有東西可驗」的訊號,不會靜靜地變成綠燈。
#
#   2  **候選列右端那一顆是展開鍵,不是翻頁鍵。**
#      §8.6.6.4:本頁還有畫不出來的候選時,⛔ 不得提供「下一頁」——
#      按下去就是讓使用者跳過他從未看見的候選,而畫面完全正常。
#      判準是**格線區有沒有大面積改變**(面板蓋上去了):翻頁只換候選列,
#      格線區一個像素都不動。右端萬一還是翻頁鍵,這一關就會紅。
#
#   3  **翻頁搬進展開面板之後,第 2 頁仍然翻得到、選得下去。**
#      搬走一個入口最容易發生的事是「搬過去就不見了」。面板裡那顆的座標
#      從 keymap.json 算出來(面板高度 = 鍵盤高 − 最後一列 − row_spacing),
#      不寫死。選下去是為了同時驗「索引沒錯位」——`rs_select_candidate`
#      吃的是頁內索引,脫鉤時使用者點第二個會選到別的字而畫面完全正常。
#
#   4  **密度:舊版翻頁鍵的位置,現在站著一個候選。**
#      改動前右端是「翻頁 ＋ 展開」兩顆(80 dp),由右數來第二顆的中心是
#      `SCREEN_W − 1.5 × 40dp`。改動後右端只剩一顆,那個位置落在第 6 個
#      候選身上 —— 點下去必須**上屏一個字**。
#      這一點同時驗到三件事,每一件單獨都能讓它紅:右端真的只剩一顆、
#      一列真的排得下 6 個、那個位置真的點得到(min_width 沒把觸控目標弄丟)。
#
# ── 環境 vs 缺陷:這一支必須分得出來(2026-08-14 補) ────────────────────
#
#   CI 的慢車道連三輪紅(edd0db2 / c8d9b4e / 562bd43),每一輪紅在不同的一關,
#   而三輪的截圖是同一件事:**IME 視窗那一塊整片空白,鍵盤根本沒畫出來**。
#   在同一台建置機的 emulator-5558 上,同一份 APK 連跑三次全綠、逐項數字相同。
#
#   為什麼三次都被報成產品缺陷:這支腳本從前**沒有任何一道判準看得見鍵盤在不在**。
#     · `read_frame` 讀的 `Window{… InputMethod}` 紀錄在 IME 收起來之後照樣存在,
#       frame 一個位元都不差 → 它永遠成功,腳本在一片空白上照常算座標、照常點。
#     · `field_text()` 從前讀 EditText 的 `text` 屬性,而那在欄位空著時回的是
#       **hint**,靶的 hint 正好是 `type here` → 「按鍵沒進引擎」那道護欄永遠不觸發,
#       而 `$D_AFTER = $COMPOSING`(兩邊都是 hint)成立 → 印出
#       「還是組字中的『type here』—— 沒有選到任何候選」。
#   兩個洞疊起來,「鍵盤不在畫面上」就長成了「候選列第 6 格點不到」。
#
#   ⚠ 查這一條的時候順手挖出第三件事,而它讓上面第二點比看起來更嚴重:
#     **這一層組字中根本不上屏**(task #68:宿主不得顯示九宮格代碼)。所以
#     `$COMPOSING` 在**綠燈的那一輪**也是 hint,不是 preedit —— CI 綠的那一輪
#     自己印著 `打完 MGGAM,輸入框(組字中)=「type here」`。也就是說,
#     三道 `[ -n "$(field_text)" ] || exit 2「按鍵沒進引擎」` 護欄
#     **從第一天起就是恆真的**,一次都沒守到過東西。
#     它們現在改問「候選列上有沒有候選」(assert_bar_has_candidates)——
#     那才是「那幾下有沒有進引擎」的真憑據。
#
#   補法(都不是新腳本,用的是這支自己已經有的東西):
#     · `assert_ime_painted` —— 點之前先數 IME 視窗那一塊的像素。
#       實測 emulator-5558:正常 ~308‰、空白 3‰(只剩導覽列),差兩個數量級。
#     · 第 4 關點下去之前,在**那張會被當成證物的截圖上**確認候選列真的有高亮。
#     · `field_text()` 改讀測試靶的狀態鏡射,並且「讀不到」與「空的」分開回報。
#   看不到的時候它以 2 結束,第一句話是「環境:這一台的畫面與預期不符」——
#   不是綠燈,也不是產品缺陷。
#
# ── 反向驗證 ────────────────────────────────────────────────────────────
#   這一支沒有 --plant:它斷言的是 **APK 裡的行為**,植入要植在程式碼裡再重建。
#     · 把 InlinePreedit.forDisplay 改成 `return preedit`      → 第 1 關必須紅
#     · 把 CandidateDensity.rightEnd 的第二條改成永遠回 PAGER  → 第 2 關必須紅
#     · 把 core/themes 的 item.padding_h 改回 10               → 第 4 關必須紅
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
# ⛔ 裝置選擇只有這一份實作,而且**沒有預設 port**。這支腳本從前的預設是
#   `emulator-${RIME_EMU_PORT:-5554}`,而這台機器上長期有三到四台在跑 ——
#   不帶 RIME_SERIAL 就會對 5554 做 pm clear,並且量到那台上的舊 APK。
# shellcheck source=lib/device.sh
. "$HERE/lib/device.sh"
# shellcheck source=lib/ocr.sh
. "$HERE/lib/ocr.sh"

SDK="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Android/Sdk}}"
ADB="$SDK/platform-tools/adb"
SERIAL=""
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
RS_TESSERACT=""
TESSERACT=""
TESSDATA=""

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
# ⛔ `--serial` 也要算「指名」。閘從前只看環境變數,於是這一行帶 `--serial`
#   就必死(RC=2,訊息說「是自動選來的」而那台正是命令列指名的)。
#   `rs_select_device` 把來源(flag / env / auto)一起記下來給閘看。
rs_select_device "$ADB" "$SERIAL" || exit 2
SERIAL="$RS_SERIAL"
adbs get-state >/dev/null 2>&1 || { echo "$SERIAL 不在線" >&2; exit 2; }
# ⚠ Pillow 缺席必須在這裡就停:所有斷言都靠它數像素,缺了就等於沒驗
#   ——「工具缺席 → 跳過」跳過的關卡與綠燈長得一模一樣。
python3 -c "import PIL" >/dev/null 2>&1 || { echo "python3 缺 Pillow(數像素要用)" >&2; exit 2; }
# tesseract **不是**必需的:它只在第 1 關紅了之後把那一塊 OCR 出來寫進訊息,
#   讓看日誌的人知道印的是什麼。斷言本身是像素數,沒有它照樣成立、照樣會紅。
#
# ⚠ 這一段從前在**檔案作用域**(參數解析之前),於是 `--help` 也會去執行
#   `tesseract --version` —— 說明路徑不得有副作用,見 scripts/verify_script_readonly.sh。
#   `rs_find_tesseract` 走變數不走 stdout(它要 export LD_LIBRARY_PATH),
#   所以不可以寫成 `T="$(rs_find_tesseract)"`。
rs_find_tesseract 2>/dev/null || true
TESSERACT="$RS_TESSERACT"
TESSDATA="${TESSDATA_PREFIX:-}"
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
rs_assert_destructive_ok "$ADB" "$SERIAL" "pm clear $IME_PKG / $TARGET_PKG、ime set" || exit 2
# ⚠ 第 5 個參數是**裝置上真的裝著的那一份**的身分來源：不帶 --apk 時
#   `apk=` / `apk_sha256=` 兩行從前整個消失，於是這份 gate artifact 事後
#   無法覆核「腳本綠的時候量的是哪一份 APK」。見 lib/device.sh 檔頭。
rs_write_device_stamp "$ADB" "$SERIAL" "$OUT_DIR/device.txt" "$APK" "$IME_PKG"
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
#
# ⛔ **不可以讀 `rime_matrix_input` 的 `text` 屬性。** 那個屬性在欄位**空著**
#   的時候回的是 **hint**,而這支靶的 hint 正好是 `type here`
#   (`build_input_matrix_app.sh:408  edit.setHint("type here")`)。
#   實測(emulator-5558,2026-08-14):欄位空的時候那個節點是
#       class='android.widget.EditText'  text='type here'
#   而且**整棵樹上沒有 `hint` / `hintText` 屬性**(平台的 `uiautomator dump`
#   不輸出它)—— 所以「空欄位」與「真的有 type here 這四個字」在這裡分不開。
#
#   後果不是理論的,是這支腳本紅過的那一次:
#     · 第 4 關的護欄 `[ -n "$D_BEFORE" ] || …「按鍵沒進引擎」` **永遠不觸發**,
#       因為 hint 非空;
#     · 於是鍵盤根本沒畫出來的那一輪,一路走到
#       `[ "$D_AFTER" = "$COMPOSING" ]`(兩邊都是 hint「type here」),
#       印出「還是組字中的「type here」—— 沒有選到任何候選」。
#       那句話把「鍵盤不在畫面上」講成「第 6 格點不到」。
#
#   改讀測試靶自己的**狀態鏡射**(content-desc `rime_matrix_mirror`):
#       STATE <field> |<text>| cs=<組字起> ce=<組字迄> sel=<a>,<b> len=<n>
#   `|…|` 中間是 `Editable.toString()`,空欄位時就是空的,hint 汙染不到它。
#   這不是新發明:`verify_selection_digit.sh:188` 與 `verify_input_matrix.sh:346`
#   早就這樣讀了,只有這一支與 `verify_syllables.sh` 還在讀會被 hint 汙染的
#   那個屬性。
#
# ⚠ 讀不到鏡射時**不可以**回空字串 —— 那會和「欄位真的空著」混在一起。
#   回 `<讀不到鏡射>` 這個不可能與真實內容相等的哨符,呼叫端的斷言會因此紅,
#   而不是靜靜通過。
FIELD_UNREADABLE='<讀不到鏡射>'
field_text() {
  local out
  out="$(dump_ui | python3 -c '
import sys, re, xml.etree.ElementTree as ET
# ⚠ dump 壞掉要走「讀不到」那條路,**不可以** sys.exit(0) —— 那會讓
#   「uiautomator 這次沒吐出東西」長得和「欄位是空的」一模一樣。
try: root = ET.fromstring(sys.stdin.read())
except Exception: sys.exit(3)
for n in root.iter("node"):
    if n.get("content-desc") == "rime_matrix_mirror":
        # 貪婪的 .* 讓內容裡本身有 `|` 也切得對(結尾錨在 `| cs=<數>`)。
        m = re.match(r"^STATE \S+ \|(.*)\| cs=-?\d+ ", n.get("text") or "")
        if m:
            sys.stdout.write(m.group(1))
            sys.exit(0)
sys.exit(3)
')" || { printf '%s' "$FIELD_UNREADABLE"; return 0; }
  printf '%s' "$out"
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
BAR_PX="$(python3 -c "import json,sys;print(json.load(open(sys.argv[1]))['bar_height_px'])" "$OUT_DIR/keymap.json")"

# ⚠ 每一次點擊之前都要重讀:打第一個字之後 IME 視窗會長高,舊座標會落在隔壁列。
#
# ⛔ **`read_frame` 成功 ≠ 鍵盤在畫面上。** 它讀的是 `dumpsys window windows`
#   裡那筆 `Window{… InputMethod}` 的 `frame=`,而**那筆紀錄在 IME 收起來之後
#   照樣存在、照樣有一模一樣的 frame**。實測(emulator-5558,2026-08-14):
#       鍵盤在      frame_top=1600 frame_bot=2400   y1600..2400 墨跡 531870
#       按 BACK 收掉 frame_top=1600 frame_bot=2400   y1600..2400 墨跡   2828
#   兩邊的 frame 一個位元都不差。所以 `read_frame` 永遠成功,腳本會在一片
#   空白上照常算座標、照常點下去,而每一關都紅在「點不到」——
#   見下面 `assert_ime_painted` 的註解。
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
  # ⚠ **不可以寫成 `FRAME_BOT - GRID_H`。** IME 視窗的下緣是螢幕下緣,而鍵盤
  #   內容讓出了一段 `honor_bottom_inset`(手勢列)—— 從下緣往回推算出來的
  #   格線區頂端會低掉那一整段,每一次點擊都落在**下一列**上。
  #   這支腳本一直是這樣算的,而它沒有紅過:九宮格的鍵有 123 px 高,
  #   差那一段剛好還壓在同一顆鍵的下緣。底列那一排就沒這麼好運。
  #   ⚠ **不要在這裡寫死那一段有多少 px。** 這裡從前寫著「實測 66 px」,
  #   而 `verify_syllables.sh` 同一天量到的是 **63 px**(= 800 − 118 − 619)
  #   —— 同一件事在兩支腳本的註解裡有兩個數字,而兩個都不是判準
  #   (判準是由上往下算,根本不需要那個數)。
  #   由上往下算沒有這個問題:候選列緊貼視窗頂端,格線區緊貼候選列。
  GRID_TOP=$((FRAME_TOP + BAR_PX))
  BAR_MID=$(( (FRAME_TOP + GRID_TOP) / 2 ))
}

# 鍵盤**真的畫在畫面上**了沒有 —— 問的是像素,不是狀態旗標。
#
# ── 為什麼要有這一段 ──────────────────────────────────────────────────
# CI 的慢車道連三輪紅(edd0db2 / c8d9b4e / 562bd43),每一輪紅在不同的一關,
# 而三輪的截圖是同一件事:**IME 視窗那一塊整片空白**。
#   /home/lc/ciprobe-dl/562bd43/verify-candbar/4-dense.png   44 254 B
#   /home/lc/ciprobe-dl/30e8a2f/verify-candbar/4-dense.png  126 336 B(綠)
# 量出來(y1600..2400,1080x2400 @420):
#   紅  墨跡   2 828 / 864 000  =    3‰      ← 只剩系統導覽列
#   綠  墨跡 266 064 / 864 000  =  308‰
# 相差兩個數量級,中間沒有灰色地帶,所以門檻不需要調校。
#
# 而在這一段補上之前,**沒有任何一道判準看得見這件事**:
#   · `read_frame` 照樣成功(見它上面的註解:frame 在 IME 收起後仍然存在);
#   · `field_text()` 從前讀 hint,空欄位回「type here」而不是空字串,
#     於是「按鍵沒進引擎」那道護欄永遠不觸發(見 field_text 的註解);
#   · 於是第 4 關把「鍵盤不在畫面上」印成「點 x=922 沒有選到任何候選」,
#     而那句話會把下一個人送去查候選列的程式碼。
#
# ⛔ **不要改用 `dumpsys input_method` 的 `mIsInputViewShown`。**
#   `verify_input_matrix.sh:384` 與 `verify_longpress.sh:319` 等的是它,而它
#   在這台機器上**是黏住的**。實測(emulator-5558,2026-08-14,同一支探針
#   連續五個狀態):
#       IME 剛被 force-stop   mIsInputViewShown=false  墨跡 703617(桌布)
#       開靶、鍵盤上來        mIsInputViewShown=true   墨跡 531870
#       按 BACK 收掉鍵盤      mIsInputViewShown=true   墨跡   2828  ← 還是 true
#       再點欄位              mIsInputViewShown=true   墨跡   2828  ← 還是 true
#   它一旦為真就只有在 IME 行程被殺掉時才回到假 —— 拿它當「鍵盤現在在不在」
#   會**在正是要擋的那一格上放行**。同一份 dumpsys 裡真的會跟著變的是
#   `mInputShown`,但那仍然只是「系統認為它該顯示」;這一關要的是
#   「使用者看得到」,而那件事只有像素回答得了。所以這裡數像素。
#
# ⛔ 這也**不是**「等久一點」或「多重試幾次」。原本這裡是 open_target() 尾巴
#   一個無條件的 `sleep 2`;現在總預算沒有變長(仍是 open_target 的 4 + 2 秒),
#   只是把那 2 秒從「盲等」換成「等到看得見為止,看不見就指名說出來並停」。
#   等不到的時候它**不會**變綠,也不會被當成產品缺陷 —— 它以 2 結束,
#   訊息第一句就是「這一台的畫面與預期不符」。真的產品缺陷(鍵盤永遠畫不出來)
#   在這裡一樣紅,而且紅得更早、更準。
IME_PAINT_PERMILLE="${RIME_IME_PAINT_PERMILLE:-50}"   # 綠 308‰ / 紅 3‰,取中間
ime_paint_permille() {  # ime_paint_permille <png> → 千分比
  python3 "$HERE/lib/count_ink.py" "$1" 0 "$FRAME_TOP" "$SCREEN_W" "$FRAME_BOT" \
    "$OUT_DIR/paint-probe.png" \
    | awk '{ if ($2 > 0) printf "%d\n", ($1 * 1000) / $2; else print 0 }'
}
# 「鍵盤不在畫面上」這句話只寫一次,四個呼叫端共用 —— 訊息分岔的話,
# 下一個人會以為那是四種不同的病。
env_no_keyboard() {  # env_no_keyboard <這是哪一步> <千分比> <截圖>
  echo "" >&2
  echo "⚠ 環境:這一台的畫面與預期不符 —— 鍵盤沒有畫在畫面上($1)。" >&2
  echo "  IME 視窗 frame=[0,$FRAME_TOP]..[${SCREEN_W},$FRAME_BOT]," >&2
  echo "  但那一整塊只有 ${2}‰ 的墨跡(下限 $IME_PAINT_PERMILLE‰;" >&2
  echo "  這台機器正常時實測 300‰ 上下,空白時 3‰)。" >&2
  echo "  也就是說 IME 視窗**登記著**卻沒有內容 —— 這幾關要點的候選、" >&2
  echo "  展開鍵、九宮格,沒有一個在畫面上。" >&2
  echo "  ⛔ 這**不是**候選列的缺陷,也還不能證明產品有問題:繼續點下去," >&2
  echo "     每一關都會紅在「點不到」,而那些訊息會指向不相干的程式碼。" >&2
  echo "  現場:$3(整張截圖)、$OUT_DIR/paint-probe.png(量的那一塊)" >&2
  echo "  IME 行程:$(adbs shell pidof "$IME_PKG" 2>/dev/null | tr -d '\r' | sed 's/^$/<不在了>/')" >&2
  echo "  前景視窗:$(adbs shell dumpsys window 2>/dev/null | tr -d '\r' \
                     | sed -n 's/.*mCurrentFocus=\(.*\)/\1/p' | head -1)" >&2
  exit 2
}
assert_ime_painted() {  # assert_ime_painted <這是哪一步>
  local tag="$1" i pm shot="$OUT_DIR/painted-$1.png"
  # 6 圈 × 1 秒。open_target 原本的盲等就是 2 秒,這裡連同 dumpsys 的往返
  # 仍在同一個量級,不是把時間拉長來掩蓋偶發。
  for i in $(seq 1 6); do
    read_frame || { sleep 1; continue; }
    adbs exec-out screencap -p > "$shot" 2>/dev/null
    pm="$(ime_paint_permille "$shot")"
    [ "${pm:-0}" -ge "$IME_PAINT_PERMILLE" ] && { info "鍵盤畫出來了($tag:$pm‰)"; return 0; }
    sleep 1
  done
  env_no_keyboard "$tag" "${pm:-0}" "$shot"
}
# 「點完之後什麼都沒上屏」的時候問一次:鍵盤**現在**還在不在?
# 在 → 那確實是選字這條路的問題,呼叫端照常紅。
# 不在 → 那一下是點在空氣上,說成產品缺陷就是誣告。
#
# ⚠ 這不是重試,也不會讓任何斷言變綠:它只在**已經失敗**的那條路上跑一次,
#   決定那句紅字要說什麼。
assert_still_painted() {  # assert_still_painted <這是哪一步>
  local tag="$1" pm shot="$OUT_DIR/painted-$1.png"
  read_frame || return 0
  adbs exec-out screencap -p > "$shot" 2>/dev/null
  pm="$(ime_paint_permille "$shot")"
  [ "${pm:-0}" -ge "$IME_PAINT_PERMILLE" ] && return 0
  env_no_keyboard "$tag" "${pm:-0}" "$shot"
}

# 「那幾下真的進了引擎嗎」 —— 問的是候選列上有沒有候選,不是問輸入框。
#
# ⛔ **不可以用「輸入框非空」來問這件事。** 這一層的九宮格**刻意不把組字串送進
#   宿主輸入框**(task #68:宿主不得顯示九宮格代碼)。也就是說:組字中的時候
#   `cn-t9-pinyin` 的輸入框**本來就是空的**,那是正確行為,不是故障。
#   證據是 CI 綠的那一輪自己印的(run 31765500254,30e8a2f):
#       [candbar] 打完 MGGAM,輸入框(組字中)=「type here」
#       [candbar] 高亮候選 x=12..158 y=1608..1714      ← 同一時刻候選是有的
#   那句「type here」是 **hint**,不是內容 —— 舊的 `field_text()` 讀 EditText 的
#   `text` 屬性,而空欄位回的就是 hint。於是從前那三道
#   `[ -n "$(field_text)" ] || exit 2 "按鍵沒進引擎"` 護欄**每一輪都恆真**,
#   一次都沒有守到東西;而 `field_text()` 改讀狀態鏡射(回真正的空字串)之後,
#   同一道護欄會變成**每一輪都誤報**。兩個方向都錯,因為問錯了問題。
#
#   對的問題是「候選列上有沒有候選」——引擎收到那幾下才會有候選。
#   判準用第 1 關就在用的 `highlight_box`,沒有新東西。
#
# 兩種失敗分開講,因為它們該送去查的地方完全不同:
#   · 整塊鍵盤沒畫出來 → 環境(CI 連三輪紅就是這個)
#   · 鍵盤在、候選列空 → 那幾下沒進引擎(佈局還沒換過來、鍵位算錯…)
assert_bar_has_candidates() {  # assert_bar_has_candidates <png> <這是哪一步>
  local shot="$1" tag="$2" pm
  [ -n "$(highlight_box "$shot" "$FRAME_TOP" "$GRID_TOP")" ] && return 0
  pm="$(ime_paint_permille "$shot")"
  [ "${pm:-0}" -lt "$IME_PAINT_PERMILLE" ] && env_no_keyboard "$tag" "${pm:-0}" "$shot"
  echo "" >&2
  echo "鍵盤有畫出來(${pm}‰),但候選列($FRAME_TOP..$GRID_TOP)上找不到高亮候選($tag)。" >&2
  echo "  也就是那幾下沒有進到引擎 —— 佈局可能還不是 $LAYOUT,或鍵座標算錯了。" >&2
  echo "  (⚠ 這一層組字中輸入框本來就是空的,見 task #68;所以不能拿輸入框來問這件事。)" >&2
  echo "  現場:$shot" >&2
  exit 2
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

# 候選列右端**那一顆**方鍵的座標。
#
# ⚠ **不可以寫成「最右端」**(見檔頭 2026-08-13 那一節):展開鍵搬進來之後,
#   最右端是展開鍵,而「點了不上屏」對兩者都成立 —— 於是整輪點錯地方而全綠。
#   算的依據是 KeyboardView.kt 的 CANDIDATE_BAR_BUTTON_DP 與主題,不是這裡。
#
# ⚠ 2026-08-13 第二次改動:右端**最多一顆**(§8.6.6.4)。九宮格一頁 9 個、
#   一列畫得出 6 個 → 右端是**展開鍵**,翻頁移進展開面板內部。
#   所以這支腳本不再有「候選列上的翻頁鍵」這個東西可以點。
dp() { python3 -c "print(int(round($1 * $WM_DENS / 160.0)))"; }
GEOM="$HERE/lib/candbar_geom.py"
[ -f "$GEOM" ] || { echo "找不到 $GEOM" >&2; exit 2; }
# 引擎一頁幾個。`core/data/shared/default.yaml` 的 menu/page_size。
PAGE_SIZE="${RIME_PAGE_SIZE:-9}"
candbar_x() {  # candbar_x <prev|next|expand|visible> [頁次] [是不是最後一頁] [本頁幾個]
  python3 "$GEOM" --root "$ROOT" --theme "$THEME" \
    --screen "$WM_SIZE" --density "$WM_DENS" \
    --page-no "${2:-0}" --last-page "${3:-0}" --page-count "${4:-$PAGE_SIZE}" --which "$1"
}
VISIBLE="$(candbar_x visible 0 0)"
info "算出來:這台機器一列畫得出 $VISIBLE 個候選(引擎一頁 $PAGE_SIZE 個)"

# 展開鍵。本頁看不完時它必定存在;算不出來就是主題關掉了展開面板,
# 那時候整支腳本沒有東西可驗 —— 不可以當成通過。
EXPAND_X="$(candbar_x expand 0 0)" || {
  echo "算不出展開鍵的座標(主題 $THEME 關掉了 scroll/expand_button?)—— 這一關沒有東西可驗" >&2
  exit 2
}
# ⛔ 本頁還有畫不出來的候選時,候選列上**不得**有翻頁鍵。
# 這一條先問模型,再用畫面驗(第 2、4 關)。
if BAD_NEXT="$(candbar_x next 0 0 2>/dev/null)"; then
  echo "candbar_geom.py 說本頁看不完(畫得出 $VISIBLE、本頁 $PAGE_SIZE)時候選列上還有翻頁鍵" >&2
  echo "  x=$BAD_NEXT —— 那就是讓使用者跳過他從未看見的候選(§8.6.6.4 第 2 條)" >&2
  exit 2
fi
info "候選列右端(算出來的):展開 x=$EXPAND_X;本頁看不完,所以候選列上沒有翻頁鍵"

# 展開面板裡那顆翻頁鍵的座標。
#
# 面板是 `Box(align = TopStart)`,頂端貼著格線區頂端,高度 =
# panelHeightLeavingBottomRow(鍵盤高 − 最後一列 − row_spacing − padding.bottom),
# 底部那一條 `bar.height` 高的 Row 靠右放 PageArrows。
# 這幾個數全部從 keymap.json 算出來,不寫死。
# 展開面板裡那顆翻頁鍵的座標。
#
# 面板是 `Box(align = TopStart)`,頂端貼著格線區頂端,高度 =
# panelHeightLeavingBottomRow(鍵盤高 − 最後一列 − row_spacing − padding.bottom),
# 底部那一條 `bar.height` 高的 Row 靠右放 PageArrows(第 1 頁只有「›」,
# 所以它在面板的最右端)。這幾個數全部從 keymap.json 算出來,不寫死。
panel_next_xy() {
  python3 - "$OUT_DIR/keymap.json" "$GRID_TOP" "$(dp 40)" <<'PY'
import json, sys
sol = json.load(open(sys.argv[1]))
grid_top = int(sys.argv[2])
button_px = int(sys.argv[3])
scale = sol["scale"]
last_row = max(k["row"] for k in sol["keys"])
row_y = min(k["y"] for k in sol["keys"] if k["row"] == last_row)
row_spacing = int(round(sol["row_spacing_dp"] * scale))
panel_bot = grid_top + row_y - row_spacing
bar_h = sol["bar_height_px"]
print(sol["screen_px"][0] - button_px // 2, panel_bot - bar_h // 2)
PY
}

# 兩塊區域的「變了多少」。單位是千分比,回傳值由呼叫端判讀。
# 千分比而不是絕對像素數:CI 的模擬器與開發機解析度不同。
region_permille() {  # region_permille <a.png> <b.png> <y0> <y1> [out.png]
  python3 "$HERE/lib/region_changed.py" "$1" "$2" 0 "$3" "$SCREEN_W" "$4" "${5:-}" \
    | awk '{print $3}'
}
# 判讀門檻。兩邊都留了一個數量級的餘裕(實測值寫在 commit 訊息裡):
#   · 展開之後格線區實測是數百‰;面板內翻頁時格線區被面板蓋著,不看它。
#   · 候選列/面板換一頁實測是數十到數百‰。
QUIET_PERMILLE="${RIME_QUIET_PERMILLE:-20}"    # 「這一塊沒變」的上限
CHANGED_PERMILLE="${RIME_CHANGED_PERMILLE:-5}" # 「這一塊真的變了」的下限

# 高亮候選(引擎的第一個)在畫面上的位置。
#
# ⚠ **不可以用固定的 x。** 候選列左端有沒有那一格組字串會讓所有候選整條位移,
#   而寫死的座標在那時候會點在一塊按不動的文字上 —— 於是「翻頁沒用」與
#   「點錯地方」在輸出上長得一模一樣(實測踩過)。改成在那條帶子裡找**高亮**:
#   它是整條上唯一的飽和色(主題的 item.highlight_background)。
#
# ⚠ 2026-08-13:高亮的預設畫法從**實心塊**換成**格底一條 2 dp 的底線**
#   (§8.6.4.3 的 `item.highlight_style`)。飽和色的判準不變,只是命中的面積
#   從整格變成一條線 —— 回傳的 y 因此落在候選文字的底部,仍然在那一格的
#   可點範圍內。
# `x0 y0 x1 y1 cx cy`;找不到就什麼都不印。
highlight_box() {
  python3 "$HERE/lib/find_highlight.py" "$1" "$2" "$3"
}

read_frame || { echo "讀不到 IME 視窗 frame" >&2; exit 2; }
# ⚠ 第一次真的要點鍵盤之前。c8d9b4e 那一輪紅在這裡:`1-typed.png` 44 026 B、
#   整片空白,於是第 1 關報「候選列上找不到高亮候選」而其實是鍵盤不在。
assert_ime_painted "第一輪打字前"

# ═══════════ 第一輪:不翻頁,直接選第一個 ═══════════
step "第一輪:第 1 頁的第一個候選"
type_nihao || { echo "點不到九宮格的鍵" >&2; exit 2; }
read_frame || { echo "讀不到 frame" >&2; exit 2; }
adbs exec-out screencap -p > "$OUT_DIR/1-typed.png" 2>/dev/null
BAR_TOP="$FRAME_TOP"; BAR_BOT="$GRID_TOP"

# ⚠ 先確認那五下真的進了引擎。打不進去的話後面每一關都會紅,而紅的理由
#   會指向產品 —— 實際上是「鍵盤還沒換成九宮格就開始打」(CI 上踩過:
#   IME 剛 attach 時畫的是 qwerty,拿九宮格的座標打上去會打出 jddyj)。
#
# ⚠ 2026-08-14:問法從「輸入框非空」改成「候選列上有候選」。理由見
#   `assert_bar_has_candidates` 的註解 —— 這一層組字中輸入框**本來就是空的**
#   (task #68),舊問法讀到的非空字串是 hint,那道護欄從來沒有守到過東西。
#   c8d9b4e 那一輪 CI 紅在這裡,而它的 1-typed.png 是 44 026 B 的一片空白。
assert_bar_has_candidates "$OUT_DIR/1-typed.png" "第一輪打完MGGAM之後"

COMPOSING="$(field_text)"
if [ "$COMPOSING" = "$FIELD_UNREADABLE" ]; then
  echo "打完之後讀不到測試靶的狀態鏡射 —— 後面每一關都要拿它比對,問不出來就不能往下走。" >&2
  echo "  多半是 uiautomator dump 失敗或靶 app 太舊(先跑 scripts/build_input_matrix_app.sh)。" >&2
  exit 2
fi
# ⚠ 這裡**期待它是空的**:組字中不上屏(task #68)。留著這個變數是給下面幾關
#   當「還沒有東西上屏」的基準用 —— 它不再是「preedit 的內容」。
info "打完 MGGAM,已上屏的內容=「$COMPOSING」(組字中不上屏,所以空的才對)"

# ── 第 1 關:第一個候選左邊不准有東西 ────────────────────────────────
step "1. 候選列左端不印按鍵代碼"
read -r HX0 HY0 HX1 HY1 HX HY <<<"$(highlight_box "$OUT_DIR/1-typed.png" "$BAR_TOP" "$BAR_BOT")"
# 上面 assert_bar_has_candidates 已經確認過高亮存在(而且是同一張圖、同一個
# 判準),所以這裡讀不到就是 BAR_TOP/BAR_BOT 這一組邊界自己算錯了 —— 指名說出來。
if [ -z "${HX0:-}" ]; then
  echo "候選列邊界算錯了:整條 IME 視窗($FRAME_TOP..$GRID_TOP)上找得到高亮," >&2
  echo "  但用第 1 關的帶子($BAR_TOP..$BAR_BOT)去找卻找不到。見 $OUT_DIR/1-typed.png" >&2
  exit 2
fi
info "高亮候選 x=$HX0..$HX1 y=$HY0..$HY1"
# ⚠ y 範圍取**整條帶子**,不是高亮那幾列。底線式的高亮只有幾個像素高,
#   拿它當 y 範圍的話這一關會退化成「那幾列上沒有東西」—— 而組字串畫在
#   帶子的正中間,根本不在那幾列上。那會是一條永遠綠的檢查。
read -r INK TOTAL <<<"$(python3 "$HERE/lib/count_ink.py" "$OUT_DIR/1-typed.png" \
                        0 "$((BAR_TOP + 2))" "$((HX0 - 4))" "$((BAR_BOT - 2))" \
                        "$OUT_DIR/left-of-first.png")"
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

# ═══════════ 第 2 關:右端那一顆是**展開鍵**,不是翻頁鍵 ═══════════
#
# §8.6.6.4:本頁還有畫不出來的候選(畫得出 $VISIBLE、本頁 $PAGE_SIZE)時,
# 右端**不得**是翻頁鍵 —— 按下去就是跳過使用者從未看見的候選。
#
# ⚠ 「點了不上屏」對翻頁鍵與展開鍵**都成立**,分不出兩者(這支腳本 2026-08-13
#   就是栽在這裡)。分得出來的是畫面:
#     · 翻頁鍵  只換候選列的內容,鍵盤格線區一個像素都不動
#     · 展開鍵  把一片面板蓋在格線區上,那一塊大面積改變
#   所以這一關要求格線區**真的變了**。倒過來看:右端萬一還是翻頁鍵,
#   格線區不會動,這一關就會紅 —— 這正是它守的東西。
step "2. 候選列右端那一顆是展開鍵(而不是「跳過沒看過的候選」的翻頁鍵)"
open_target
# ⚠ 原本這裡是一個無條件的 `sleep 2`。換成「等到鍵盤真的畫出來」——
#   時間預算沒變,但看不到的時候會指名說出來而不是往下點。
assert_ime_painted "第2關開靶後"
read_frame || { echo "讀不到 frame" >&2; exit 2; }
type_nihao || { echo "點不到九宮格的鍵" >&2; exit 2; }
read_frame || { echo "讀不到 frame" >&2; exit 2; }
E_BEFORE="$(field_text)"
adbs exec-out screencap -p > "$OUT_DIR/2-before-expand.png" 2>/dev/null
# ⚠ 從前這裡是 `[ -n "$(field_text)" ] || exit 2 "按鍵沒進引擎"` —— 恆真的護欄
#   (讀到的非空字串是 hint)。改問候選列,而且問的是剛拍下來的那張證物。
assert_bar_has_candidates "$OUT_DIR/2-before-expand.png" "第二輪打完MGGAM之後"
BAR_TOP2="$FRAME_TOP"; BAR_BOT2="$GRID_TOP"
adbs shell input tap "$EXPAND_X" "$BAR_MID" >/dev/null 2>&1
sleep 1
E_AFTER="$(field_text)"
adbs exec-out screencap -p > "$OUT_DIR/2-expanded.png" 2>/dev/null

if [ "$E_AFTER" != "$E_BEFORE" ]; then
  fail "點算出來的右端那一顆(x=$EXPAND_X),輸入框從「$E_BEFORE」變成「$E_AFTER」—— " \
       "那裡是候選,不是控制鍵。座標算錯了。"
else
  pass "點了右端那一顆(x=$EXPAND_X),沒有任何東西上屏"
fi

E_GRID="$(region_permille "$OUT_DIR/2-before-expand.png" "$OUT_DIR/2-expanded.png" \
          "$GRID_TOP" "$FRAME_BOT" "$OUT_DIR/2-grid-diff.png")"
info "點下去之後鍵盤格線區變了 ${E_GRID}‰"
if [ "${E_GRID:-0}" -le "$QUIET_PERMILLE" ]; then
  fail "點下去之後格線區只變了 ${E_GRID}‰(下限 $QUIET_PERMILLE‰)—— 面板沒有打開。" \
       "那一顆是**翻頁鍵**(翻頁只換候選列,格線區不動),而本頁還有 " \
       "$((PAGE_SIZE - VISIBLE)) 個畫不出來的候選:按下去就是跳過使用者沒看過的字。" \
       "圖:$OUT_DIR/2-grid-diff.png"
else
  pass "格線區變了 ${E_GRID}‰ —— 展開面板真的蓋上去了,那一顆是展開鍵"
fi

# ═══════════ 第 3 關:翻頁在面板裡,而且翻得到、選得下去 ═══════════
#
# 翻頁鍵從候選列搬進展開面板(§8.6.6.4:面板裡使用者才真的看完了本頁)。
# 搬過去之後它**還在不在、按不按得動**,只有這一關驗得到。
#
# 為什麼還要「選下去」:`rs_select_candidate` 吃的是**頁內索引**,翻頁之後
# 畫面與索引一旦脫鉤,使用者點第二個會選到別的字 —— 而畫面完全正常。
# 上屏的字是唯一能同時驗到「翻頁」與「索引沒錯位」的東西。
# 也刻意**不寫死**第 2 頁應該是哪幾個詞:那會隨詞庫漂移,漂了之後紅的是腳本。
step "3. 翻頁移進展開面板之後,第 2 頁翻得到、選得下去"
read -r PN_X PN_Y <<<"$(panel_next_xy)"
info "展開面板裡那顆下一頁鍵(算出來的):x=$PN_X y=$PN_Y"
adbs shell input tap "$PN_X" "$PN_Y" >/dev/null 2>&1
sleep 1.2
P_AFTER="$(field_text)"
adbs exec-out screencap -p > "$OUT_DIR/3-panel-page2.png" 2>/dev/null
if [ "$P_AFTER" != "$E_BEFORE" ]; then
  fail "點面板裡的翻頁鍵,輸入框從「$E_BEFORE」變成「$P_AFTER」—— 那裡是候選不是翻頁鍵"
else
  pass "點了面板裡的翻頁鍵,沒有任何東西上屏"
fi
PANEL_MOVED="$(region_permille "$OUT_DIR/2-expanded.png" "$OUT_DIR/3-panel-page2.png" \
               "$GRID_TOP" "$FRAME_BOT" "$OUT_DIR/3-panel-diff.png")"
info "翻頁之後面板內容變了 ${PANEL_MOVED}‰"
if [ "${PANEL_MOVED:-0}" -lt "$CHANGED_PERMILLE" ]; then
  fail "點下去之後面板一點都沒變(${PANEL_MOVED}‰,下限 $CHANGED_PERMILLE‰)—— " \
       "翻頁鍵搬進面板之後按不動了。圖:$OUT_DIR/3-panel-diff.png"
else
  pass "面板真的換了一頁(${PANEL_MOVED}‰)"
fi

# 面板裡的第一個候選:同樣找高亮,範圍改成**面板那一塊**。
#
# ⚠ 下界不可以用 $FRAME_BOT。面板是浮層,**底列的鍵仍然露出來** ——
#   而底列的 Enter 鍵是 `style: action`,底色就是重點色。掃到螢幕底部的話,
#   飽和色的聯集會從面板裡那條高亮底線一路跨到 Enter 鍵,中心落在兩者之間的
#   死區:點下去什麼都不會發生,而輸出看起來像「翻頁之後選不出東西」。
#   (實測踩過一次,症狀正是這樣。)
PANEL_BOT=$((PN_Y - $(dp 22)))
read -r _ _ _ _ PX PY <<<"$(highlight_box "$OUT_DIR/3-panel-page2.png" "$GRID_TOP" "$PANEL_BOT")"
if [ -z "${PX:-}" ]; then
  fail "翻頁之後面板上沒有高亮候選 —— 翻到了一頁不存在的地方"
  PX="$PN_X"; PY="$PN_Y"
fi
adbs shell input tap "$PX" "$PY" >/dev/null 2>&1
sleep 1.5
T2="$(field_text)"
info "第 2 頁選第一個 → 上屏「${T2:-<空>}」"

if [ "$T1" = "$FIELD_UNREADABLE" ] || [ "$T2" = "$FIELD_UNREADABLE" ]; then
  fail "讀不到測試靶的狀態鏡射(第 1 頁「$T1」/ 第 2 頁「$T2」)—— 有沒有上屏無從得知。" \
       "不是產品的結論:多半是 uiautomator dump 失敗,或靶 app 太舊。"
elif [ -z "$T1" ]; then
  # 點完之後什麼都沒上屏 → 先問「鍵盤還在不在」,不在就是環境(它會 exit 2)。
  assert_still_painted "第1頁選完之後"
  fail "第一輪就沒有上屏 —— 選字這條路本身壞了,後面比不出東西(鍵盤仍在畫面上)"
elif [ -z "$T2" ]; then
  # ⚠ 從前這裡下面還有一支 `[ "$T2" = "$COMPOSING" ]`「還在組字中」。
  #   `field_text()` 改讀狀態鏡射之後那一支**恆假**:這一層組字中不上屏
  #   (task #68),`$COMPOSING` 就是空字串,已經被這一支收掉了。
  #   留著它只會是一條永遠走不到、卻看起來像在守的死碼。
  assert_still_painted "第2頁選完之後"
  fail "翻頁之後選不出東西(上屏是空的)—— 那一下沒有選到任何候選(鍵盤仍在畫面上)"
elif [ "$T1" = "$T2" ]; then
  fail "翻到第 2 頁再選第一個,拿到的還是「$T1」—— 翻頁鍵不存在或按不動,使用者永遠停在第 1 頁"
else
  pass "第 2 頁的第一個候選是「$T2」,與第 1 頁的「$T1」不同"
fi

# ═══════════ 第 4 關:密度 —— 舊翻頁鍵那個位置現在站著一個候選 ═══════════
#
# 這一關把「一列排得下幾個」變成**摸得到**的事實。
#
# 改動前,候選列右端是 翻頁 ＋ 展開兩顆(80 dp),而由右數來第二顆(翻頁鍵)
# 的中心是 `SCREEN_W − 1.5 × 40dp`。改動後右端只剩一顆,那個位置落在
# **第 6 個候選**身上 —— 點下去必須上屏一個字。
#
# 它同時驗到三件事,而且每一件單獨都能讓這一關紅:
#   · 右端真的只剩一顆(還是兩顆的話,那一點仍是翻頁鍵,不上屏)
#   · 一列真的排得下 6 個(只排得下 3 個的話,那一點是空白,不上屏)
#   · 那個位置點得到(min_width / padding 沒把觸控目標弄丟)
step "4. 密度:舊版翻頁鍵的位置,現在站著第 $VISIBLE 個候選"
if [ "${VISIBLE:-0}" -lt 6 ]; then
  fail "算出來一列只畫得出 $VISIBLE 個(這台機器 $WM_SIZE @${WM_DENS}dpi 應該 ≥ 6)"
else
  pass "算出來一列畫得出 $VISIBLE 個"
fi
open_target
# ⚠ 562bd43 那一輪 CI 紅在這裡:`4-dense.png` 44 254 B、整片空白。原本這一行
#   是無條件的 `sleep 2`,於是空白照樣往下走。
assert_ime_painted "第4關開靶後"
read_frame || { echo "讀不到 frame" >&2; exit 2; }
type_nihao || { echo "點不到九宮格的鍵" >&2; exit 2; }
read_frame || { echo "讀不到 frame" >&2; exit 2; }
D_BEFORE="$(field_text)"
adbs exec-out screencap -p > "$OUT_DIR/4-dense.png" 2>/dev/null
OLD_NEXT_X=$(( SCREEN_W - $(dp 60) ))   # 舊版右端第二顆(翻頁鍵)的中心
info "舊版翻頁鍵的中心 x=$OLD_NEXT_X;現在右端只有展開鍵(x=$EXPAND_X)"

# ⛔ **點之前先確認要點的東西真的在那裡** —— 而且是在**這一張、等一下會被
#   當成證物貼進失敗訊息的截圖**上確認,不是在別的時刻確認。
#   (從前這裡是 `[ -n "$D_BEFORE" ] || exit 2 "按鍵沒進引擎"`,而那道護欄
#    讀到的非空字串是 hint,一次都沒有守到過東西。)
#
#   拿 CI 自己的產物實跑過(2026-08-14,scripts/lib/find_highlight.py 原樣):
#     30e8a2f/verify-candbar/4-dense.png(綠)→ 12 1608 158 1714 85 1661,307‰
#     562bd43/verify-candbar/4-dense.png(紅)→ <什麼都沒有>,3‰
#     c8d/verify-candbar/1-typed.png    (紅)→ <什麼都沒有>,3‰
#   也就是說:這一段補上之後,CI 那兩輪會停在這裡並說「鍵盤不在畫面上」,
#   而不是走到下面印出「點 x=922 沒有選到任何候選」。
assert_bar_has_candidates "$OUT_DIR/4-dense.png" "第四輪打完MGGAM之後"

adbs shell input tap "$OLD_NEXT_X" "$BAR_MID" >/dev/null 2>&1
sleep 1.5
D_AFTER="$(field_text)"
adbs exec-out screencap -p > "$OUT_DIR/4-after-tap.png" 2>/dev/null
# ⛔ 這裡的斷言曾經只有「變了沒有」(`[ "$D_AFTER" = "$D_BEFORE" ]`),
#   而實跑拿到過 **`[PASS] 上屏了「」`** —— `field_text()` 回空字串時
#   「變了」成立,於是「什麼都沒發生」被判成通過。
#
#   `field_text()` 把三件事混成同一個空字串:欄位真的空著、欄位顯示的是
#   hint(text 屬性是空的)、`uiautomator dump` 這一次失敗(它會 `sys.exit(0)`)。
#   三種裡沒有一種是「上屏了一個候選」。
#
#   修法是把**第 3 關已經有的那三道防線**照抄過來(不發明新的):
#     空的 → 讀不到,不是上屏;等於組字串 → 還在組字,沒選到;沒變 → 沒發生。
#
#   ⚠ 2026-08-14:`field_text()` 改讀狀態鏡射之後,這三道防線塌成兩道,而且
#   意思更準了:
#     · 「讀不到」有自己的哨符 `$FIELD_UNREADABLE`,不再混進空字串;
#     · 「還在組字」與「什麼都沒上屏」在這一層**是同一件事** —— 組字中不上屏
#       (task #68),所以 `$COMPOSING`、`$D_BEFORE` 都是空的。從前那兩支之所以
#       看起來不同,只是因為前者比的是 hint「type here」。
#   所以留兩支:讀不到 / 沒變。不留一支永遠走不到的死碼冒充防線。
if [ "$D_AFTER" = "$FIELD_UNREADABLE" ]; then
  fail "點 x=$OLD_NEXT_X 之後**讀不到測試靶的狀態鏡射** —— 這一下有沒有上屏無從得知。" \
       "不是產品的結論:多半是 uiautomator dump 這次失敗,或靶 app 太舊(先跑" \
       "scripts/build_input_matrix_app.sh)。圖:$OUT_DIR/4-after-tap.png"
elif [ "$D_AFTER" = "$D_BEFORE" ]; then
  # 點完之後什麼都沒上屏 → 先問「鍵盤還在不在」,不在就是環境(它會 exit 2)。
  assert_still_painted "第4關點完之後"
  fail "點 x=$OLD_NEXT_X(舊版翻頁鍵的位置)什麼都沒上屏(前後都是「$D_BEFORE」)—— " \
       "那裡不是候選。可能是右端仍然保留兩顆鍵,也可能是一列根本排不到那麼多個。" \
       "(這一句有前提:點之前已經量過鍵盤畫出來了(≥$IME_PAINT_PERMILLE‰)、" \
       "而且那一列上有高亮候選 —— 所以不是「畫面上沒東西」。)" \
       "圖:$OUT_DIR/4-after-tap.png"
else
  pass "點 x=$OLD_NEXT_X 上屏了「$D_AFTER」—— 那 40 dp 現在是候選,不是按鍵"
fi

echo
if [ "$FAILURES" -gt 0 ]; then
  echo "✗ $FAILURES 項沒過。artifact:$OUT_DIR"
  exit 1
fi
echo "✓ 候選列左端不印按鍵代碼;右端那一顆是展開鍵而不是「跳過沒看過的候選」的翻頁鍵;"
echo "  翻頁搬進面板之後第 2 頁仍然翻得到、選得下去;舊翻頁鍵的位置現在站著一個候選"
echo "   artifact:$OUT_DIR"
