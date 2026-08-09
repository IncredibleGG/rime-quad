#!/usr/bin/env bash
#
# verify_syllables.sh — 九宮格拼音消歧欄的**畫面**驗證
#
# ── 為什麼需要這支腳本 ──────────────────────────────────────────────────
#
#   消歧欄這條線交付時,有的只是「純函式的單元測試 + 人工看的截圖」。
#   而這個專案已經吃過七次「單元測試綠、使用者打開看不到」的虧,
#   使用者原本的回報(cn-t9-pinyin 沒有消歧欄)到今天都沒有重現、原因不明。
#
#   所以這支腳本**只認畫面上的像素**:截圖 → 裁切到消歧欄該在的那一塊 →
#   OCR → 斷言讀得到 `ni` / `mi`。內部狀態對而畫面沒畫出來,它會紅。
#
#   ⚠ 裁切必須夠緊。候選列上的 comment 本來就印著「ni hao」——
#   把候選列一起裁進來,OCR 會讀到 ni 而永遠是綠的。那是最容易做出來的假綠燈。
#
# ── 四道關 ──────────────────────────────────────────────────────────────
#   0  方案漂移:裝置上(APK 裡)的方案必須與 core/data/schemas/ 一致
#   1  範圍非空:掃到的九宮格佈局數必須 >= RS_MIN_T9_LAYOUTS(§2-G)
#   2  第一個音節:打 MG… 之後,消歧欄上讀得到 ni 與 mi
#   3  第二個音節:點下 ni 之後,同一塊區域讀得到 hao / gan / gao 之一
#
# ── 植入違規(證明它會紅)─────────────────────────────────────────────
#   --plant stale-schema   把裝置上的方案換成舊的單編碼版 → 第 0 關必須紅
#   --plant narrow-scope   只掃一份佈局              → 第 1 關必須紅
#   --plant bad-slot-ids   把佈局的 syllable_slots 指到不存在的 key id
#                          → 格位替換不會發生,畫面照常顯示標點 → 第 2 關必須紅
#                          (這正是「有人把 pu_comma 改名」的真實形狀)
#
#   帶 --plant 時**退出碼是反的**:斷言紅了才算通過(exit 0),
#   植入了卻還是綠的就是這支腳本壞了(exit 1)。而且不是「紅就算過」——
#   每一種植入都指名它**應該踩紅哪一條**,踩紅別條(模擬器抽風、裝不上去)
#   一樣算失敗。exit 2 保留給工具/環境問題,不會被反轉。
#
#   前兩種只碰主機上的檔案,不需要裝置,所以第 1 關之後就收尾 ——
#   這樣它們才進得了快車道(見 .github/workflows/build.yml)。
#   bad-slot-ids 斷言的是畫面像素,只能跟著模擬器那條車道跑。
#
#   --check-ci    不跑任何驗證,只檢查上面宣告的每一種 --plant 都真的接進了
#                 .github/workflows/build.yml。這一支的三個反向測試曾經
#                 **一次都沒有在 CI 上跑過**(檔頭寫著、workflow 沒接),
#                 而那看起來與一切正常一模一樣。--check-ci --self-test
#                 會先把接線拆掉一條,證明這一關真的會紅。
#
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
# shellcheck source=lib/product.sh
. "$HERE/lib/product.sh"

SDK="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Android/Sdk}}"
ADB="$SDK/platform-tools/adb"
SERIAL="${RIME_SERIAL:-${ANDROID_SERIAL:-emulator-${RIME_EMU_PORT:-5554}}}"
IME_ID="${RIME_IME_ID:-$RS_ANDROID_IME_ID}"
IME_PKG="${IME_ID%%/*}"
TARGET_PKG=dev.rime.inputmatrix
TARGET_ACT="$TARGET_PKG/.MainActivity"
THEME="${RIME_THEME:-default-light}"
SCHEMA=t9_pinyin
OUT_DIR="$ROOT/build/verify-syllables"
APK=""
PLANT=""
CHECK_CI=0
SELF_TEST=0
# 每一種植入**指名**它該踩紅的那一條 FAIL 訊息。只看退出碼是不夠的:
# 模擬器沒開、APK 裝不上去一樣是 exit 1,而那不叫「反向測試通過」。
plant_expect_re() {
  case "$1" in
    stale-schema) echo '不一致|alphabet 不含小寫拼音' ;;
    narrow-scope) echo '少於下界' ;;
    bad-slot-ids) echo '消歧欄上讀不到 ni/mi' ;;
    *) echo '' ;;
  esac
}
# 只碰主機檔案、不需要裝置的植入。列在這裡的可以進快車道。
plant_is_host_only() {
  case "$1" in
    stale-schema|narrow-scope) return 0 ;;
    *) return 1 ;;
  esac
}
# §2-G:掃描範圍必須非空。三份九宮格佈局(cn-t9-pinyin / -numrow / t9-pinyin)
# 少一份就代表有人刪了佈局、或這裡的判準壞了 —— 兩種都必須紅,而不是靜靜地少驗一份。
MIN_T9_LAYOUTS="${RS_MIN_T9_LAYOUTS:-3}"

# tesseract 可以是系統的,也可以由 RIME_TESSERACT 指定(CI 上用 apt 裝)。
TESSERACT="${RIME_TESSERACT:-$(command -v tesseract || true)}"
TESSDATA="${TESSDATA_PREFIX:-}"

while [ $# -gt 0 ]; do
  case "$1" in
    --serial) SERIAL="$2"; shift 2 ;;
    --apk)    APK="$2"; shift 2 ;;
    --plant)  PLANT="$2"; shift 2 ;;
    --theme)  THEME="$2"; shift 2 ;;
    --out)    OUT_DIR="$2"; shift 2 ;;
    --check-ci)  CHECK_CI=1; shift ;;
    --self-test) SELF_TEST=1; shift ;;
    -h|--help)
      # 範圍不要寫死行號:檔頭一長,說明就會被默默截掉一半。
      sed -n '2,/^set -uo pipefail$/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "未知參數: $1" >&2; exit 2 ;;
  esac
