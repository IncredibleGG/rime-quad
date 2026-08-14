#!/usr/bin/env bash
#
# verify_layout.sh — 從佈局 YAML 自動算出鍵座標，並逐鍵實測任意一份佈局
#
# 為什麼要有這支
# ─────────────────────────────────────────────────────────────────────────────
# 先前驗證九宮格時，鍵的座標是照 docs/theme-format.md §9.3 的佈版演算法用
# Python **手算**的（算出 row0=2362, row1=2558）。算對了，但那是碰運氣的手工活：
#   · 佈局一改就失效；
#   · core/layouts/ 現在有十幾份，手算不可行；
#   · 換一台幾何不同的裝置就要從頭再算一次。
#
# verify_input_matrix.sh 走的是另一條路 —— 沿著一欄由下往上逐點戳、看打出什麼字
# 來反推排距。那條路不必知道佈局，但一次校準約一分鐘，而且**只認得出「四排等距」
# 這一種形狀**：九宮格的 5 欄、注音的 11 欄與不等 weight 的列、cn-symbols 的
# 7 欄，它一個都量不準。
#
# 這支把座標變成**算出來的**：scripts/layout_geom.py 讀主題 yaml + 佈局 yaml，
# 實作 §8.8.0 的高度模型與 §9.3 的佈版演算法，吐出每顆鍵的中心座標。
# 零校準、零寫死、佈局改了自動跟上。
#
# 另外兩個坑，這支一併堵掉：
#
#   1. **rime session 會跨 app 重啟存活**。上一輪留下的組字會混進這一輪，
#      preedit 出現沒打過的字元，驗證截圖是髒的。所以本腳本一律從
#      `pm clear` 開始 —— 「已知狀態」不是「大概沒問題的狀態」。
#
#   2. **鍵盤畫出來不等於能打字**。只看 `mIsInputViewShown=true` 就開跑，
#      整輪會在 rs_init/部署還沒完成的視窗期跑完：rs_process_key 回 false，
#      每一鍵走 fallbackKey 直接上屏，畫面上「字都有進去」，其實引擎是死的。
#      所以這裡等的是**應用層的就緒訊號** `RimeRuntime: phase → READY`，
#      再加一道組字區（cs >= 0）的確認。
#
# 用法
#   ./scripts/verify_layout.sh --layout qwerty --schema luna_pinyin_tw \
#       --keys n,i,h,a,o --expect 'n,ni,ni h,ni ha,ni hao'
#
#   ./scripts/verify_layout.sh --layout cn-t9-pinyin --schema t9_pinyin \
#       --theme cn-compact-light --keys k_mno,k_ghi,space --expect '-,-,你'
#
#   ⚠ 上面這個例子原本寫的是 `--keys k_mno,k_ghi --expect '-,你好'`，那是錯的：
#     兩顆鍵之後畫面上是**組字中**的 `MG` 加一排候選（你 米 迷 擬 尼），
#     還沒有上屏，所以編輯框裡不可能是「你好」。要有字上屏就得再選一次候選
#     （九宮格上是空白鍵）。照著錯的例子跑會得到一個 DIFF，
#     然後開始懷疑佈局 —— 但佈局是對的。
#
# 選項
#   --layout <id>     要驗的佈局（core/layouts/<id>.yaml）
#   --layer <id>      要驗的層（預設用佈局的 default_layer）
#   --schema <id>     要釘住的 librime 方案 id
#   --keys <csv>      要依序點的**鍵 id**（§9.6 的 key.id），逗號分隔
#   --expect <csv>    每一步之後編輯框應有的內容；'-' = 該步不檢查
#   --theme <id>      主題（預設 default-light，座標會跟著主題的間距/高度變）
#   --field <id>      測試靶的欄位型別（預設 text）
#   --serial <s>      裝置（預設 $RIME_SERIAL；沒指定而且不只一台在線就中止）
#   --no-install      不重新安裝 APK（沿用裝置上已有的）
#   --no-clear        不做 pm clear（除錯用；正常不要關）
#   --no-composing-check  不檢查組字區（驗純上屏的佈局，如 numeric-symbol）
#   --keep            結束後不關閉測試靶
#   --out <dir>       artifact 目錄（預設 build/layoutverify/<layout>）
#
# 離開碼：0 全過；1 有鍵沒過；2 環境/前置條件失敗。

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
# 產品識別碼的唯一來源,見 scripts/lib/product.env。
# shellcheck source=lib/product.sh
. "$SCRIPT_DIR/lib/product.sh"
# ⚠ 就緒判斷不可以寫成 `logcat | grep -q`:pipefail 之下命中會變成 141
#   (grep -q 一命中就結束 → 上游 SIGPIPE),於是「有命中」被判成「沒命中」。
#   改用 lib/logmatch.sh 的 log_has / log_matches —— 它們先收進變數再用內建比對。
. "$SCRIPT_DIR/lib/logmatch.sh"
# ⛔ 裝置選擇的唯一入口。沒有預設 port —— 這台機器上長期有三到四台在跑,
#   而 `adb devices` 以 port 升冪列出,「預設 5554」與「抓第一台」都會
#   落在同一台**別人的**機器上,然後 pm clear 它。
# shellcheck source=lib/device.sh
. "$SCRIPT_DIR/lib/device.sh"

ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Android/Sdk}}"
ADB="$ANDROID_SDK_ROOT/platform-tools/adb"
SERIAL=""

