#!/usr/bin/env bash
#
# verify_selection_digit.sh — 候選列的序號:**按下去真的選得到嗎**
#                             (規範草稿 §8.6.1.1;`core/selection-digit.tsv` 的
#                              產生器,也是它的斷言者)
#
# ── 為什麼需要這一支 ──────────────────────────────────────────────────────
#
# 候選列上的序號 `1 2 3` 只有一個用途:讓使用者按數字鍵選第 N 個。上一輪的
# 判準是「這一層有沒有 `send.keysym` 落在 1..9」,實測四份會亮的佈局裡**三份
# 是錯的**,其中一份是**破壞性**的:
#
#   cn-t9-pinyin-numrow ＋ t9_pinyin  畫 1..6,按 3 → 輸入框變成 `3⋯`,
#                                     使用者已經打好的 `MG GAM` 沒了
#   bopomofo-dachen ＋ bopomofo_tw    ㄅ=1、ㄉ=2、ˇ=3 是**大千鍵位標示**,
#                                     那些鍵送的數字被 speller 當成注音字母吃掉
#   t9-pinyin/t9                      畫 1..6,只有 `k1` 真的送得出 `1`
#
# 而守它的那一條單元測試永遠不會紅:它拿**手搓的 fixture** 問「這個手搓的層
# 有沒有數字鍵」—— 同義反覆,而且從來沒有碰過 `core/layouts/` 任何一份檔案。
#
# 「按下去選不選得到」只有真機答得出來。這一支就是那個答案的來源。
#
# ── 每一格量什麼 ──────────────────────────────────────────────────────────
#
#   引擎側  1. 打字 → 點**高亮那一格**(引擎的第 1 個)→ 記下上屏的詞 T1
#           2. 重打 → 按送得出 `1` 的那顆鍵      → 記下上屏的詞 D1
#              **D1 必須逐字等於 T1**。這就是序號 `1` 對使用者的承諾。
#           3. 重打 → 按送得出 `3` 的那顆鍵      → D3 必須是**上屏了一個詞**
#              (非空、且不等於組字中的那一串)。
#
#   ⚠ 為什麼第 3 步不比對「第 3 個候選的字」:使用者詞典會學。點過第 3 個之後
#     那個詞就升到第 1 個,下一輪按 3 拿到的是別的詞 —— 兩種順序都會讓斷言
#     自己翻面,而翻面的理由是詞頻不是缺陷。所以位置的精確比對只做第 1 個
#     (它本來就在第 1 個,再選一次不會重排),第 3 個只驗「有沒有選到東西」。
#     實測上這已經分得出全部三種失敗:被 recognizer 吃掉會變成 `3⋯`(沒上屏)、
#     被 speller 吃掉會變成多打一個注音(沒上屏)。
#
#   畫面側  4. 打字之後量**高亮塊的寬度**。高亮是實心塊(§8.6.4.3 的預設),
#              它的寬度就是那一格的量測寬,而「有沒有畫序號」恰好差
#              一個序號寬 ＋ 一段 gap(隨附主題:兩字候選 56 dp vs 65.6 dp,
#              在 420dpi 上差 25 px)。所以畫面自己說得出序號畫了沒有。
#
#   斷言    5. **畫面畫了序號 ⟺ 引擎那一側真的選得到。** 這一條就是牙:
#              把 `CandidateDensity.selectionDigitUsable` 改成永遠回 true,
#              t9 那一格會畫序號而引擎選不到 —— 這一關當場紅。
#           6. 量到的結果與 `core/selection-digit.tsv` 一致。`--bless` 則是
#              把量到的寫回去(那是這份表**唯一**該有的產生方式)。
#
# ── 用法 ──────────────────────────────────────────────────────────────────
#   RIME_SERIAL=emulator-5558 scripts/verify_selection_digit.sh --apk <apk>
#   RIME_SERIAL=emulator-5558 scripts/verify_selection_digit.sh --bless
#   ... --only cn-qwerty-numrow          只跑某一份佈局
#
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
# shellcheck source=lib/product.sh
. "$HERE/lib/product.sh"
# shellcheck source=lib/logmatch.sh
. "$HERE/lib/logmatch.sh"
# shellcheck source=lib/device.sh
. "$HERE/lib/device.sh"