done

mkdir -p "$OUT_DIR"
# 打錯的 --plant 名字必須當場停。否則它會被當成「沒有植入」跑完一整輪、
# 全綠、然後被反轉成 exit 1 或(更糟)被當成通過 —— 兩種都在說謊。
if [ -n "$PLANT" ] && [ -z "$(plant_expect_re "$PLANT")" ]; then
  echo "不認得的 --plant「$PLANT」。可用:stale-schema / narrow-scope / bad-slot-ids" >&2
  exit 2
fi
adbs() { "$ADB" -s "$SERIAL" "$@"; }
info() { echo "[syllables] $*" >&2; }
pass() { echo "  [PASS] $*"; }
FAILURES=0
FAIL_LOG=""
# ⚠ 訊息要留下來。「紅了」不等於「該紅的那一條紅了」:模擬器抽風、APK 裝不上去
#   同樣會讓退出碼變 1,而 --plant 的斷言若只看退出碼,就會把環境故障當成
#   「反向測試通過」—— 那正是這一支要防的假綠燈的另一個形狀。
fail() { echo "  [FAIL] $*" >&2; FAILURES=$((FAILURES + 1)); FAIL_LOG="$FAIL_LOG
$*"; }

# 收尾。帶 --plant 時退出碼是**反的**,而且要求紅的是指名的那一條。
finish() {
  echo
  if [ -n "$PLANT" ]; then
    local want; want="$(plant_expect_re "$PLANT")"
    if [ "$FAILURES" -eq 0 ]; then
      echo "✗ 植入了 $PLANT,斷言卻還是全綠 —— 這一關沒有在守。artifact:$OUT_DIR"
      return 1
    fi
    if ! printf '%s' "$FAIL_LOG" | grep -qE "$want"; then
      echo "✗ 植入了 $PLANT,紅的卻不是該紅的那一條(要找的是 /$want/)。"
      echo "  實際紅的是:"
      printf '%s\n' "$FAIL_LOG" | sed '/^$/d; s/^/    /'
      echo "  artifact:$OUT_DIR"
      return 1
    fi
    echo "✓ 反向測試通過:植入 $PLANT 之後,該紅的那一條紅了(共 $FAILURES 項)"
    echo "   artifact:$OUT_DIR"
    return 0
  fi
  if [ "$FAILURES" -gt 0 ]; then
    echo "✗ $FAILURES 項沒過。artifact:$OUT_DIR"
    return 1
  fi
  echo "✓ $1"
  echo "   artifact:$OUT_DIR"
  return 0
}

step() { echo; echo "── $* ──"; }

# IME 視窗的 frame,以及由它回推的格線區頂端。
#
# ⚠ **每次截圖與每次點擊之前都要重讀。** 上方橫排一出現,IME 視窗就往上長,
#   frame_top 會變小 —— 拿打字前讀到的值去裁,裁到的正好是候選列,而候選列上的
#   comment 本來就印著「ni hao」,於是 OCR 讀得到 ni,測試永遠綠。
#   那是這支腳本最容易做出來的假綠燈(第一版就是這樣,實測抓到)。
#
# 格線區用**視窗底端**回推,不用 frame_top + bar_h:格線區永遠貼著視窗底部,
# 而上方多不多一排會讓 frame_top + bar_h 差一整排的高度,按鍵高度只有一百多 px
# —— 差一排就點到隔壁列。
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
}

# 裝置與 OCR 的工具檢查。**依然不准跳過**,只是挪到真的要碰裝置之前才問 ——
# 第 0/1 關純粹讀主機上的檔案,不需要 adb 也不需要 tesseract,而
# stale-schema / narrow-scope 兩個植入只驗那兩關。要求它們先有一台模擬器,
# 等於把這兩個反向測試永遠關在慢車道外面(它們至今一次都沒跑過)。
require_device_tools() {
  [ -x "$ADB" ] || { echo "找不到 adb:$ADB" >&2; exit 2; }
  if [ -z "$TESSERACT" ] || [ ! -x "$TESSERACT" ]; then
    # ⚠ 不可以「找不到 OCR 就跳過」。跳過的關卡與綠燈長得一模一樣。
    echo "找不到 tesseract。請安裝(apt-get install -y tesseract-ocr)或設 RIME_TESSERACT。" >&2
    exit 2
  fi
  # ⚠ 同樣的道理,但這一條是**吃過的虧**:GitHub runner 上沒有 Pillow,
  #   裁切那段 python 每次都 ModuleNotFoundError,而腳本沒有 -e、
  #   `ocr_region` 的輸出檔就是空的 —— 於是三份佈局都報
  #   「消歧欄上讀不到 ni/mi」。**看起來像產品壞了,其實是關卡自己缺套件。**
  #   工具缺席必須在這裡就停,而且訊息要指向安裝,不要指向產品。
  if ! python3 -c "import PIL" >/dev/null 2>&1; then
    echo "python3 缺 Pillow(裁切要用)。請安裝:pip3 install --user pillow" >&2
    exit 2
  fi
}