IME_PKG="$RS_ANDROID_APP_ID"
IME_ID="${RIME_IME_ID:-$RS_ANDROID_IME_ID}"
TARGET_PKG="dev.rime.inputmatrix"
TARGET_ACT="$TARGET_PKG/.MainActivity"
TAG="IMEMATRIX"

LAYOUT=""
LAYER=""
SCHEMA=""
KEYS=""
EXPECT_CSV=""
THEME="default-light"
FIELD="text"
DO_INSTALL=1
DO_CLEAR=1
CHECK_COMPOSING=1
KEEP=0
OUT_DIR=""

while [ $# -gt 0 ]; do
  case "$1" in
    --layout) LAYOUT="$2"; shift 2 ;;
    --layer)  LAYER="$2"; shift 2 ;;
    --schema) SCHEMA="$2"; shift 2 ;;
    --keys)   KEYS="$2"; shift 2 ;;
    --expect) EXPECT_CSV="$2"; shift 2 ;;
    --theme)  THEME="$2"; shift 2 ;;
    --field)  FIELD="$2"; shift 2 ;;
    --serial) SERIAL="$2"; shift 2 ;;
    --out)    OUT_DIR="$2"; shift 2 ;;
    --no-install) DO_INSTALL=0; shift ;;
    --no-clear)   DO_CLEAR=0; shift ;;
    --no-composing-check) CHECK_COMPOSING=0; shift ;;
    --keep) KEEP=1; shift ;;
    -h|--help) sed -n '2,64p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "未知選項：$1" >&2; exit 2 ;;
  esac
done

[ -n "$LAYOUT" ] || { echo "缺 --layout" >&2; exit 2; }
[ -n "$KEYS" ]   || { echo "缺 --keys" >&2; exit 2; }

# ⚠ 這裡以前只強制 --layout 與 --keys。**沒有 --expect 一樣跑得完,而且全綠** ——
#   每一步都判 SKIP,SKIP 不進 FAILURES,結尾照樣印「✓ 全部 N 鍵通過」。
#   再加上 --no-composing-check 可以把唯一剩下的斷言關掉,於是
#   「點了 N 下、一個字都沒有比對過」與「N 鍵全部正確」在輸出上一模一樣。
#   所以:--expect 必填,而且**至少要有一個格子不是 `-`**。
[ -n "$EXPECT_CSV" ] || {
  echo "缺 --expect。沒有預期值就沒有斷言,那不是驗證,是點一遍。" >&2
  echo "  想跳過某幾步的比對,那幾格寫 '-';但不能整條都是 '-'。" >&2
  exit 2
}
_N_KEYS=0; _N_EXPECT=0; _N_REAL=0
IFS=',' read -r -a _KA <<< "$KEYS"
IFS=',' read -r -a _EA <<< "$EXPECT_CSV"
_N_KEYS="${#_KA[@]}"; _N_EXPECT="${#_EA[@]}"
for _e in ${_EA[@]+"${_EA[@]}"}; do
  [ "$_e" = "-" ] || _N_REAL=$((_N_REAL + 1))