SDK="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Android/Sdk}}"
ADB="$SDK/platform-tools/adb"
SERIAL=""
IME_ID="${RIME_IME_ID:-$RS_ANDROID_IME_ID}"
IME_PKG="${IME_ID%%/*}"
TARGET_PKG=dev.rime.inputmatrix
TARGET_ACT="$TARGET_PKG/.MainActivity"
THEME="${RIME_THEME:-default-light}"
TABLE="$ROOT/core/selection-digit.tsv"
OUT_DIR="$ROOT/build/verify-seldigit"
APK=""
BLESS=0
ONLY=""

while [ $# -gt 0 ]; do
  case "$1" in
    --serial) SERIAL="$2"; shift 2 ;;
    --apk)    APK="$2"; shift 2 ;;
    --out)    OUT_DIR="$2"; shift 2 ;;
    --only)   ONLY="$2"; shift 2 ;;
    --bless)  BLESS=1; shift ;;
    -h|--help) sed -n '2,/^set -uo pipefail$/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "未知參數: $1" >&2; exit 2 ;;
  esac
done

mkdir -p "$OUT_DIR"
adbs() { "$ADB" -s "$SERIAL" "$@"; }
info() { echo "[seldigit] $*" >&2; }
pass() { echo "  [PASS] $*"; }
FAILURES=0
fail() { echo "  [FAIL] $*" >&2; FAILURES=$((FAILURES + 1)); }
step() { echo; echo "── $* ──"; }

[ -x "$ADB" ] || { echo "找不到 adb:$ADB" >&2; exit 2; }
[ -f "$TABLE" ] || { echo "找不到 $TABLE" >&2; exit 2; }
python3 -c "import PIL" >/dev/null 2>&1 || { echo "python3 缺 Pillow(數像素要用)" >&2; exit 2; }
# ⛔ `--serial` 也要算「指名」。閘從前只看環境變數,於是這一行帶 `--serial`
#   就必死(RC=2,訊息說「是自動選來的」而那台正是命令列指名的)。
#   `rs_select_device` 把來源(flag / env / auto)一起記下來給閘看。
rs_select_device "$ADB" "$SERIAL" || exit 2
SERIAL="$RS_SERIAL"
adbs get-state >/dev/null 2>&1 || { echo "$SERIAL 不在線" >&2; exit 2; }
rs_assert_destructive_ok "$ADB" "$SERIAL" "pm clear $IME_PKG / $TARGET_PKG、ime set" || exit 2
AVD="$(rs_avd_name "$ADB" "$SERIAL")"
TODAY="$(date +%F)"
STAMP="${AVD:-unknown}@$TODAY"

if [ -n "$APK" ]; then
  info "安裝 $APK"
  adbs install -r -g -t "$APK" >/dev/null 2>&1 || { echo "安裝失敗" >&2; exit 2; }
fi
# ⛔ **裝完才寫 device.txt。** 這一行從前在 `install` **之前**,於是
#   `pkg_version` / `pkg_apk_sha256` / `pkg_last_update` 記的是**上一份**
#   APK —— 實測 2026-08-13 23:55 的那一輪,device.txt 寫著
#   `pkg_version=0.1.0-dev+26081314.fbb68aa`(上一個 commit)而量的是新的那一份。
#   `lib/device.sh` 檔頭自己寫著「量的是裝置上真的裝著的那一份,不是我打算裝
#   上去的那一份」—— 順序錯了,那句話就不成立。
rs_write_device_stamp "$ADB" "$SERIAL" "$OUT_DIR/device.txt" "$APK" "$IME_PKG"

WM_SIZE="$(adbs shell wm size 2>/dev/null | tr -d '\r' | sed -n 's/.*: *\([0-9]*x[0-9]*\).*/\1/p' | tail -1)"
WM_DENS="$(adbs shell wm density 2>/dev/null | tr -d '\r' | sed -n 's/.*: *\([0-9]*\).*/\1/p' | tail -1)"
SCREEN_W="${WM_SIZE%%x*}"
info "裝置 $SERIAL(avd=${AVD:-?})$WM_SIZE @${WM_DENS}dpi"