# ═════════════════ --check-ci:反向測試有沒有真的接進 CI ═════════════════
#
# 這一關擋的是這支腳本自己踩過的坑:檔頭把三種 --plant 寫得清清楚楚,
# workflow 卻一次都沒有呼叫過。「宣告了反向測試」與「反向測試在跑」
# 在任何日誌上看起來都一模一樣 —— 除非有人去比對這兩份檔案。
if [ "$CHECK_CI" -eq 1 ]; then
  WF="$ROOT/.github/workflows/build.yml"
  SELF="${BASH_SOURCE[0]}"
  if [ "$SELF_TEST" -eq 1 ]; then
    # 反向測試的反向測試:把接線拆掉一條,這一關必須紅。
    TMPWF="$(mktemp -d)/build.yml"
    mkdir -p "$(dirname "$TMPWF")"
    grep -v -- "--plant narrow-scope" "$WF" > "$TMPWF"
    WF="$TMPWF"
    info "自我測試:用一份**拿掉 narrow-scope 接線**的 build.yml"
  fi
  [ -f "$WF" ] || { echo "找不到 $WF" >&2; exit 2; }

  # 宣告的植入種類直接從檔頭讀,不要在這裡再抄一份 —— 抄的那一份會漂移,
  # 而漂移時「檢查通過」的那一份說了算。
  mapfile -t DECLARED < <(sed -n 's/^#   --plant \([a-z-]\+\).*/\1/p' "$SELF" | sort -u)
  if [ "${#DECLARED[@]}" -lt 3 ]; then
    echo "!! 檔頭只解析出 ${#DECLARED[@]} 種 --plant —— 解析式壞了,這一關在空轉。" >&2
    exit 1
  fi
  info "檔頭宣告的植入:${DECLARED[*]}"
  MISSING=0
  for pl in "${DECLARED[@]}"; do
    if grep -q -- "--plant $pl" "$WF"; then
      pass "build.yml 有跑 --plant $pl"
    else
      fail "build.yml 沒有跑 --plant $pl —— 檔頭宣告了「證明它會紅」,實際上沒人證明過。"
      MISSING=$((MISSING + 1))
    fi
  done
  # 正向那一次也要在,否則把正向拿掉、只留植入,一樣是全綠。
  if grep -qE "verify_syllables\.sh --apk" "$WF"; then
    pass "build.yml 有跑正向(--apk,不帶 --plant)"
  else
    fail "build.yml 找不到不帶 --plant 的正向呼叫。"
    MISSING=$((MISSING + 1))
  fi
  [ "$SELF_TEST" -eq 1 ] && rm -rf "$(dirname "$WF")"
  echo
  if [ "$SELF_TEST" -eq 1 ]; then
    if [ "$MISSING" -eq 0 ]; then
      echo "✗ 自我測試失敗:接線都被拆掉一條了,--check-ci 卻還是綠的。"
      exit 1
    fi
    echo "✓ 自我測試通過:拆掉接線之後 --check-ci 會紅($MISSING 條)"
    exit 0
  fi
  if [ "$MISSING" -gt 0 ]; then
    echo "✗ $MISSING 條反向測試沒有接進 CI"
    exit 1
  fi
  echo "✓ 檔頭宣告的 ${#DECLARED[@]} 種植入都接進 build.yml 了,正向那一次也在"
  exit 0
fi

# ═══════════════════════ 第 0 關:方案漂移 ═══════════════════════
#
# core/data/shared/ 是 scripts/collect_data.sh 產生的,而且在 .gitignore 裡。
# 協調端改了 core/data/schemas/ 之後,沒重跑 collect_data.sh 的機器上,
# 裝置拿到的仍是**舊方案** —— 症狀是 rs_set_input() 回 false,
# 看起來像前端寫壞了。這一關就是為了不讓下一個人再查一次那件事。
step "0. 方案漂移"
SRC_SCHEMA="$ROOT/core/data/schemas/$SCHEMA.schema.yaml"
SHIPPED_SCHEMA="$ROOT/core/data/shared/$SCHEMA.schema.yaml"
[ -f "$SRC_SCHEMA" ] || { echo "找不到 $SRC_SCHEMA" >&2; exit 2; }

if [ "$PLANT" = "stale-schema" ]; then
  info "植入違規:把 shared 的方案換成舊的單編碼版"
  mkdir -p "$(dirname "$SHIPPED_SCHEMA")"
  sed -e "s/^  alphabet: .*/  alphabet: 'ADGJMPTW'/" \
      -e "s/^  spelling_hints: .*/  spelling_hints: 5/" "$SRC_SCHEMA" > "$OUT_DIR/planted.schema.yaml"
  SHIPPED_SCHEMA="$OUT_DIR/planted.schema.yaml"
fi

if [ ! -f "$SHIPPED_SCHEMA" ]; then
  fail "core/data/shared/$SCHEMA.schema.yaml 不存在 —— 先跑 scripts/collect_data.sh。"
elif ! cmp -s "$SRC_SCHEMA" "$SHIPPED_SCHEMA"; then
  fail "裝置要裝的方案與 core/data/schemas/ 不一致(shared 是產生檔,且在 gitignore 裡)。"
  fail "  先跑 scripts/collect_data.sh;跑不動時最少要把這一個檔案複製過去:"
  fail "  cp core/data/schemas/$SCHEMA.schema.yaml core/data/shared/"
  diff <(grep -E '^  (alphabet|spelling_hints):' "$SRC_SCHEMA") \
       <(grep -E '^  (alphabet|spelling_hints):' "$SHIPPED_SCHEMA") >&2 || true
else
  pass "core/data/shared/$SCHEMA.schema.yaml 與 schemas/ 一致"
fi

# 雙編碼是消歧欄的前提:沒有小寫拼音,rs_set_input("niGAM") 一定被拒。
if grep -qE "^  alphabet: '.*[a-z].*'" "$SHIPPED_SCHEMA" 2>/dev/null; then
  pass "方案是雙編碼(alphabet 含小寫拼音)"
else
  fail "方案的 alphabet 不含小寫拼音 —— 音節改寫一定失敗(rs_set_input 回 false)"
fi

# ═══════════════════════ 第 1 關:掃描範圍 ═══════════════════════
step "1. 掃描範圍(§2-G:範圍必須非空)"
mapfile -t T9_LAYOUTS < <(
  cd "$ROOT/core/layouts" || exit 1
  for f in *.yaml; do
    grep -qE "^for_schema:.*\"$SCHEMA\"" "$f" && basename "$f" .yaml
  done | sort
)
if [ "$PLANT" = "narrow-scope" ]; then
  info "植入違規:只掃一份佈局"
  T9_LAYOUTS=("${T9_LAYOUTS[0]}")