done
if [ "$_N_EXPECT" -ne "$_N_KEYS" ]; then
  # 少寫幾格 = 後面那幾鍵靜靜地不比對。要跳過就明寫 '-',不要用「沒寫」表示。
  echo "--keys 有 $_N_KEYS 個,--expect 只有 $_N_EXPECT 個。" >&2
  echo "  數目必須相同 —— 「沒寫」與「刻意不比對」要分得出來,寫 '-' 表示不比對。" >&2
  exit 2
fi
if [ "$_N_REAL" -eq 0 ]; then
  echo "--expect 整條都是 '-',一鍵都不會比對到。這樣的綠燈沒有意義。" >&2
  exit 2
fi
if [ "$CHECK_COMPOSING" -eq 0 ] && [ "$_N_REAL" -eq 0 ]; then
  echo "--no-composing-check 加上零個實際比對 = 零斷言。拒絕。" >&2
  exit 2
fi
OUT_DIR="${OUT_DIR:-$PROJECT_ROOT/build/layoutverify/$LAYOUT}"
mkdir -p "$OUT_DIR"

# ⛔ `--serial` 也要算「指名」。閘從前只看環境變數,於是這一行帶 `--serial`
#   就必死(RC=2,訊息說「是自動選來的」而那台正是命令列指名的)。
#   `rs_select_device` 把來源(flag / env / auto)一起記下來給閘看。
rs_select_device "$ADB" "$SERIAL" || exit 2
SERIAL="$RS_SERIAL"
adbs() { "$ADB" -s "$SERIAL" "$@"; }
GEOM="$PROJECT_ROOT/scripts/layout_geom.py"
[ -f "$GEOM" ] || { echo "找不到 $GEOM" >&2; exit 2; }

fail() { echo "✗ $*" >&2; }
# 比對過幾鍵 / 明寫 '-' 略過幾鍵。結尾的分母用 COMPARED,不用點過的鍵數。
COMPARED=0
SKIPPED=0
info() { echo "  $*"; }

# ─────────────────────────────────────────────── 裝置幾何 ───────────────────
SIZE="$(adbs shell wm size 2>/dev/null | tr -d '\r' | tail -1 | sed 's/.*: //')"
DENSITY="$(adbs shell wm density 2>/dev/null | tr -d '\r' | tail -1 | sed 's/.*: //')"
[ -n "$SIZE" ] && [ -n "$DENSITY" ] || { echo "讀不到裝置幾何（$SERIAL 在嗎？）" >&2; exit 2; }
SCREEN_W="${SIZE%x*}"
SCREEN_H="${SIZE#*x}"

geom() { python3 "$GEOM" --root "$PROJECT_ROOT" --layout "$LAYOUT" --theme "$THEME" \
           ${LAYER:+--layer "$LAYER"} --screen "$SIZE" --density "$DENSITY" "$@"; }

# 先把整份鍵位圖算出來存檔。**不管過不過都存**：過了它是這次跑的座標依據，
# 沒過它是除錯的第一份材料（規範說鍵應該在哪，配上截圖就知道差在哪）。
geom --map > "$OUT_DIR/keymap.txt" 2>"$OUT_DIR/keymap.err" || {
  fail "座標計算失敗："; cat "$OUT_DIR/keymap.err" >&2; exit 2; }
geom --json > "$OUT_DIR/keymap.json" 2>/dev/null

GRID_H="$(python3 -c "import json,sys;print(json.load(open(sys.argv[1]))['grid_height_px'])" "$OUT_DIR/keymap.json")"
BAR_H="$(python3 -c "import json,sys;print(json.load(open(sys.argv[1]))['bar_height_px'])" "$OUT_DIR/keymap.json")"

echo "裝置    : $SERIAL  ${SCREEN_W}x${SCREEN_H} @${DENSITY}dpi"
echo "佈局    : $LAYOUT${LAYER:+（層 $LAYER）}   主題：$THEME"
echo "方案    : ${SCHEMA:-<不釘住>}"
echo "鍵序列  : $KEYS"
echo "算出    : 格線區高 ${GRID_H}px、候選列 ${BAR_H}px"
echo "輸出    : $OUT_DIR"
echo