dump_ui() {
  adbs shell "uiautomator dump /sdcard/seldigit.xml >/dev/null 2>&1; cat /sdcard/seldigit.xml" 2>/dev/null | tr -d '\r'
}
field_text() {
  dump_ui | python3 -c '
import sys, xml.etree.ElementTree as ET
try: root = ET.fromstring(sys.stdin.read())
except Exception: sys.exit(0)
for n in root.iter("node"):
    if n.get("content-desc") == "rime_matrix_input":
        print((n.get("text") or "").strip()); break
'
}
# ⛔ **`field_text()` 讀的是 EditText 的 `text`,而空欄位時 uiautomator 回的是
#    hint**(這台靶上是 `type here`)。於是「打完之後輸入框是空的 —— 那幾下
#    沒有進到引擎」與「按 3 之後內容變了」這兩條斷言,在**遮罩 preedit 的
#    佈局**(九宮格的 PGM 就是)上近乎恆真:組字中 `field_text()` 印的是
#    `type here`,而它非空。方向仍然是 fail-closed(造不出假的 yes),
#    但**守門訊息與它實際守的東西不是同一件事** —— 那種守門下一次就會
#    被當成「它一直是綠的」而沒有人再看。
#
# 這一支改讀測試靶的**狀態鏡射**(`scripts/build_input_matrix_app.sh` 的
# `rime_matrix_mirror`),格式是
#
#     STATE <field> |<text>| cs=<組字起> ce=<組字迄> sel=<a>,<b> len=<n>
#
# `cs`/`ce` 是 `InputConnection.setComposingRegion` 的實際範圍 —— 它**不是**
# 畫面上的文字,hint 汙染不到它。空欄位時是 `cs=-1 ce=-1 len=0`。
mirror_state() {
  dump_ui | python3 -c '
import sys, xml.etree.ElementTree as ET
try: root = ET.fromstring(sys.stdin.read())
except Exception: sys.exit(0)
for n in root.iter("node"):
    if n.get("content-desc") == "rime_matrix_mirror":
        print((n.get("text") or "").strip()); break
'
}
# 組字區的長度(`ce - cs`),沒有組字時是 0。⚠ 讀不到鏡射時回 `-1`
# (「問不出來」與「沒有組字」必須分得開,不然又是一個恆真的斷言)。
composing_len() {
  mirror_state | python3 -c '
import re, sys
m = re.search(r"cs=(-?\d+) ce=(-?\d+)", sys.stdin.read())
if not m:
    print(-1)
else:
    cs, ce = int(m.group(1)), int(m.group(2))
    print(max(0, ce - cs) if cs >= 0 and ce >= 0 else 0)
'
}
# **已經上屏**的字數 = `len`(整個欄位)− 組字區長度。與 hint 無關。
#
# ⛔ 為什麼不是直接讀 `len`:`len` **含組字區**(qwerty 上打 `ni hao` 時
#    `cs=0 ce=6 len=6`)。拿 `len > 0` 當「選到字了」,在「按 3 被 recognizer
#    收走、組字變成 `3⋯`」那一格上也是真的 —— 又一個恆真的斷言。
#    差別只有一個:被收走時多出來的那幾個字**還在組字區裡**。
committed_len() {
  mirror_state | python3 -c '
import re, sys
m = re.search(r"cs=(-?\d+) ce=(-?\d+) sel=\S+ len=(\d+)", sys.stdin.read())
if not m:
    print(-1)
else:
    cs, ce, ln = int(m.group(1)), int(m.group(2)), int(m.group(3))
    comp = max(0, ce - cs) if cs >= 0 and ce >= 0 else 0
    print(max(0, ln - comp))
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

# ⚠ 每一次點擊之前都要重讀:打第一個字之後 IME 視窗會長高,舊座標會落在隔壁列。
#   格線區的頂端一律由**上往下**算(視窗頂端 ＋ 候選列高),不從視窗底端回推
#   —— 底端回推會少掉一段 honor_bottom_inset(實測 63 px)。
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
  GRID_TOP=$((FRAME_TOP + BAR_PX))
  BAR_MID=$(( (FRAME_TOP + GRID_TOP) / 2 ))
}