fi
info "宣告 for_schema 含 $SCHEMA 的佈局:${T9_LAYOUTS[*]:-<空>}"
if [ "${#T9_LAYOUTS[@]}" -lt "$MIN_T9_LAYOUTS" ]; then
  fail "只掃到 ${#T9_LAYOUTS[@]} 份九宮格佈局,少於下界 $MIN_T9_LAYOUTS。"
  fail "  這一關擋的是「判準壞了 → 掃到 0 份 → 全綠」。"
else
  pass "掃到 ${#T9_LAYOUTS[@]} 份九宮格佈局(下界 $MIN_T9_LAYOUTS)"
fi

# 掃不到就沒有東西可驗了,直接收尾(前面已經記了 FAIL)。
[ "${#T9_LAYOUTS[@]}" -gt 0 ] || { echo; echo "✗ 沒有可驗的佈局"; exit 1; }

# 主機端的植入到這裡就驗完了 —— 第 0/1 關只讀檔案。再往下開模擬器
# 證明不了多一件事,卻會讓這兩個反向測試永遠上不了快車道。
if [ -n "$PLANT" ] && plant_is_host_only "$PLANT"; then
  info "$PLANT 只驗主機端的第 0/1 關,不需要裝置 —— 到此收尾。"
  finish ""; exit $?
fi

# ═══════════════════════ 裝置準備 ═══════════════════════
require_device_tools
if [ -n "$APK" ]; then
  info "安裝 $APK"
  adbs install -r -g -t "$APK" >/dev/null 2>&1 || { echo "安裝失敗" >&2; exit 2; }
fi

# 每一份佈局跑一輪
for LAYOUT in "${T9_LAYOUTS[@]}"; do
  step "2/3. $LAYOUT"
  LOUT="$OUT_DIR/$LAYOUT"
  mkdir -p "$LOUT"

  # ── 釘方案與佈局 ────────────────────────────────────────────────
  # ⚠ pm clear 會把我們踢出「已啟用的輸入法」,系統當場退回 Gboard;
  #   而 force-stop 待測 IME 會把套件打進 stopped 狀態、**再也回不來**。
  #   所以:只用 pm clear(它本來就會殺掉行程),而且事後一定要確認預設輸入法。
  adbs shell pm clear "$IME_PKG" >/dev/null 2>&1
  adbs shell pm clear "$TARGET_PKG" >/dev/null 2>&1
  sleep 3

  adbs shell "run-as $IME_PKG mkdir -p shared_prefs" >/dev/null 2>&1
  printf '%s' "<?xml version='1.0' encoding='utf-8' standalone='yes' ?>
<map>
    <string name=\"pending_schema\">$SCHEMA</string>