# ─────────────────────────────────────────────── 安裝與重置 ─────────────────
if [ "$DO_INSTALL" -eq 1 ]; then
  APK="$PROJECT_ROOT/android/app/build/outputs/apk/debug/app-debug.apk"
  [ -f "$APK" ] || { fail "找不到 $APK，先跑 ./gradlew :app:assembleDebug -p android"; exit 2; }
  info "安裝輸入法…"
  adbs install -r -g "$APK" >/dev/null 2>&1 || { fail "輸入法安裝失敗"; exit 2; }
  info "建置並安裝測試靶…"
  TAPK="$("$SCRIPT_DIR/build_input_matrix_app.sh" 2>/dev/null | tail -1)"
  [ -f "$TAPK" ] || { fail "測試靶建置失敗"; exit 2; }
  adbs install -r -g "$TAPK" >/dev/null 2>&1 || { fail "測試靶安裝失敗"; exit 2; }
fi

# ⛔ **裝完才寫。** 這一行從前在第 167 行(安裝在 209 行之後),於是
#   `${APK:-}` 在那個時間點根本還沒被指派 —— device.txt 上寫的是
#   `apk=<未指定 --apk>`,而這一輪明明裝了一份。第五個參數(套件名)也是這一輪
#   補的:有了它才問得到「裝置上真的裝著的那一份」的 sha256。
#   eb3c588 的 commit 標題就是「device.txt 要裝完才寫」,漏了這一支。
rs_write_device_stamp "$ADB" "$SERIAL" "$OUT_DIR/device.txt" "${APK:-}" "$IME_PKG"

if [ "$DO_CLEAR" -eq 1 ]; then
  # 為什麼一定要清：rime session 跨 app 重啟存活。上一輪留下的組字會出現在
  # 這一輪的 preedit 裡（「打了沒打過的字」），驗證截圖因此是髒的，
  # 而且一旦組字沒清乾淨，第一鍵的預期內容就對不上，看起來像佈局壞了。
  rs_assert_destructive_ok "$ADB" "$SERIAL" "pm clear、ime set" || exit 2
  info "pm clear（回到已知狀態）…"
  adbs shell pm clear "$IME_PKG" >/dev/null 2>&1
  adbs shell pm clear "$TARGET_PKG" >/dev/null 2>&1
fi

# ─────────────────────────────────────────────── 釘住方案與佈局 ─────────────
#
# 為什麼要釘：§9.1.1 的自動切換會在方案改變時換佈局，而「當前方案」在
# pm clear 之後是 default.custom.yaml 的第一個（luna_pinyin_tw）。不釘的話
# 這支腳本只驗得到那一份佈局，`--layout` 形同虛設。
#
# 釘方案走 StoreSettings 的 pending_schema（SharedPreferences 純 XML，
# 見 store/StoreController.kt）—— IME 在部署完成、session 重建之後會自己兌現。
# 這是產品裡既有的交接點，不是為了測試另開的後門。
#
# 釘佈局走 §2.3 的搜尋路徑：user_data_dir 排在 assets 之前，而 §9.1.1 第 1 步
# 取的是「搜尋路徑裡最先找到的那一份」。把目標佈局複製一份到
# files/rime/user/layouts/ 就贏得這場競賽 —— t9_pinyin 同時被 cn-t9-pinyin 與
# t9-pinyin 宣告，不釘就只能賭 listFiles() 的順序。
# 複本只改 `for_schema` 與 `auto_for_schema` 兩行（用 sed，不重排 YAML，避免踩到
# §3.2 的 RTS 限制），其餘逐字照抄 —— 幾何與鍵位是**原檔**的，這正是要驗的東西。
#
# ⚠ **`auto_for_schema` 也得改，否則有一整類佈局根本釘不住。** §9.1.1 把「資格」
#   與「自動命中」拆成兩個欄位之後，`cn-t9-pinyin-numrow` 這種「有資格但把自動
#   命中讓出去」的佈局（`auto_for_schema: []`）永遠贏不了讓給它的那一份：
#   只改 `for_schema` 的話，裝置上載入的仍然是 `cn-t9-pinyin`。
#   下面那道「要驗的是 X，實際載入的卻是 Y」的檢查會擋下來並中止 —— 它沒有說謊，
#   但結果是**那份佈局一次都沒被驗過**，而清單裡不會有任何紅字提醒你。
#   （發現於九宮格消歧欄那一輪：改了 numrow 卻驗不到它。）
if [ -n "$SCHEMA" ]; then
  info "釘住方案 $SCHEMA…"
  adbs shell "run-as $IME_PKG mkdir -p shared_prefs" >/dev/null 2>&1
  printf '%s' "<?xml version='1.0' encoding='utf-8' standalone='yes' ?>