# ═════════════════ 逐列量測 ═════════════════
ROWS="$(grep -v '^[[:space:]]*#' "$TABLE" | grep -v '^[[:space:]]*$')"
[ -n "$ROWS" ] || { echo "$TABLE 裡一列資料都沒有" >&2; exit 2; }

MEASURED="$OUT_DIR/measured.tsv"
: > "$MEASURED"

# ⚠ 迴圈的輸入**先讀進陣列**,不要用 `while read ... <<< "$ROWS"`:
#   迴圈體裡的 `adb shell` 會把 stdin 吃光,於是第一列跑完之後 read 就讀不到
#   東西了 —— 症狀是「只跑了第一格然後安靜地結束」,而那正是綠燈的形狀。
#   (這一支第一次跑就踩到:5 列只量了 1 列。)
ROW_LIST=()
while IFS= read -r _line; do ROW_LIST+=("$_line"); done <<< "$ROWS"

for _row in "${ROW_LIST[@]}"; do
  IFS=$'\t' read -r LAYOUT SCHEMA WANT COMPOSE OLD_STAMP NOTE <<< "$_row"
  [ -n "${LAYOUT:-}" ] || continue
  if [ -n "$ONLY" ] && [ "$LAYOUT" != "$ONLY" ]; then
    printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$LAYOUT" "$SCHEMA" "$WANT" "$COMPOSE" "$OLD_STAMP" "${NOTE:-}" >> "$MEASURED"
    continue
  fi
  step "$LAYOUT × $SCHEMA(打 $COMPOSE)"
  SRC_LAYOUT="$ROOT/core/layouts/$LAYOUT.yaml"
  [ -f "$SRC_LAYOUT" ] || { fail "找不到 $SRC_LAYOUT"; continue; }

  # ── 裝置準備:釘住方案與佈局 ────────────────────────────────────────
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
  # ⛔ **競爭那一份也要處理。**
  #   隨附的 `cn-t9-pinyin` 自己就寫著 `auto_for_schema: ["t9_pinyin"]`,於是
  #   上面那一行做完之後,**有兩份佈局同時自動命中同一個方案**。裝置上載進去
  #   哪一份,從前是 `File.listFiles()` 說了算 —— 而它不保證順序。
  #   症狀正是這支腳本的「一次紅一次綠」:紅的那一次連 `cn-t9-pinyin-numrow`
  #   都沒有載進去,而畫面完全正常。
  #
  #   產品端這一輪已經把決勝定死了(`LayoutPriority`:使用者目錄 > 隨附,
  #   同級用 id 字典序),但那條規則解不掉這裡的平手 —— 兩份都會被寫進
  #   使用者目錄的話,贏的會是 id 較小的 `cn-t9-pinyin`,而要驗的是另一份。
  #   所以這裡把對手的自動命中清成 `[]`,讓要驗的那一份是**唯一**命中的。
  for _other in "$ROOT"/core/layouts/*.yaml; do
    _oid="$(basename "$_other" .yaml)"
    [ "$_oid" = "$LAYOUT" ] && continue
    grep -qE "^auto_for_schema:.*\"$SCHEMA\"" "$_other" || continue
    info "  $_oid 也自動命中 $SCHEMA —— 在使用者目錄裡把它的 auto_for_schema 清成 []"
    sed -e "s/^auto_for_schema:.*/auto_for_schema: []/" "$_other" \
      | adbs shell "run-as $IME_PKG sh -c 'cat > files/rime/user/layouts/$_oid.yaml'" >/dev/null 2>&1
  done

  IME_NOW=""
  for _ in $(seq 1 20); do
    adbs shell ime enable "$IME_ID" >/dev/null 2>&1
    adbs shell ime set "$IME_ID" >/dev/null 2>&1
    IME_NOW="$(adbs shell settings get secure default_input_method 2>/dev/null | tr -d '\r')"
    [ "$IME_NOW" = "$IME_ID" ] && break
    sleep 1
  done
  [ "$IME_NOW" = "$IME_ID" ] || { fail "設不成預設輸入法(現在是 ${IME_NOW:-<空>})"; continue; }

  adbs logcat -c >/dev/null 2>&1
  open_target
  for _ in $(seq 1 40); do
    log_has "READY" adbs logcat -d -s RimeRuntime:I && break
    sleep 3
  done
  sleep 3
  open_target
  ACTIVE=""
  for _ in $(seq 1 30); do
    ACTIVE="$(adbs logcat -d 2>/dev/null | tr -d '\r' | sed -n 's/.*佈局 . \([a-zA-Z0-9_-]*\).*/\1/p' | tail -1)"
    [ "$ACTIVE" = "$LAYOUT" ] && break
    sleep 2
  done
  [ "$ACTIVE" = "$LAYOUT" ] || { fail "裝置上載入的是 ${ACTIVE:-<無>},不是 $LAYOUT"; continue; }
  info "裝置確認:佈局=$ACTIVE 方案=$SCHEMA"

  python3 "$ROOT/scripts/layout_geom.py" --root "$ROOT" --layout "$LAYOUT" --theme "$THEME" \
    --screen "$WM_SIZE" --density "$WM_DENS" --json > "$OUT_DIR/$LAYOUT.keymap.json" 2>"$OUT_DIR/$LAYOUT.geom.err" \
    || { fail "座標計算失敗,見 $OUT_DIR/$LAYOUT.geom.err"; continue; }
  BAR_PX="$(python3 -c "import json,sys;print(json.load(open(sys.argv[1]))['bar_height_px'])" "$OUT_DIR/$LAYOUT.keymap.json")"

  # 送得出某個 keysym 的那顆鍵的 id。與 verify_candbar.sh 同一招:從 YAML 讀,
  # 不寫死 —— 鍵改名之後這裡會找不到而中止,不會安靜地點在別的鍵上。
  # ⚠ **只認得使用者現在人在的那一層。** `bopomofo-dachen` 的 `alpha` 層也有
  #   `e_n3` 送 `3`,但打注音時人在 `bopomofo` 層上 —— 拿別層的鍵 id 去算座標
  #   會算不出來,或更糟:算出一個落在別的鍵上的座標。
  #
  # ⚠ 而且**要真的解析 YAML**,不要 grep 一行。`b_` 是寫成一行的
  #   `{ id: "b_", label: "ㄅ", hint: "1", send: { keysym: "1" } }`,而 `tone3`
  #   是多行的 —— grep 那一版找得到前者、找不到後者,於是這一支報「這一層送不出
  #   3」而**寫下一個理由是錯的 `no`**。同一格答案,兩個不同的理由,只有一個是真的。
  key_for() {
    python3 - "$SRC_LAYOUT" "$1" "$OUT_DIR/$LAYOUT.keymap.json" <<'PYK'
import json, sys
import yaml
doc = yaml.safe_load(open(sys.argv[1], encoding='utf-8'))
want = sys.argv[2]
ids = {k['id'] for k in json.load(open(sys.argv[3]))['keys'] if k.get('id')}
for layer in doc.get('layers', []):
    for row in layer.get('rows', []):
        for k in row.get('keys', []):
            if not isinstance(k, dict):
                continue
            if k.get('id') not in ids:
                continue
            send = k.get('send')
            if isinstance(send, dict) and str(send.get('keysym')) == want:
                print(k['id'])
                raise SystemExit(0)
raise SystemExit(1)
PYK
  }
  tap_key() {
    read_frame || return 1
    local xy
    xy="$(python3 "$ROOT/scripts/layout_geom.py" --root "$ROOT" --layout "$LAYOUT" --theme "$THEME" \
          --screen "$WM_SIZE" --density "$WM_DENS" --key "$1" --grid-top "$GRID_TOP" 2>/dev/null)"
    [ -n "$xy" ] || return 1
    # shellcheck disable=SC2086
    adbs shell input tap $xy >/dev/null 2>&1
    sleep 0.8
  }
  type_compose() {
    local name kid
    for name in $COMPOSE; do
      kid="$(key_for "$name")"
      [ -n "$kid" ] || { echo "找不到送 $name 的鍵" >&2; return 1; }
      tap_key "$kid" || return 1
    done
    return 0
  }

  K1="$(key_for 1)"; K3="$(key_for 3)"
  if [ -z "$K1" ] || [ -z "$K3" ]; then
    info "這一份佈局的預設層送不出 1 或 3 —— 判準本來就會說「不畫」,不必上機量"
    printf '%s\t%s\tno\t%s\t%s\t%s\n' "$LAYOUT" "$SCHEMA" "$COMPOSE" "$STAMP" "佈局上沒有整排數字鍵" >> "$MEASURED"
    continue
  fi

  # ── 1. 高亮那一格 = 引擎的第 1 個 ────────────────────────────────
  open_target; read_frame || { fail "讀不到 frame"; continue; }
  type_compose || { fail "打不進去(點不到鍵)"; continue; }
  read_frame || { fail "讀不到 frame"; continue; }
  # ⚠ 只當除錯資訊印出來。**不要拿它當斷言** —— 空欄位時它是 hint。
  # ⚠ 只當除錯資訊。**不要拿它當斷言** —— 空欄位時它是 hint。
  COMPOSING="$(field_text)"
  CLEN="$(composing_len)"
  PRE_COMMITTED="$(committed_len)"
  [ "$CLEN" != "-1" ] || { fail "$LAYOUT:讀不到 rime_matrix_mirror —— 靶 app 太舊?先跑 scripts/build_input_matrix_app.sh"; continue; }
  # ⛔ **不可以拿「host 端有沒有組字區」當「那幾下有沒有進到引擎」。**
  #    九宮格上 host 端的組字區**本來就是空的**:PGM 代碼刻意不送給宿主
  #    (`InlinePreedit.forDisplay` 把整串代碼濾掉之後沒有東西剩下,
  #    工單 #68),所以 `cs=-1 ce=-1 len=0` 是**正常**的。
  #    這一支第一版的修法就踩到這裡,而它紅得很大聲 —— 那是對的。
  #    「進到引擎了」的證據是**候選列上有高亮候選**(下面 find_highlight.py
  #    那一段,找不到就 fail)。
  [ "$PRE_COMMITTED" -eq 0 ] || { fail "$LAYOUT:還沒選字就已經有 $PRE_COMMITTED 字上屏 —— 輸入框沒清乾淨,後面的比對不算數"; continue; }
  info "組字中:鏡射「$(mirror_state)」(組字區 $CLEN 字、已上屏 $PRE_COMMITTED 字);field_text 讀到「$COMPOSING」"
  adbs exec-out screencap -p > "$OUT_DIR/$LAYOUT-typed.png" 2>/dev/null
  read -r HX0 HY0 HX1 HY1 HX HY <<<"$(python3 "$HERE/lib/find_highlight.py" \
      "$OUT_DIR/$LAYOUT-typed.png" "$FRAME_TOP" "$GRID_TOP")"
  if [ -z "${HX0:-}" ]; then
    fail "候選列上找不到高亮候選 —— 沒有候選可驗,見 $OUT_DIR/$LAYOUT-typed.png"
    continue
  fi
  HIGHLIGHT_W=$((HX1 - HX0))
  adbs shell input tap "$HX" "$HY" >/dev/null 2>&1
  sleep 1.5
  T1="$(field_text)"
  # 「有沒有東西上屏」問鏡射(`len` − 組字區),不問 `field_text`(空欄位回 hint)。
  T1LEN="$(committed_len)"
  info "點高亮那一格 → 上屏「${T1:-<空>}」(已上屏 $T1LEN 字);高亮塊寬 ${HIGHLIGHT_W}px"

  # ── 2. 按送得出 `1` 的那顆鍵 ─────────────────────────────────────
  open_target; read_frame || { fail "讀不到 frame"; continue; }
  type_compose || { fail "第二輪打不進去"; continue; }
  tap_key "$K1" || { fail "點不到送 1 的鍵($K1)"; continue; }
  sleep 1.2
  D1="$(field_text)"
  info "按 1（鍵 $K1）→ 輸入框「${D1:-<空>}」"

  # ── 3. 按送得出 `3` 的那顆鍵 ─────────────────────────────────────
  open_target; read_frame || { fail "讀不到 frame"; continue; }
  type_compose || { fail "第三輪打不進去"; continue; }
  COMPOSING3="$(field_text)"
  CLEN3="$(composing_len)"
  C3_PRE="$(committed_len)"
  [ "$C3_PRE" -eq 0 ] || { fail "$LAYOUT:第三輪還沒按 3 就已經有 $C3_PRE 字上屏"; continue; }
  tap_key "$K3" || { fail "點不到送 3 的鍵($K3)"; continue; }
  sleep 1.2
  D3="$(field_text)"
  # ⛔ 「按 3 之後有沒有選到字」問的是**有沒有東西真的上屏**,
  #    也就是 `len − 組字區長度` 有沒有從 0 變成正數:
  #      選到字             → 組字收掉、上屏 N 字            (0 → N)
  #      被 recognizer 吃掉 → 那個 `3` 附加到組字串裡,
  #                           畫面上是 `3⋯` 而它**整段都還在組字區**(0 → 0)
  #      被 speller 吃掉    → 同上(`ㄋㄧ ㄏㄠˇ` 全在組字區)
  #    三種結局在 `field_text` 上難分(遮罩之下都是 hint 或都有字),
  #    在這個數上一刀兩斷。
  CLEN3_AFTER="$(composing_len)"
  C3_POST="$(committed_len)"
  info "按 3（鍵 $K3）→ 輸入框「${D3:-<空>}」;組字區 $CLEN3 → $CLEN3_AFTER 字、已上屏 $C3_PRE → $C3_POST 字"
  info "     按完的鏡射:$(mirror_state)"

  # ⛔ 三件事都要成立才算 `yes`(fail-closed):
  #    (a) 點高亮那一格**真的上屏了東西**(鏡射的 len > 0,不是 hint);
  #    (b) 按 `1` 上屏的詞**逐字等於**點高亮那一格上屏的詞;
  #    (c) 按 `3` 之後**組字區變短或消失,而且輸入框裡多了東西** ——
  #        被 recognizer 吃掉的那一格剛好相反:組字區**變長**(`MGGAM` →
  #        `MGGAM3`)而輸入框仍然是空的。
  ENGINE=no
  if [ "${T1LEN:-0}" -gt 0 ] && [ -n "$T1" ] && [ "$D1" = "$T1" ] \
     && [ "${C3_POST:-0}" -gt "${C3_PRE:-0}" ]; then
    ENGINE=yes
  fi
  info "引擎側的答案:$ENGINE"

  # ── 4. 畫面上到底畫了序號沒有 ────────────────────────────────────
  # 高亮是實心塊,它的寬度就是那一格的量測寬。兩種假設差一個序號寬 ＋ gap。
  CHARS="$(python3 -c "import sys;print(len(sys.argv[1]))" "${T1:-x}")"
  read -r W_NOLABEL W_LABEL <<<"$(python3 "$HERE/lib/candbar_geom.py" --root "$ROOT" \
      --theme "$THEME" --item-width "$CHARS")"
  PX_NOLABEL="$(python3 -c "print(round($W_NOLABEL * $WM_DENS / 160.0))")"
  PX_LABEL="$(python3 -c "print(round($W_LABEL * $WM_DENS / 160.0))")"
  DELTA="$(python3 -c "print(abs($PX_LABEL - $PX_NOLABEL))")"
  DRAWN=unknown
  if [ "$DELTA" -lt 12 ]; then
    info "這一格兩種假設只差 ${DELTA}px(min_width 把兩者夾成一樣),畫面分不出來 —— 這一項跳過"
  else
    DN="$(python3 -c "print(abs($HIGHLIGHT_W - $PX_NOLABEL))")"
    DL="$(python3 -c "print(abs($HIGHLIGHT_W - $PX_LABEL))")"
    if [ "$DL" -lt "$DN" ]; then DRAWN=yes; else DRAWN=no; fi
    info "高亮塊 ${HIGHLIGHT_W}px:無序號應是 ${PX_NOLABEL}px、有序號應是 ${PX_LABEL}px → 畫面畫了序號:$DRAWN"
  fi

  # ── 5. ⛔ 畫了序號 ⟺ 真的按得到 ─────────────────────────────────
  if [ "$DRAWN" = unknown ]; then
    info "畫面分不出來,這一格只留引擎側的結果"
  elif [ "$BLESS" -eq 1 ] && [ "$DRAWN" != "$ENGINE" ]; then
    # ⚠ `--bless` 這一輪跑的 APK 帶的是**還沒更新的**那份表,所以畫面本來就
    #   可能與剛量到的結果不一致。這裡只提醒,並要求重建後再跑一次斷言 ——
    #   把它當成 FAIL 會讓「產生表」這件事永遠紅一次,而永遠紅一次的守門
    #   會被當成噪音忽略掉。
    info "（--bless）畫面畫序號=$DRAWN、實測=$ENGINE。這是預期的:跑的 APK 帶的是舊表。"
    info "         重建 APK 之後再跑一次(不帶 --bless)才是真的斷言。"
  elif [ "$DRAWN" = "$ENGINE" ]; then
    pass "$LAYOUT × $SCHEMA:畫面畫序號=$DRAWN、按下去選得到=$ENGINE —— 一致"
  else
    fail "$LAYOUT × $SCHEMA:**畫面畫序號=$DRAWN,而按下去選得到=$ENGINE**。" \
         "畫了卻按不到 = 使用者按下去毀掉自己的組字;按得到卻不畫 = 白白藏起一個真功能。" \
         "圖:$OUT_DIR/$LAYOUT-typed.png"
  fi

  # ── 6. 與 core/selection-digit.tsv 對帳 ─────────────────────────
  if [ "$BLESS" -eq 1 ]; then
    printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$LAYOUT" "$SCHEMA" "$ENGINE" "$COMPOSE" "$STAMP" \
      "T1=${T1:-<空>} D1=${D1:-<空>} D3=${D3:-<空>}" >> "$MEASURED"
    info "--bless:寫下 $LAYOUT × $SCHEMA = $ENGINE($STAMP)"
  else
    printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$LAYOUT" "$SCHEMA" "$WANT" "$COMPOSE" "$OLD_STAMP" "${NOTE:-}" >> "$MEASURED"
    if [ "$ENGINE" = "$WANT" ]; then
      pass "與 core/selection-digit.tsv 一致($WANT)"
    else
      fail "core/selection-digit.tsv 說 $LAYOUT × $SCHEMA = $WANT,實測是 $ENGINE。" \
           "要更新那份表就跑 --bless(那是它唯一該有的產生方式)"
    fi
  fi