</map>
" | adbs shell "run-as $IME_PKG sh -c 'cat > shared_prefs/$RS_ANDROID_PREFS_STORE.xml'" >/dev/null 2>&1

  SRC_LAYOUT="$ROOT/core/layouts/$LAYOUT.yaml"
  adbs shell "run-as $IME_PKG mkdir -p files/rime/user/layouts" >/dev/null 2>&1
  PLANTED_NOTE=""
  if [ "$PLANT" = "bad-slot-ids" ]; then
    PLANTED_NOTE="(已植入 bad-slot-ids)"
    SED_SLOTS='s/^    syllable_slots: .*/    syllable_slots: ["nope_1", "nope_2", "nope_3"]/'
  else
    SED_SLOTS='s/^__never__$/&/'
  fi
  sed -e "s/^for_schema:.*/for_schema: [\"$SCHEMA\"]/" \
      -e "s/^auto_for_schema:.*/auto_for_schema: [\"$SCHEMA\"]/" \
      -e "s/^deprecated:.*/deprecated: false/" \
      -e "$SED_SLOTS" "$SRC_LAYOUT" \
    | adbs shell "run-as $IME_PKG sh -c 'cat > files/rime/user/layouts/$LAYOUT.yaml'" >/dev/null 2>&1

  # ── 設成預設輸入法(並確認)───────────────────────────────────────
  IME_NOW=""
  for _ in $(seq 1 20); do
    adbs shell ime enable "$IME_ID" >/dev/null 2>&1
    adbs shell ime set "$IME_ID" >/dev/null 2>&1
    IME_NOW="$(adbs shell settings get secure default_input_method 2>/dev/null | tr -d '\r')"
    [ "$IME_NOW" = "$IME_ID" ] && break
    sleep 1
  done
  if [ "$IME_NOW" != "$IME_ID" ]; then
    fail "$LAYOUT:設不成預設輸入法(現在是 ${IME_NOW:-<空>})—— 接下來會對著別的鍵盤打字"
    continue
  fi

  adbs logcat -c >/dev/null 2>&1
  adbs shell am force-stop "$TARGET_PKG" >/dev/null 2>&1
  adbs shell am start -n "$TARGET_ACT" --es field text >/dev/null 2>&1
  sleep 4
  FIELD_XY="$(adbs shell "uiautomator dump /sdcard/b.xml >/dev/null 2>&1; cat /sdcard/b.xml" 2>/dev/null \
    | tr -d '\r' | python3 -c '
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
  # shellcheck disable=SC2086
  [ -n "$FIELD_XY" ] && adbs shell input tap $FIELD_XY >/dev/null 2>&1
  for _ in $(seq 1 40); do
    adbs logcat -d -s RimeRuntime:I 2>/dev/null | tr -d '\r' | grep -q "READY" && break
    sleep 3
  done
  sleep 3

  ACTIVE="$(adbs logcat -d 2>/dev/null | tr -d '\r' | sed -n 's/.*佈局 . \([a-zA-Z0-9_-]*\).*/\1/p' | tail -1)"
  if [ -z "$ACTIVE" ]; then
    fail "$LAYOUT:裝置沒有回報任何佈局 —— 前景鍵盤可能根本不是我們的"
    adbs exec-out screencap -p > "$LOUT/fail-nolayout.png" 2>/dev/null
    continue
  fi
  if [ "$ACTIVE" != "$LAYOUT" ]; then
    fail "$LAYOUT:裝置上載入的卻是 $ACTIVE。驗到別份佈局卻報綠燈比沒測更糟。"
    continue
  fi
  info "裝置確認:佈局=$ACTIVE $PLANTED_NOTE"

  # ── 幾何 ────────────────────────────────────────────────────────
  WM_SIZE="$(adbs shell wm size 2>/dev/null | tr -d '\r' | sed -n 's/.*: *\([0-9]*x[0-9]*\).*/\1/p' | tail -1)"
  WM_DENS="$(adbs shell wm density 2>/dev/null | tr -d '\r' | sed -n 's/.*: *\([0-9]*\).*/\1/p' | tail -1)"
  python3 "$ROOT/scripts/layout_geom.py" --root "$ROOT" --layout "$LAYOUT" --theme "$THEME" \
    --screen "$WM_SIZE" --density "$WM_DENS" --json > "$LOUT/keymap.json" 2>"$LOUT/geom.err" || {
      fail "$LAYOUT:座標計算失敗,見 $LOUT/geom.err"; continue; }

  # 打字順序 M G G A M(= ni + hao)。鍵 id 由方案契約的 keysym 反查,
  # 不寫死 —— 三份佈局的 id 各不相同(k_mno / k6 …)。
  KEY_M="$(grep -m1 -oE '\{ *id: *"[^"]+".*keysym: *"M" *\}' "$SRC_LAYOUT" | sed -n 's/.*id: *"\([^"]*\)".*/\1/p')"
  KEY_G="$(grep -m1 -oE '\{ *id: *"[^"]+".*keysym: *"G" *\}' "$SRC_LAYOUT" | sed -n 's/.*id: *"\([^"]*\)".*/\1/p')"
  KEY_A="$(grep -m1 -oE '\{ *id: *"[^"]+".*keysym: *"A" *\}' "$SRC_LAYOUT" | sed -n 's/.*id: *"\([^"]*\)".*/\1/p')"
  if [ -z "$KEY_M" ] || [ -z "$KEY_G" ] || [ -z "$KEY_A" ]; then
    fail "$LAYOUT:找不到送 M/G/A 的鍵(M=${KEY_M:-?} G=${KEY_G:-?} A=${KEY_A:-?})"
    continue
  fi

  GRID_H="$(python3 -c "import json,sys;print(json.load(open(sys.argv[1]))['grid_height_px'])" "$LOUT/keymap.json")"
  read_frame || { fail "$LAYOUT:讀不到 IME 視窗 frame"; continue; }

  tap_key() {
    # 每一次點擊前都重讀 frame:打第一個字之後上方橫排就出現了,整個格線區
    # 會跟著移動,拿舊座標點下去會落在隔壁列(或兩鍵之間的縫裡)。
    read_frame || return 1
    local kid="$1" xy
    xy="$(python3 "$ROOT/scripts/layout_geom.py" --root "$ROOT" --layout "$LAYOUT" --theme "$THEME" \
          --screen "$WM_SIZE" --density "$WM_DENS" --key "$kid" --grid-top "$GRID_TOP" 2>/dev/null)"
    [ -n "$xy" ] || return 1
    # shellcheck disable=SC2086
    adbs shell input tap $xy >/dev/null 2>&1
    sleep 0.8
  }
  shot() { adbs shell screencap -p "/sdcard/syl.png" >/dev/null 2>&1; adbs pull /sdcard/syl.png "$1" >/dev/null 2>&1; }

  # ⚠ 打字**之前**先拍一張。消歧欄該出現的那幾格,是「打字前後真的變了」的
  #   那幾格 —— 用變化定位比用幾何模型定位可靠得多:
  #   · 幾何模型實測在 cn-t9-pinyin-numrow 上與渲染器差了約 75px;
  #   · 而整條左欄從格線頂端裁到底,會把數字列與 `!@#` 那顆鍵一起裁進來,
  #     OCR 於是讀出「ee ni mi lot」「ne mi o」這種夾雜垃圾的字串 ——
  #     CI 上就是這樣紅的,而畫面上明明白白寫著 ni 與 mi。
  #   而且這樣一來,斷言從「這一塊讀得到 ni」變成
  #   「**打字後才出現**的那幾格讀得到 ni」,比原本更嚴。
  # ⚠ 底圖要在畫面靜止之後才拍。鍵盤是滑上來的,拍太早的話「打字前後的差異」
  #   會變成整片都在動 —— 實測那一次:整條左欄被併成**一個**橫帶,
  #   點擊座標算成整欄的中點(y=2709,第三列的空格位),於是那一下把組字取消掉,
  #   第二關再比就變成「和底圖一模一樣」,0 個橫帶。
  #   徵狀看起來像功能壞了,其實是底圖拍早了。
  wait_stable() {
    local a="$LOUT/.s1.png" b="$LOUT/.s2.png" i
    for i in $(seq 1 12); do
      shot "$a"; sleep 0.4; shot "$b"
      if python3 - "$a" "$b" <<'PYS'
import sys
from PIL import Image, ImageChops
try:
    a = Image.open(sys.argv[1]).convert("L")
    b = Image.open(sys.argv[2]).convert("L")
except Exception:
    sys.exit(1)
if a.size != b.size:
    sys.exit(1)
bbox = ImageChops.difference(a, b).point(lambda p: 255 if p > 24 else 0).getbbox()
# 狀態列的時鐘一直在變,所以允許極小面積的差異。
if bbox is None:
    sys.exit(0)
area = (bbox[2] - bbox[0]) * (bbox[3] - bbox[1])
sys.exit(0 if area < a.width * a.height // 200 else 1)
PYS
      then
        rm -f "$a" "$b"
        return 0
      fi
      sleep 0.6
    done
    rm -f "$a" "$b"
    return 1
  }
  wait_stable || { fail "$LAYOUT:畫面一直在動,拍不到穩定的底圖"; continue; }
  shot "$LOUT/0-idle.png"

  for k in "$KEY_M" "$KEY_G" "$KEY_G" "$KEY_A" "$KEY_M"; do
    tap_key "$k" || { fail "$LAYOUT:點不到鍵 $k"; continue 2; }
  done

  # ── 消歧欄該在哪一塊 ────────────────────────────────────────────
  # 佈局的預設層宣告了 syllable_slots → 左側直欄;否則 → 候選列上方一橫排
  # (這正是退化規則(一),4 欄舊版走的就是這條)。
  SLOT_LINE="$(grep -m1 -E '^    syllable_slots:' "$SRC_LAYOUT" || true)"
  if [ "$PLANT" = "bad-slot-ids" ] && [ -n "$SLOT_LINE" ]; then
    SLOT_LINE='    syllable_slots: ["nope_1", "nope_2", "nope_3"]'
  fi

  # 裁切 + OCR。⚠ 一定要避開候選列(理由見 read_frame)。
  ocr_region() {
    local png="$1" outtxt="$2"
    python3 - "$png" "$outtxt" "$LOUT/keymap.json" "$GRID_TOP" "$FRAME_TOP" "$WM_DENS" \
             "$SLOT_LINE" "$TESSERACT" "$TESSDATA" "$LOUT/0-idle.png" <<'PY'
import json, subprocess, sys, os, re
from PIL import Image, ImageOps
png, outtxt, keymap, grid_top, frame_top, dens, slot_line, tess, tessdata, idle = sys.argv[1:11]
grid_top, frame_top, dens = int(grid_top), int(frame_top), int(dens)
im = Image.open(png).convert("L")
base = Image.open(idle).convert("L") if os.path.isfile(idle) else None
if base is not None and base.size != im.size:
    base = None          # 尺寸變了(旋轉?)就別比,寧可退回舊做法也不要比錯
env = dict(os.environ)
if tessdata:
    env["TESSDATA_PREFIX"] = tessdata

def ocr(img, tag, psms):
    """一格(或一排)上印的字母。認不出來回空字串。

    ⚠ **先裁到墨跡再放大。** 一顆鍵是 227x170 px,而上面只有 `ni` 兩個字母置中;
    直接整格丟給 tesseract,它看到的是一大片空白加中間一點點字,psm 7 幾乎讀不出
    東西(實測讀成 `a` / `pe`)。裁到墨跡的外框、再統一放大到固定字高,
    辨識率才穩定。
    """
    work = ImageOps.autocontrast(img, cutoff=1)
    # ⚠ 門檻要**相對**,不能寫死。左欄是淺灰底深灰字,上方橫排是白底淺灰字 ——
    #   寫死 110 的那一版把上方橫排整個判成空白(實測:t9-pinyin 讀出空字串,
    #   而同一張圖在沒有裁墨跡時是讀得到 ni mi 的)。
    lo, hi = work.getextrema()
    if hi - lo < 12:
        return ""            # 整塊是同一個顏色 = 沒有字
    thr = lo + (hi - lo) * 0.45
    mask = work.point(lambda p: 255 if p <= thr else 0)
    bb = mask.getbbox()
    if bb is None:
        return ""            # 空格位(spacer),本來就沒有字
    pad = 10
    work = work.crop((max(0, bb[0] - pad), max(0, bb[1] - pad),
                      min(work.width, bb[2] + pad), min(work.height, bb[3] + pad)))
    if work.height < 4 or work.width < 4:
        return ""
    # 字高拉到 ~160px 再送。80px 那一版把 cn-t9-pinyin 的 `ni` 讀成 `ne` ——
    # 那一格是淺灰底上的深灰字,`i` 的點在低解析度下會併進字身。
    scale = max(1, int(round(160.0 / work.height)))
    work = work.resize((work.width * scale, work.height * scale), Image.LANCZOS)
    work = ImageOps.expand(work, border=30, fill=255)
    work.save(outtxt + "." + tag + ".png")
    # psm 8 = 單一詞(左欄一格就是一個詞),psm 7 = 單行(上方橫排一行三個詞)。
    # ⚠ 順序由呼叫端決定:對上方橫排先用 psm 8 的話,它只會吐出一個詞,
    #   `mi` 就不見了 —— 而斷言要的是「ni 與 mi 都在」。
    #
    # ⚠ **不要跨 psm 取聯集。** 在真的 band 圖上掃過一輪(12 張、4 個 psm、
    #   oem 與 whitelist 各兩種)結果很乾脆:
    #     · 一格一個詞的直欄 → psm 8 / 13 全對,psm 6 / 7 一律吐 `al` `aal`
    #     · 一行多個詞的橫排 → psm 6 / 7 全對,psm 8 / 13 黏成 `nimi`
    #   whitelist 與 oem 完全不影響。所以正確的 psm 只有一個,聯集只會把
    #   另一個 psm 的垃圾一起收進來(實測:`ni al mi aal`)。
    #   呼叫端給的第一個就是對的那一個,後面的只在前面讀不出東西時才試。
    for psm in psms:
        r = subprocess.run(
            [tess, outtxt + "." + tag + ".png", "stdout", "--psm", psm,
             "-c", "tessedit_char_whitelist=abcdefghijklmnopqrstuvwxyz",
             # 關掉字典:它會把 `ni` 修成英文字。我們要的是鍵面上印的那幾個字母。
             "-c", "load_system_dawg=0", "-c", "load_freq_dawg=0"],
            capture_output=True, text=True, env=env)
        t = re.sub(r"[^a-z]+", " ", r.stdout.lower()).strip()
        if t:
            return t
    return ""

ids = re.findall(r'"([^"]+)"', slot_line) if slot_line.strip() else []
tokens, boxes = [], []
rects = []
if ids:
    keys = {k["id"]: k for k in json.load(open(keymap))["keys"] if k.get("id")}
    rects = [keys[i] for i in ids if i in keys]

tap_xy = ""
if ids and not rects:
    # ⚠ 佈局宣告了格位,但那些 id 在這一層裡不存在(有人把 pu_comma 改名了)。
    #   **絕不可以退回去裁「候選列上方那一條」** —— 那時候上方橫排根本沒有渲染,
    #   裁到的會是**候選列本身**,而候選列的 comment 印著「ni hao」,
    #   OCR 讀得到 ni,關卡就永遠是綠的。這正是這支腳本要擋的那種假綠燈。
    open(outtxt, "w").write("")
    open(outtxt + ".tap", "w").write("")
    print("BOX=[] TEXT= TAP=- ERROR=宣告的格位 id 在這一層找不到:%s" % ",".join(ids))
    sys.exit(0)

if rects:
    # 左側直欄。
    #
    # ⚠ **不照 layout_geom 的每一格去裁。** 實測 `cn-t9-pinyin-numrow` 的模型把
    #   `pu_comma` 放在比實際渲染低 ~75px 的位置(同一份模型對 `k_mno` 卻是準的),
    #   於是三個框全部裁在格與格之間,OCR 讀出「re i ry」而畫面上明明寫著 ni / mi。
    #   幾何模型與渲染器對不上時,**要相信畫面**。
    #
    # 作法:取整條左欄(x 用模型的欄寬,y 從格線區頂端到視窗底端 —— 這樣一定
    # 涵蓋所有列,而且**絕不會碰到候選列**,候選列在格線區之上,它的 comment
    # 印著「ni hao」,裁進來就是永遠綠的假關卡)。再依「有墨跡的橫帶」把欄切成
    # 一格一格,每一帶就是一個讀音。
    x0 = min(r["x"] for r in rects); x1 = max(r["x"] + r["w"] for r in rects)
    # ⚠ **格線頂端要往上讓一點,否則 `i` 的那一點會被切掉。**
    #   CI 上實際發生過:`grid_top = FRAME_BOT - GRID_H` 算出來的線正好壓在
    #   `ni` 的字身上緣,那一點落在線的上面 —— 裁出來的圖是 `nl`,
    #   tesseract 讀成 `ne`,而畫面完全正確。(本機模擬器解析度不同,
    #   那一點剛好在線下面,所以本機一直是綠的 —— 又一個「換台機器才發作」。)
    #
    #   讓多少要有上限:候選列就在格線區正上方,而它的 comment 印著「ni hao」——
    #   讓過頭就會裁到候選列,那是這支腳本從第一天就在防的假綠燈。
    #   兩個上限取小的:格線區高度的 6%,以及「視窗頂端到格線頂端」的 25%
    #   (候選列的文字在它自己那一條的中間,25% 連它的下緣留白都碰不到)。
    lift = min(int(0.06 * max(0, im.height - grid_top)),
               int(0.25 * max(0, grid_top - frame_top)))
    col = (max(0, x0), max(0, grid_top - lift), min(im.width, x1), im.height)
    boxes.append(col)
    strip = im.crop(col)
    if base is not None:
        # 「有沒有變」比「有沒有墨跡」精準:數字列與 `!@#` 那顆鍵一直都在,
        # 但它們不會因為打了字而改變,於是自然被排除。
        b = base.crop(col)
        sp, bp = strip.load(), b.load()
        rowink = [
            sum(1 for x in range(strip.width) if abs(sp[x, y] - bp[x, y]) > 24)
            for y in range(strip.height)
        ]
        # 少量雜訊(抗鋸齒、游標)不算變化。
        floor = max(2, strip.width // 20)
        rowink = [n if n >= floor else 0 for n in rowink]
        w = strip
    else:
        w = ImageOps.autocontrast(strip, cutoff=1)
        lo, hi = w.getextrema()
        thr = lo + (hi - lo) * 0.45 if hi > lo else 0
        px = w.load()
        rowink = [sum(1 for x in range(w.width) if px[x, y] <= thr) for y in range(w.height)]
    # 先切出所有的墨跡段(不論多短),之後再合併、再篩。
    runs, cur = [], None
    for y, n in enumerate(rowink):
        if n > 0 and cur is None:
            cur = y
        elif n == 0 and cur is not None:
            runs.append((cur, y))
            cur = None
    if cur is not None:
        runs.append((cur, w.height))

    # ⚠ **`i` 的那一點是獨立的一段。** 直接用「長度 >= 8」篩,那一點會被丟掉,
    #   而裁出來的圖就變成 `nl` —— CI 上實際發生過:tesseract 讀成 `ne`,
    #   看起來像功能壞了,其實是這裡把字裁壞了。(本機的模擬器解析度不同,
    #   那一點剛好併進來了,所以本機一直是綠的。)
    #   所以:間距小於「較高那一段的 60%」就當成同一個字併起來。
    merged = []
    for r in runs:
        if merged:
            gap = r[0] - merged[-1][1]
            tall = max(r[1] - r[0], merged[-1][1] - merged[-1][0])
            if gap <= max(6, int(tall * 0.6)):
                merged[-1] = (merged[-1][0], r[1])
                continue
        merged.append(r)
    bands = [r for r in merged if r[1] - r[0] >= 8]
    # 一個橫帶佔掉半條欄以上,那不是一個格位 —— 幾乎一定是底圖拍在畫面還在動的
    # 時候。這種情況要**指名**,不能讓它變成「消歧欄上讀不到 ni/mi」——
    # 那句話會把人送去查一個沒壞的功能。
    if bands and max(y1 - y0 for y0, y1 in bands) > w.height * 0.5:
        open(outtxt, "w").write("")
        open(outtxt + ".tap", "w").write("")
        print("BOX=%s TEXT= TAP=- ERROR=左欄整片都在變,底圖不可信(鍵盤可能還在動)"
              % (boxes,))
        sys.exit(0)
    for n, (by0, by1) in enumerate(bands):
        pad = 8
        b = (0, max(0, by0 - pad), w.width, min(w.height, by1 + pad))
        tok = ocr(strip.crop(b), "band%d" % n, ("8", "13"))
        tokens.append(tok)
        # 點的是**我們剛剛在畫面上讀到的那一格**,不是模型算出來的座標。
        if tok and "ni" in tok.split() and not tap_xy:
            tap_xy = "%d %d" % ((col[0] + col[2]) // 2, col[1] + (by0 + by1) // 2)
    if not tap_xy and bands:
        by0, by1 = bands[0]
        tap_xy = "%d %d" % ((col[0] + col[2]) // 2, col[1] + (by0 + by1) // 2)
else:
    # 上方橫排:貼著 IME 視窗頂端,高度 = 主題的 height(預設 40dp)。
    h = int(round(40 * dens / 160.0))
    b = (0, frame_top, im.width, min(im.height, frame_top + h))
    boxes.append(b)
    if base is not None:
        d = im.crop(b)
        e = base.crop(b)
        dp, ep = d.load(), e.load()
        changed = sum(
            1
            for y in range(d.height)
            for x in range(0, d.width, 4)
            if abs(dp[x, y] - ep[x, y]) > 24
        )
        # 門檻是量出來的,不是猜的:實測 t9-pinyin 這一條在打字前後有 172 個
        # 取樣點變了("ni mi" 兩個小字),而完全沒變的區域是 0。所以這裡要的是
        # 一個「有沒有」的下限,不是「變了多少」的比例 —— 比例那一版訂在 226,
        # 把一條**畫對了**的消歧欄判成沒出現。
        if changed < 24:
            # 這一條在打字前後一模一樣 —— 它不是消歧欄,是別的東西。
            open(outtxt, "w").write("")
            open(outtxt + ".tap", "w").write("")
            print("BOX=%s TEXT= TAP=- ERROR=候選列上方那一條在打字前後沒有變化" % (boxes,))
            sys.exit(0)
    tokens.append(ocr(im.crop(b), "row", ("7", "6")))

text = " ".join(t for t in tokens if t).strip()
open(outtxt, "w").write(text)
open(outtxt + ".tap", "w").write(tap_xy)
print("BOX=%s TEXT=%s TAP=%s" % (boxes, text, tap_xy or "-"))
PY
  }

  # ── 第 2 關:第一個音節 ─────────────────────────────────────────
  read_frame || { fail "$LAYOUT:讀不到 IME 視窗 frame"; continue; }
  shot "$LOUT/1-typed.png"
  OCR1="$(ocr_region "$LOUT/1-typed.png" "$LOUT/1-typed.txt" 2>&1)"; RC1=$?
  info "$OCR1"
  # 裁切/OCR 自己壞掉 ≠ 畫面上沒有那幾個字。兩者報成同一句話的話,
  # 下一個人會去查一個根本沒壞的功能(這正是這一輪發生的事)。
  if [ "$RC1" -ne 0 ]; then
    fail "$LAYOUT:裁切/OCR 這一步自己失敗了(exit $RC1),不是畫面的問題:$OCR1"
    continue
  fi
  T1="$(cat "$LOUT/1-typed.txt" 2>/dev/null || echo)"
  if echo "$T1" | grep -qw ni && echo "$T1" | grep -qw mi; then
    pass "$LAYOUT:畫面上讀得到第一個音節 ni 與 mi(\"$T1\")"
  else
    fail "$LAYOUT:消歧欄上讀不到 ni/mi。OCR 讀到的是「$T1」。截圖 $LOUT/1-typed.png"
    continue
  fi

  # ── 點下 ni ─────────────────────────────────────────────────────
  if [ -n "$SLOT_LINE" ] && [ "$PLANT" != "bad-slot-ids" ]; then
    # 點「畫面上讀到 ni 的那一格」,座標由上一步的 OCR 一起算出來。
    # 用模型座標點的那一版,在 numrow 上會落在兩格之間 —— 而畫面是對的。
    TAP_XY="$(cat "$LOUT/1-typed.txt.tap" 2>/dev/null || echo)"
    if [ -z "$TAP_XY" ]; then fail "$LAYOUT:算不出消歧格的座標"; continue; fi
    # shellcheck disable=SC2086
    adbs shell input tap $TAP_XY >/dev/null 2>&1
    sleep 1
  else
    # 上方橫排的第一個 chip:左邊 12dp padding,再往右一點點。
    read_frame || { fail "$LAYOUT:讀不到 IME 視窗 frame"; continue; }
    PADPX=$(python3 -c "print(int(round(12*$WM_DENS/160.0)))")
    ROWH=$(python3 -c "print(int(round(40*$WM_DENS/160.0)))")
    adbs shell input tap $((PADPX + 20)) $((FRAME_TOP + ROWH / 2)) >/dev/null 2>&1
    sleep 1
  fi
  sleep 1

  # ── 第 3 關:第二個音節 ─────────────────────────────────────────
  read_frame || { fail "$LAYOUT:讀不到 IME 視窗 frame"; continue; }
  shot "$LOUT/2-picked.png"
  OCR2="$(ocr_region "$LOUT/2-picked.png" "$LOUT/2-picked.txt" 2>&1)"; RC2=$?
  info "$OCR2"
  if [ "$RC2" -ne 0 ]; then
    fail "$LAYOUT:裁切/OCR 這一步自己失敗了(exit $RC2),不是畫面的問題:$OCR2"
    continue
  fi
  T2="$(cat "$LOUT/2-picked.txt" 2>/dev/null || echo)"
  if echo "$T2" | grep -qwE 'hao|gan|gao'; then
    pass "$LAYOUT:選了 ni 之後,畫面上換成第二個音節(\"$T2\")"
  else
    fail "$LAYOUT:選了 ni 之後讀不到第二個音節(hao/gan/gao)。OCR:「$T2」。截圖 $LOUT/2-picked.png"
  fi
done

finish "${#T9_LAYOUTS[@]} 份九宮格佈局上都畫出來了,而且選得下去(斷言的是畫面像素)"