<map>
    <string name=\"pending_schema\">$SCHEMA</string>
</map>
" | adbs shell "run-as $IME_PKG sh -c 'cat > shared_prefs/$RS_ANDROID_PREFS_STORE.xml'" >/dev/null 2>&1
fi

SRC_LAYOUT="$PROJECT_ROOT/core/layouts/$LAYOUT.yaml"
if [ -n "$SCHEMA" ] && [ -f "$SRC_LAYOUT" ]; then
  info "釘住佈局 $LAYOUT（複製到 user_data_dir，for_schema 與 auto_for_schema 都綁到 $SCHEMA）…"
  adbs shell "run-as $IME_PKG mkdir -p files/rime/user/layouts" >/dev/null 2>&1
  # `^for_schema:` 不會誤中 `auto_for_schema:`（後者不以 for_schema 開頭）。
  sed -e "s/^for_schema:.*/for_schema: [\"$SCHEMA\"]/" \
      -e "s/^auto_for_schema:.*/auto_for_schema: [\"$SCHEMA\"]/" "$SRC_LAYOUT" \
    | adbs shell "run-as $IME_PKG sh -c 'cat > files/rime/user/layouts/$LAYOUT.yaml'" >/dev/null 2>&1
fi

# ─────────────────────────────────────────────── 啟動與就緒 ─────────────────
# ⛔ 這兩行是**頂層**的，而上面那道閘關在 `if [ "$DO_CLEAR" -eq 1 ]` 裡面 ——
#   `--no-clear` 跑起來時它一次都沒被問過，卻照樣把這台機器的預設輸入法換掉。
#   `verify_device_hygiene.sh` 規則 C 是逐呼叫點的，看得到這件事（前提是它
#   認得 `adbs`；2026-08-14 之前它不認得，所以這兩行從沒被指名過）。
rs_assert_destructive_ok "$ADB" "$SERIAL" "ime enable、ime set" || exit 2
adbs shell ime enable "$IME_ID" >/dev/null 2>&1
adbs shell ime set "$IME_ID" >/dev/null 2>&1
CUR="$(adbs shell settings get secure default_input_method 2>/dev/null | tr -d '\r')"
[ "$CUR" = "$IME_ID" ] || { fail "設不成預設輸入法（目前 $CUR）"; exit 2; }

adbs logcat -c >/dev/null 2>&1
adbs shell am force-stop "$TARGET_PKG" >/dev/null 2>&1
adbs shell am start -n "$TARGET_ACT" --es field "$FIELD" >/dev/null 2>&1

# 等測試靶自己說 READY，再點一下輸入框把鍵盤叫出來。
for i in $(seq 1 20); do
  log_has "READY $FIELD" adbs logcat -d -s "$TAG:I" && break
  sleep 0.5
done
FIELD_XY="$(adbs shell "uiautomator dump /sdcard/rime_bounds.xml >/dev/null 2>&1; cat /sdcard/rime_bounds.xml" \
  2>/dev/null | tr -d '\r' | python3 -c '