done

# ═════════════════ --bless:把量到的寫回表裡 ═════════════════
if [ "$BLESS" -eq 1 ]; then
  python3 - "$TABLE" "$MEASURED" <<'PY'
import io, sys
table, measured = sys.argv[1], sys.argv[2]
new = {}
order = []
for line in io.open(measured, encoding='utf-8'):
    c = line.rstrip('\n').split('\t')
    if len(c) < 5:
        continue
    new[(c[0], c[1])] = c
    order.append((c[0], c[1]))
out = []
seen = set()
for raw in io.open(table, encoding='utf-8'):
    line = raw.rstrip('\n')
    if not line.strip() or line.lstrip().startswith('#'):
        out.append(line)
        continue
    c = line.split('\t')
    key = (c[0].strip(), c[1].strip())
    if key in new:
        out.append('\t'.join(new[key]))
        seen.add(key)
    else:
        out.append(line)
for key in order:
    if key not in seen:
        out.append('\t'.join(new[key]))
io.open(table, 'w', encoding='utf-8').write('\n'.join(out) + '\n')
print('[seldigit] 已寫回 %s' % table)
PY
fi

echo
if [ "$FAILURES" -gt 0 ]; then
  echo "✗ $FAILURES 項沒過。artifact:$OUT_DIR"
  exit 1
fi
echo "✓ 每一格都對得上:畫面上有序號 ⟺ 按下去真的選得到第 N 個"
echo "   artifact:$OUT_DIR(含 device.txt:序號、AVD、APK sha256)"