import sys, re, xml.etree.ElementTree as ET
try: root = ET.fromstring(sys.stdin.read())
except Exception: sys.exit(0)
for n in root.iter("node"):
    if n.get("content-desc") == "rime_matrix_input":
        m = re.match(r"\[(\d+),(\d+)\]\[(\d+),(\d+)\]", n.get("bounds",""))
        if m:
            a,b,c,d = map(int, m.groups()); print((a+c)//2, (b+d)//2)
        break
')"
[ -n "$FIELD_XY" ] && adbs shell input tap $FIELD_XY >/dev/null 2>&1
sleep 1.5

# 應用層就緒訊號。**不是** mIsInputViewShown —— 那只說明鍵盤畫出來了。
wait_ready() {
  local i
  for i in $(seq 1 "${RIME_READY_TRIES:-40}"); do
    if log_has "phase → READY" adbs logcat -d -s RimeRuntime:I; then
      info "librime 已就緒（RimeRuntime phase → READY，第 $i 次輪詢）"
      return 0
    fi
    if log_has "phase → FAILED" adbs logcat -d -s RimeRuntime:I; then
      fail "RimeRuntime 進入 FAILED，部署失敗"
      adbs logcat -d -s RimeRuntime:I RimeIME:I 2>/dev/null | tail -40 > "$OUT_DIR/rime-fail.log"
      return 1
    fi
    sleep 3
  done
  fail "等不到 RimeRuntime phase → READY"
  adbs logcat -d -s RimeRuntime:I RimeIME:I 2>/dev/null | tail -60 > "$OUT_DIR/rime-fail.log"
  return 1
}
wait_ready || exit 2

# 方案切換是在部署完成之後才兌現的，所以要再等一下 LayoutHost 落定。
sleep 3
ACTIVE_LAYOUT="$(adbs logcat -d -s LayoutHost:I 2>/dev/null | tr -d '\r' \
  | sed -n 's/.*佈局 → \([a-zA-Z0-9_-]*\).*/\1/p' | tail -1)"
ACTIVE_SCHEMA="$(adbs logcat -d -s RimeIME:I 2>/dev/null | tr -d '\r' \
  | sed -n 's/.*方案 → \([a-zA-Z0-9_-]*\).*/\1/p' | tail -1)"
info "裝置回報：佈局=${ACTIVE_LAYOUT:-<未知>} 方案=${ACTIVE_SCHEMA:-<未知>}"

# 沒驗到就別假裝驗過了。這一關比什麼都重要：測到別份佈局卻報綠燈，
# 比沒有測試更糟 —— 它會說謊。
if [ -z "$ACTIVE_LAYOUT" ]; then
  # ⚠ 這一條以前是「有回報才比」——裝置沒回報的時候,整道關卡連跑都沒跑,
  #   而輸出上看不出差別。**「不知道驗的是哪一份」與「驗的是對的那一份」
  #   不是同一件事**,而前者正是這道關卡存在的理由。
  fail "裝置沒有回報載入了哪一份佈局(logcat 裡找不到「佈局 → …」)。"
  fail "  不知道驗的是哪一份,就不能把結果當成 $LAYOUT 的結果。中止。"
  adbs logcat -d -s LayoutHost:I RimeIME:I 2>/dev/null | tail -60 > "$OUT_DIR/no-layout-report.log"
  adbs exec-out screencap -p > "$OUT_DIR/fail-no-layout-report.png" 2>/dev/null
  exit 2
fi
if [ "$ACTIVE_LAYOUT" != "$LAYOUT" ]; then
  fail "要驗的是 $LAYOUT，裝置上實際載入的卻是 $ACTIVE_LAYOUT。中止。"
  adbs exec-out screencap -p > "$OUT_DIR/fail-wrong-layout.png" 2>/dev/null
  exit 2
fi
# --schema 有指定的話,方案也要對得上 —— 佈局對、方案不對照樣是在驗別的東西。
if [ -n "$SCHEMA" ] && [ -n "$ACTIVE_SCHEMA" ] && [ "$ACTIVE_SCHEMA" != "$SCHEMA" ]; then
  fail "要驗的方案是 $SCHEMA，裝置上載入的卻是 $ACTIVE_SCHEMA。中止。"
  exit 2
fi

# ─────────────────────────────────────────────── 垂直錨點 ───────────────────
#
# layout_geom.py 的 y 原點是「格線區左上角」，與裝置無關。要變成螢幕絕對座標，
# 只缺一個偏移量，而那個偏移量**不該用猜的**：向 WindowManager 要 IME 視窗的
# frame，加上候選列高度就是格線區頂端。
#
# 同時做一次一致性檢查：視窗高 − 候選列 − 格線區 = 系統底部 inset。
# 這個差值若不落在合理範圍，代表高度模型與渲染器已經對不上了
# （例如規範改了但這支腳本沒跟上），此時**寧可中止**也不要往下戳。
ime_frame() {
  adbs shell dumpsys window windows 2>/dev/null | tr -d '\r' | python3 -c '
import sys, re
txt = sys.stdin.read()
m = re.search(r"Window\{[^}]*InputMethod\}:(.*?)(?=\n  Window \#|\Z)", txt, re.S)
if not m: sys.exit(0)
f = re.search(r"\bframe=\[(\d+),(\d+)\]\[(\d+),(\d+)\]", m.group(1))
if f: print(" ".join(f.groups()))
'
}
FRAME="$(ime_frame)"
[ -n "$FRAME" ] || { fail "讀不到 IME 視窗 frame（dumpsys window）"; exit 2; }
set -- $FRAME
FRAME_TOP="$2"; FRAME_BOT="$4"
WIN_H=$((FRAME_BOT - FRAME_TOP))
GRID_TOP=$((FRAME_TOP + BAR_H))
INSET=$((WIN_H - BAR_H - GRID_H))
info "IME 視窗 y=[$FRAME_TOP,$FRAME_BOT]（高 $WIN_H）→ 格線區頂端 $GRID_TOP，推得底部 inset $INSET px"
if [ "$INSET" -lt -8 ] || [ "$INSET" -gt 220 ]; then
  fail "一致性檢查沒過：視窗高 $WIN_H − 候選列 $BAR_H − 格線區 $GRID_H = $INSET px，不是合理的系統 inset。"
  fail "高度模型（§8.8.0）與渲染器已經對不上，座標不可信。鍵位圖見 $OUT_DIR/keymap.txt"
  adbs exec-out screencap -p > "$OUT_DIR/fail-geometry.png" 2>/dev/null
  exit 2
fi
echo

# ─────────────────────────────────────────────── 逐鍵驗證 ───────────────────
last_state() {
  adbs logcat -d -s "$TAG:I" 2>/dev/null | tr -d '\r' \
    | grep -F "$TAG: STATE " | tail -1 | sed 's/.*STATE //'
}
state_text() { echo "$1" | sed -n 's/^[^ ]* |\(.*\)| cs=.*/\1/p'; }
state_cs()   { echo "$1" | sed -n 's/.*cs=\(-\?[0-9]*\) .*/\1/p'; }

IFS=',' read -r -a KEY_ARR <<< "$KEYS"
IFS=',' read -r -a EXP_ARR <<< "$EXPECT_CSV"

adbs logcat -c >/dev/null 2>&1
FAILURES=0
RESULTS="$OUT_DIR/results.tsv"
: > "$RESULTS"

printf '%-3s %-16s %9s %9s  %-16s %-16s %s\n' 步 鍵id x y 實際 預期 判定
printf '%s\n' "---------------------------------------------------------------------------------"

STEP=0
LAST_CS="-1"
# 「這一輪裡 librime 到底有沒有參與」要看**任何一步**有沒有組字區,不是最後一步。
# 見底下那段檢查的註解 —— 只看最後一步會把正常的中文輸入判成失敗。
SAW_COMPOSING=0
COMPOSING_AT=""
for KID in "${KEY_ARR[@]}"; do
  XY="$(geom --key "$KID" --grid-top "$GRID_TOP" 2>"$OUT_DIR/keyerr.txt")"
  if [ -z "$XY" ]; then
    fail "佈局 $LAYOUT 裡沒有 id 為 '$KID' 的可點鍵；可用的鍵見 $OUT_DIR/keymap.txt"
    cat "$OUT_DIR/keyerr.txt" >&2
    exit 2
  fi
  set -- $XY
  KX="$1"; KY="$2"
  adbs shell input tap "$KX" "$KY" >/dev/null 2>&1
  sleep 0.9
  RAW="$(last_state)"
  ACTUAL="$(state_text "$RAW")"
  LAST_CS="$(state_cs "$RAW")"
  if [ "${LAST_CS:-"-1"}" != "-1" ] && [ "$SAW_COMPOSING" -eq 0 ]; then
    SAW_COMPOSING=1
    COMPOSING_AT="$STEP（$KID）"
  fi
  EXP="${EXP_ARR[$STEP]:-}"
  [ "$EXP" = "<empty>" ] && EXP=""

  if [ -z "${EXP_ARR[$STEP]+x}" ] || [ "$EXP" = "-" ]; then
    # 略過要自己有一個計數,而且結尾的分母要用「**比對過**幾鍵」,
    # 不能用「點過幾鍵」—— 後者把沒比對的也算成通過了。
    VERDICT="SKIP"
    SKIPPED=$((SKIPPED + 1))
  elif [ "$ACTUAL" = "$EXP" ]; then
    VERDICT="OK"
    COMPARED=$((COMPARED + 1))
  else
    VERDICT="DIFF"
    COMPARED=$((COMPARED + 1))
    FAILURES=$((FAILURES + 1))
  fi
  printf '%-3s %-16s %9s %9s  %-16s %-16s %s\n' \
    "$STEP" "$KID" "$KX" "$KY" "${ACTUAL:-<空>}" "${EXP:-<空>}" "$VERDICT"
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$STEP" "$KID" "$KX" "$KY" "$ACTUAL" "$EXP" "$VERDICT" >> "$RESULTS"
  STEP=$((STEP + 1))
done

echo
adbs exec-out screencap -p > "$OUT_DIR/final.png" 2>/dev/null

# 組字區的確認。session 失效時 fallbackKey() 也會把字面字元 commit 進去，
# 內容看起來一模一樣 —— 唯一可靠的分水嶺是「有沒有組字範圍」。
#
# ⚠ 判準是「**有沒有任何一步**出現過組字區」，不是「最後一步還在組字」。
#    原本寫的是後者，於是**正常的中文輸入會被判成失敗**：
#      k_mno, k_ghi, space → 你     ← 選了字就上屏，組字區當然消失
#    腳本卻報「走的是 fallback 直接上屏，librime 沒真的參與」——
#    一個看起來像重大缺陷、實際上是這支腳本自己按出來的假失敗，
#    而且它指控的正好是相反的事實（librime 參與得最完整的那一種序列）。
#    fallback 那條路**任何一步都不會**產生組字區，所以改成看整輪，
#    要擋的東西一樣擋得住，而且不再冤枉正常的序列。
if [ "$CHECK_COMPOSING" -eq 1 ]; then
  if [ "$SAW_COMPOSING" -eq 0 ]; then
    fail "整輪沒有任何一步出現組字區（cs 一直是 -1）：字雖然進去了，走的卻是 fallback 直接上屏，librime 沒真的參與。"
    FAILURES=$((FAILURES + 1))
  else
    info "組字區確認：第 $COMPOSING_AT 步起有組字區（librime 真的有參與），最後一步 cs=$LAST_CS"
  fi
fi

if [ "$FAILURES" -gt 0 ]; then
  echo
  fail "$FAILURES 個判定沒過。除錯材料："
  echo "    預期座標與完整鍵位圖 : $OUT_DIR/keymap.txt"
  echo "    機讀版（含每鍵矩形） : $OUT_DIR/keymap.json"
  echo "    逐步結果             : $RESULTS"
  echo "    實際截圖             : $OUT_DIR/final.png"
  echo
  sed -n '1,12p' "$OUT_DIR/keymap.txt"
  [ "$KEEP" -eq 1 ] || adbs shell am force-stop "$TARGET_PKG" >/dev/null 2>&1
  exit 1
fi

# ⚠ 分母以前用的是 ${#KEY_ARR[@]}(**點過**幾鍵),而不是實際比對過幾鍵。
#   於是「點了 8 下、比對了 0 下」也會印「全部 8 鍵通過」。
if [ "$COMPARED" -eq 0 ]; then
  fail "一鍵都沒有比對到（$SKIPPED 鍵標記為不比對）—— 這不是通過。"
  [ "$KEEP" -eq 1 ] || adbs shell am force-stop "$TARGET_PKG" >/dev/null 2>&1
  exit 1
fi
if [ "$SKIPPED" -gt 0 ]; then
  echo "  註:$SKIPPED 鍵在 --expect 裡明寫 '-'，這一輪沒有比對它們。"
fi
echo "✓ $LAYOUT：比對過 $COMPARED/${#KEY_ARR[@]} 鍵，全數相符（座標由 $LAYOUT.yaml 算出，非寫死）"
echo "   鍵位圖：$OUT_DIR/keymap.txt   截圖：$OUT_DIR/final.png"
[ "$KEEP" -eq 1 ] || adbs shell am force-stop "$TARGET_PKG" >/dev/null 2>&1
exit 0
