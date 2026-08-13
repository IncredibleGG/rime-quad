#!/usr/bin/env bash
#
# verify_input_matrix.sh — 對「輸入框型別矩陣」逐鍵驗證輸入法
#
# 為什麼要有這支
# ─────────────────────────────────────────────────────────────────────────────
# verify_ime.sh 打的是 `adb shell input text`(走實體鍵盤路徑)並且只對一個
# 最單純的 EditText。真機使用者回報的輸入 bug 之所以漏掉,就是因為:
#   1. 沒有測**軟鍵盤逐鍵點擊**這條路徑(SendSpec → rs_process_key);
#   2. 沒有測**不同 inputType**(Telegram 的訊息框是
#      textCapSentences|textMultiLine,不是 verify_ime.sh 用的裸 EditText);
#   3. 沒有測**組字中**被打斷(換框、旋轉、離開、session 失效)。
# 這支把這三件事變成可重跑的迴歸測試。
#
# 用法
#   ./scripts/verify_input_matrix.sh                      # 全矩陣 + 全情境
#   ./scripts/verify_input_matrix.sh --fields text,number
#   ./scripts/verify_input_matrix.sh --no-scenarios
#   RIME_SERIAL=emulator-5556 ./scripts/verify_input_matrix.sh
#
# 選項
#   --serial <s>    指定裝置(沒指定而且不只一台在線就中止)
#   --fields <csv>  只跑這些欄位(預設全部)
#   --keys <str>    要逐鍵點的字母序列(預設 nihao)
#   --expect <csv>  每一步的預期內容,逗號分隔;預設 n,ni,ni h,ni ha,ni hao
#                   (朗月拼音的組字串)。給 '-' 表示該步不檢查。
#   --no-scenarios  只跑矩陣,跳過組字中斷情境
#   --no-install    不重新建置/安裝測試靶
#   --out <dir>     artifact 目錄(預設 <專案>/build/inputmatrix/run)
#
# 離開碼:0 全綠;1 有格子沒過。
#
# 名詞:一「格」= (欄位, 第幾個按鍵)。格子的判定:
#   OK       內容等於預期
#   NOCHANGE 這一鍵之後內容完全沒變 →「輸入沒反應」
#   SHRANK   這一鍵之後內容變短了   →「輸入一個字符他刪除一個字符」
#   DIFF     有變、但不等於預期(可能是方案不同,不一定是 bug)

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Android/Sdk}}"
ADB="$ANDROID_SDK_ROOT/platform-tools/adb"
SERIAL=""

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
IME_ID="${RIME_IME_ID:-$RS_ANDROID_IME_ID}"
IME_PKG="${IME_ID%%/*}"
PKG="dev.rime.inputmatrix"
ACT="$PKG/.MainActivity"
TAG="IMEMATRIX"

ALL_FIELDS="text textMultiLine textCapSentencesMultiLine textNoSuggestions textUri textEmailAddress textPassword textVisiblePassword number webview"
FIELDS="$ALL_FIELDS"
KEYS="nihao"
EXPECT_CSV="n,ni,ni h,ni ha,ni hao"
EXPECT_OVERRIDE=0
RUN_SCENARIOS=1
DO_INSTALL=1
OUT_DIR="$PROJECT_ROOT/build/inputmatrix/run"

while [ $# -gt 0 ]; do
  case "$1" in
    --serial) SERIAL="$2"; shift 2 ;;
    --fields) FIELDS="$(echo "$2" | tr ',' ' ')"; shift 2 ;;
    --keys)   KEYS="$2"; shift 2 ;;
    --expect) EXPECT_CSV="$2"; EXPECT_OVERRIDE=1; shift 2 ;;
    --no-scenarios) RUN_SCENARIOS=0; shift ;;
    --no-install)   DO_INSTALL=0; shift ;;
    --out)    OUT_DIR="$2"; shift 2 ;;
    -h|--help) sed -n '2,40p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "未知選項:$1" >&2; exit 2 ;;
  esac
done

# ⛔ `--serial` 也要算「指名」。閘從前只看環境變數,於是這一行帶 `--serial`
#   就必死(RC=2,訊息說「是自動選來的」而那台正是命令列指名的)。
#   `rs_select_device` 把來源(flag / env / auto)一起記下來給閘看。
rs_select_device "$ADB" "$SERIAL" || exit 2
SERIAL="$RS_SERIAL"
adbs() { "$ADB" -s "$SERIAL" "$@"; }
rs_assert_destructive_ok "$ADB" "$SERIAL" "ime set、settings put system user_rotation" || exit 2

mkdir -p "$OUT_DIR"
RESULTS="$OUT_DIR/results.tsv"
: > "$RESULTS"
FAILURES=0
SCEN_LOG="$OUT_DIR/scenarios.txt"
: > "$SCEN_LOG"

# ───────────────────────────────────────────────── 鍵盤座標 ─────────────────
#
# 軟鍵盤是自繪的 Compose 畫面,沒有 accessibility 節點可以定位,只能算座標。
# 佈局是 10 單位寬的 qwerty(core/layouts/qwerty.yaml),所以 x 可由螢幕寬度
# 直接推出來;y 則量自鍵盤底部,四排等距。量測基準:1440x3120 / density 505
# (三星 S24U 的幾何)。換幾何時用 RIME_KB_ROW_BOTTOM / RIME_KB_ROW_STEP 覆寫。
SIZE="$(adbs shell wm size | tr -d '\r' | tail -1 | sed 's/.*: //')"
SCREEN_W="${SIZE%x*}"
SCREEN_H="${SIZE#*x}"
UNIT=$((SCREEN_W / 10))
CALIB_FILE="$PROJECT_ROOT/build/inputmatrix/calibration.txt"
# 先給一組佔位值,真正的值由 ensure_calibrated 決定。
ROW_BOTTOM="${RIME_KB_ROW_BOTTOM:-$((SCREEN_H - 92))}"
ROW_STEP="${RIME_KB_ROW_STEP:-185}"

row_y() { echo $((ROW_BOTTOM - ROW_STEP * $1)); }   # 0=最下排, 3=qwerty 排

# 回傳 "<x> <y>";只支援矩陣測試會用到的字母鍵與 backspace。
key_xy() {
  local k="$1" i
  case "$k" in
    q) i=0;; w) i=1;; e) i=2;; r) i=3;; t) i=4;;
    y) i=5;; u) i=6;; i) i=7;; o) i=8;; p) i=9;;
  esac
  case "$k" in
    q|w|e|r|t|y|u|i|o|p) echo "$((UNIT / 2 + UNIT * i)) $(row_y 3)"; return;;
  esac
  case "$k" in
    a) i=0;; s) i=1;; d) i=2;; f) i=3;; g) i=4;;
    h) i=5;; j) i=6;; k) i=7;; l) i=8;;
  esac
  case "$k" in
    a|s|d|f|g|h|j|k|l) echo "$((UNIT + UNIT * i)) $(row_y 2)"; return;;
  esac
  case "$k" in
    z) i=0;; x) i=1;; c) i=2;; v) i=3;; b) i=4;; n) i=5;; m) i=6;;
  esac
  case "$k" in
    z|x|c|v|b|n|m) echo "$((UNIT * 2 + UNIT * i)) $(row_y 1)"; return;;
  esac
  case "$k" in
    BACKSPACE) echo "$((UNIT * 9 + UNIT / 4)) $(row_y 1)"; return;;
    SHIFT)     echo "$((UNIT * 3 / 4)) $(row_y 1)"; return;;
    SPACE)     echo "$((UNIT * 5)) $(row_y 0)"; return;;
    COMMA)     echo "$((UNIT * 15 / 2)) $(row_y 0)"; return;;
    SYMBOLS)   echo "$((UNIT * 3 / 4)) $(row_y 0)"; return;;
    # numeric-symbol 佈局:「#+=」在數字層第三排最左,「€」在符號層第二排第六個。
    TO_SYMBOL) echo "$((UNIT * 3 / 4)) $(row_y 1)"; return;;
    EURO)      echo "$((UNIT / 2 + UNIT * 5)) $(row_y 2)"; return;;
  esac
  echo "0 0"
}

tap_key() { local xy; xy="$(key_xy "$1")"; adbs shell input tap $xy >/dev/null 2>&1; }

# numeric-symbol 佈局的數字排,和 qwerty 第一排同一個位置(10 單位、同一列 y)。
# 1..9 依序,0 在最右邊。
tap_digit() {
  local d="$1" i
  if [ "$d" = "0" ]; then i=9; else i=$((d - 1)); fi
  adbs shell input tap $((UNIT / 2 + UNIT * i)) $(row_y 3) >/dev/null 2>&1
}

# ─────────────────────────────────────────────── 鍵盤座標校準 ───────────────
#
# 為什麼不能寫死座標:鍵盤高度是活的。主題、使用者偏好的「鍵盤高度」、以及
# 正在進行中的自適應高度修正,都會把每一排的 y 挪動幾十 px。第一版把
# 1440x3120 量到的值寫死,結果換了一版 APK 之後整份矩陣一起變紅 —— 不是
# 輸入法壞了,是測試腳本在戳空氣。那種測試比沒有測試更糟,因為它會說謊。
#
# 所以改成開跑前先量一次:沿著 x=7.5 單位這一欄由下往上逐點戳,記錄每一戳
# 打出什麼字。同一個字連續出現的那一段,就是那一排的縱向範圍,取中點即為
# 該排中心 —— **不必知道那顆鍵是什麼字**,只要「連續幾戳打出同一個字」就
# 夠了。這一點很重要:第一版拿 n/j/i 這些字母去認排,結果上一輪測試把鍵盤
# 留在符號佈局上,認不出來就整個掛掉。改成認「band」之後,鍵盤停在哪一層
# 都量得準。
#
# 量測在 **textVisiblePassword** 上做,理由是密碼框不經過 librime
# (見 RimeInputMethodService.shouldBypassRime),每一戳就是老老實實地
# 附加一個字面字元 —— 不會被組字、選字、自動上屏攪亂,解讀起來沒有歧義。
#
# 結果快取在 build/inputmatrix/calibration.txt。每次開跑先用一次探針驗證
# 快取還準不準,不準才重量,所以平常的重跑不會付這個成本。

CALIB_X=$((UNIT * 15 / 2))

calibrate() {
  echo "校準鍵盤座標(沿 x=$CALIB_X 由下往上掃描)…"
  launch_field textVisiblePassword
  adbs logcat -c >/dev/null 2>&1
  local scan="$OUT_DIR/calib-scan.txt"
  : > "$scan"
  local y prev="" cur app
  y=$((SCREEN_H - 130))
  while [ "$y" -gt $((SCREEN_H - 1250)) ]; do
    adbs shell input tap "$CALIB_X" "$y" >/dev/null 2>&1
    sleep 0.55
    cur="$(state_text "$(last_state)")"
    app="${cur#"$prev"}"
    printf '%s\t%s\n' "$y" "$app" >> "$scan"
    prev="$cur"
    y=$((y - 25))
  done
  python3 - "$scan" > "$CALIB_FILE" <<'CALIB'
import sys, collections
rows = []
for line in open(sys.argv[1], encoding="utf-8"):
    y, _, ch = line.rstrip("\n").partition("\t")
    if len(ch) == 1:
        rows.append((int(y), ch))
# 把「同一個字連續出現」的段落收起來,段落中點就是那一排的中心 y。
bands = []
for y, ch in rows:
    if bands and bands[-1][0] == ch:
        bands[-1][1].append(y)
    else:
        bands.append((ch, [y]))
# 只取夠寬的段落(至少 3 戳),免得把邊界上的雜訊當成一排。
solid = [(ch, ys) for ch, ys in bands if len(ys) >= 3]
centers = sorted(((max(ys) + min(ys)) // 2 for _, ys in solid), reverse=True)
if len(centers) < 4:
    sys.stderr.write("校準失敗:只認出 %d 排,掃描結果 %r\n" % (len(centers), bands))
    sys.exit(1)
centers = centers[:4]                      # 由下往上四排
gaps = [centers[i] - centers[i + 1] for i in range(3)]
step = sum(gaps) // len(gaps)
# 排距應該是均勻的;差太多代表掃描被彈出視窗之類的東西干擾了。
if max(gaps) - min(gaps) > step // 3:
    sys.stderr.write("校準失敗:排距不均勻 %r\n" % (gaps,))
    sys.exit(1)
print("ROW_BOTTOM=%d" % centers[0])
print("ROW_STEP=%d" % step)
CALIB
}

# 刻意**不快取**:鍵盤高度會隨主題、使用者偏好與正在演進的自適應高度而變,
# 沿用上一次的量測值只會讓腳本對著空氣戳,還理直氣壯地報綠燈。量一次約一分鐘,
# 相對於整份矩陣是可以接受的成本。真的想省,用 RIME_KB_ROW_BOTTOM/STEP 覆寫。
ensure_calibrated() {
  if [ -n "${RIME_KB_ROW_BOTTOM:-}" ] && [ -n "${RIME_KB_ROW_STEP:-}" ]; then
    echo "鍵盤座標由環境變數指定:ROW_BOTTOM=$ROW_BOTTOM ROW_STEP=$ROW_STEP"
    return 0
  fi
  calibrate || { echo "鍵盤座標校準失敗,詳見 $OUT_DIR/calib-scan.txt" >&2; return 1; }
  . "$CALIB_FILE"
  echo "校準完成:ROW_BOTTOM=$ROW_BOTTOM ROW_STEP=$ROW_STEP"
}

# ───────────────────────────────────────────────── 讀回狀態 ─────────────────

# 從 logcat 取最後一行 STATE。用 logcat 而不是 uiautomator dump 的理由有二:
#   · 快(uiautomator dump 每次要一兩秒,矩陣有數十格);
#   · **密碼欄不會被遮蔽** —— uiautomator 讀回來是圓點,logcat 是真值。
# uiautomator 仍在每個欄位結尾做一次交叉佐證(見 cross_check)。
last_state() {
  adbs logcat -d -s "$TAG:I" 2>/dev/null | tr -d '\r' \
    | grep -F "$TAG: STATE " | tail -1 | sed 's/.*STATE //'
}
state_text() { echo "$1" | sed -n 's/^[^ ]* |\(.*\)| cs=.*/\1/p'; }
state_cs()   { echo "$1" | sed -n 's/.*cs=\(-\?[0-9]*\) .*/\1/p'; }

# librime 到底活著沒有?
#
# 判準不能只看「字有沒有進去」:session 失效時 fallbackKey() 也會把字面字元
# commit 進去,看起來一模一樣。真正的分水嶺是**組字區**:
#   · 健康  → setComposingText(),編輯框有組字範圍,cs >= 0
#   · 失效  → commitText(),沒有組字範圍,cs = -1
# 這也正是「打字看起來有反應、其實引擎是死的」這種假陽性的唯一可靠識別法。
rime_composing_works() {
  launch_field text
  adbs logcat -c >/dev/null 2>&1
  tap_key n; sleep 0.9
  tap_key i; sleep 0.9
  local st; st="$(last_state)"
  [ "$(state_text "$st")" = "ni" ] && [ "$(state_cs "$st")" = "0" ]
}

# 只戳一下 n 的位置,用來分辨三種狀況(見 ensure_alpha_layout / wait_for_rime_ready):
#   "n" + cs>=0  → 英數佈局 + librime 活著
#   "n" + cs=-1  → 佈局對,但 librime 沒就緒(走了 fallbackKey 直接上屏)
#   其他         → 鍵盤根本不在英數佈局上(上一輪可能停在符號層)
probe_n() {
  launch_field text
  adbs logcat -c >/dev/null 2>&1
  tap_key n; sleep 0.9
  last_state
}

# 佈局狀態不隨編輯框重置:上一輪測試若停在 numeric-symbol,這一輪整份矩陣
# 會對著符號鍵盤戳字母。
#
# 一開始是用 `am force-stop "$IME_PKG"` 來重置的 —— **那是錯的**。
# force-stop 會把套件打成 Android 的 stopped 狀態,IMMS 之後就不會再自動
# 把它綁回來,輸入法整個消失,後續全部在沒有輸入法的情況下空跑。
# 改成按 numeric-symbol 佈局左下角的「ABC」鍵切回去 —— 那顆鍵和 qwerty 的
# 「?123」同一個位置,所以不必先知道現在在哪一層。
ensure_alpha_layout() {
  local i st
  for i in 1 2 3; do
    st="$(probe_n)"
    [ "$(state_text "$st")" = "n" ] && return 0
    echo "  鍵盤不在英數佈局上(戳 n 得到 '$(state_text "$st")'),按左下角切回去…"
    tap_key SYMBOLS
    sleep 1.2
  done
  echo "切不回英數佈局,請看 $OUT_DIR 下的截圖" >&2
  adbs exec-out screencap -p > "$OUT_DIR/fail-layout.png" 2>/dev/null || true
  return 1
}

# 部署(rs_init + 編譯詞庫)可能要一兩分鐘,期間 rs_process_key 一律回 false。
# 不等它就開跑,整份矩陣會在 fallback 路徑上跑完並且「全部有字」,
# 卻什麼都沒真的測到。
wait_for_rime_ready() {
  local i
  for i in $(seq 1 "${RIME_READY_TRIES:-12}"); do
    if rime_composing_works; then
      echo "librime 已就緒(第 $i 次探測)"
      return 0
    fi
    echo "  librime 尚未就緒(組字區還沒出現),等待中… ($i)"
    sleep 10
  done
  echo "等不到 librime 就緒:組字區始終沒有出現。可能是部署失敗,請看 logcat RimeIME。" >&2
  return 1
}

ic_calls_since() {
  adbs logcat -d -s "$TAG:I" 2>/dev/null | tr -d '\r' \
    | grep -F "$TAG: IC " | sed 's/.*IC //' | tail -n "${1:-40}"
}

cross_check() {
  adbs shell "uiautomator dump /sdcard/rime_matrix.xml >/dev/null 2>&1; cat /sdcard/rime_matrix.xml" \
    2>/dev/null | tr -d '\r' > "$OUT_DIR/ui-$1.xml"
  python3 - "$OUT_DIR/ui-$1.xml" <<'PY' 2>/dev/null || true
import sys, xml.etree.ElementTree as ET
try:
    root = ET.parse(sys.argv[1]).getroot()
except Exception:
    sys.exit(0)
for n in root.iter("node"):
    if n.get("content-desc") == "rime_matrix_mirror":
        print(n.get("text", ""))
        break
PY
}

# 讀回 rime_matrix_input 的中心座標(EditText 與 WebView 都適用)。
field_center() {
  adbs shell "uiautomator dump /sdcard/rime_bounds.xml >/dev/null 2>&1; cat /sdcard/rime_bounds.xml" \
    2>/dev/null | tr -d '\r' > "$OUT_DIR/bounds.xml"
  python3 - "$OUT_DIR/bounds.xml" <<'BOUNDS' 2>/dev/null
import sys, re, xml.etree.ElementTree as ET
try:
    root = ET.parse(sys.argv[1]).getroot()
except Exception:
    sys.exit(0)
for n in root.iter("node"):
    if n.get("content-desc") == "rime_matrix_input":
        m = re.match(r"\[(\d+),(\d+)\]\[(\d+),(\d+)\]", n.get("bounds", ""))
        if m:
            a, b, c, d = map(int, m.groups())
            print((a + c) // 2, (b + d) // 2)
        break
BOUNDS
}

launch_field() {
  local f="$1"; shift
  adbs shell am force-stop "$PKG" >/dev/null 2>&1
  adbs shell am start -n "$ACT" --es field "$f" "$@" >/dev/null 2>&1
  # 等 READY,最多 8 秒
  local i
  for i in $(seq 1 16); do
    log_has "READY $f" adbs logcat -d -s "$TAG:I" && break
    sleep 0.5
  done
  # 等軟鍵盤真的出現
  for i in $(seq 1 20); do
    log_has "mIsInputViewShown=true" adbs shell dumpsys input_method && break
    sleep 0.5
  done
  # 點一下輸入框本身,再靜置。兩個理由:
  #   · WebView 裡的 <input> 不點就拿不到焦點,也就不會建立 InputConnection;
  #   · 鍵盤剛彈出時視窗還在做進場動畫,這段期間打到鍵盤上的觸控會被系統
  #     丟掉 —— 矩陣第一版每一格的第一鍵都憑空消失,原因就在這裡,不是 IME
  #     漏收。等動畫做完再開始逐鍵點,結果才可重現。
  local xy
  xy="$(field_center)"
  [ -n "$xy" ] && adbs shell input tap $xy >/dev/null 2>&1
  sleep 1.5
}

# 每一格 / 每一個情境開跑前都確認一次 librime 還活著。
#
# 這不是防禦性寫法的過度發揮,而是本任務最重要的一課:**session 失效時,
# 按鍵會回落成 fallbackKey() 的字面上屏,畫面上「字有進去」,看起來一切正常。**
# 不主動檢查組字區,測試就會安安靜靜地把一整輪 fallback 路徑當成通過。
# 實測中輸入法確實會在長時間跑批的中途被系統回收,不擋掉的話後面幾十格
# 全是假訊號。
ensure_healthy() {
  rime_composing_works && return 0
  # ⚠ 先把兩件不同的事分清楚,否則會把「鍵盤停在符號層」誤診成「librime 死了」,
  #   然後對著一個其實好端端的輸入法反覆重綁、白等好幾分鐘。
  #   上一個情境若切過 ?123 / #+=,鍵盤就是停在那裡的。
  ensure_alpha_layout || return 1
  rime_composing_works && return 0
  echo "  [!] librime 不在健康狀態(組字區沒出現),嘗試重新綁定輸入法…"
  adbs shell ime enable "$IME_ID" >/dev/null 2>&1
  adbs shell ime set "$IME_ID" >/dev/null 2>&1
  sleep 2
  ensure_alpha_layout || return 1
  wait_for_rime_ready
}

# ───────────────────────────────────────────────── 主矩陣 ───────────────────

echo "裝置       : $SERIAL  ($SCREEN_W x $SCREEN_H)"
echo "輸入法     : $IME_ID"
echo "按鍵序列   : $KEYS"
echo "輸出       : $OUT_DIR"
echo

adbs shell ime enable "$IME_ID" >/dev/null 2>&1
adbs shell ime set "$IME_ID" >/dev/null 2>&1
CUR="$(adbs shell settings get secure default_input_method | tr -d '\r')"
[ "$CUR" = "$IME_ID" ] || { echo "無法把 $IME_ID 設為預設輸入法(目前 $CUR)" >&2; exit 2; }

if [ "$DO_INSTALL" -eq 1 ]; then
  APK="$("$SCRIPT_DIR/build_input_matrix_app.sh" 2>/dev/null | tail -1)"
  [ -f "$APK" ] || { echo "測試靶建置失敗" >&2; exit 2; }
  adbs install -r -g "$APK" >/dev/null 2>&1 || { echo "測試靶安裝失敗" >&2; exit 2; }
fi

# 不同型別的框，「正確」長得不一樣。把這件事寫死在腳本裡而不是靠人記，
# 才有資格叫迴歸測試。
#
#   一般文字框   走 librime，看到的是拼音組字串 "ni hao"
#   密碼框       **不該**走 librime（見 RimeInputMethodService.shouldBypassRime）：
#                密碼進了 speller 就會進使用者詞典。所以預期是逐字上屏 "nihao"
#   數字框       宿主的 KeyListener 會擋掉字母 —— 打字母**本來就該**沒有反應。
#                真正要驗的是「數字進得去」，見下面的『數字框收得下數字』情境。
expects_for() {
  if [ "$EXPECT_OVERRIDE" -eq 1 ]; then echo "$EXPECT_CSV"; return; fi
  case "$1" in
    textPassword|textVisiblePassword) echo "n,ni,nih,niha,nihao" ;;
    number) echo "<empty>,<empty>,<empty>,<empty>,<empty>" ;;
    *) echo "$EXPECT_CSV" ;;
  esac
}


# 順序是有講究的:
#   1. 校準只看「同一戳打出同一個字」,與佈局、與 librime 死活都無關,
#      所以擺第一,後面兩步才有可用的座標可以戳。
#   2. 回到英數佈局(要座標)。
#   3. 等 librime 真的就緒(要座標,也要英數佈局才判讀得了組字區)。
ensure_calibrated || exit 2
ensure_alpha_layout || exit 2
wait_for_rime_ready || exit 2
echo

printf '%-28s %-3s %-4s %-14s %-14s %s\n' 欄位 步 按鍵 實際 預期 判定
printf '%s\n' "--------------------------------------------------------------------------------------"

for FIELD in $FIELDS; do
  if ! ensure_healthy; then
    echo "librime 救不回來,中止(已完成的結果見 $RESULTS)" >&2
    exit 2
  fi
  IFS=',' read -r -a EXPECTS <<< "$(expects_for "$FIELD")"
  launch_field "$FIELD"
  adbs logcat -c >/dev/null 2>&1
  PREV=""
  STEP=0
  FIELD_BAD=0
  while [ "$STEP" -lt "${#KEYS}" ]; do
    K="${KEYS:$STEP:1}"
    tap_key "$K"
    sleep 0.9
    RAW="$(last_state)"
    ACTUAL="$(state_text "$RAW")"
    EXP="${EXPECTS[$STEP]:-}"
    [ "$EXP" = "<empty>" ] && EXP=""
    HAS_EXP=1
    [ -z "${EXPECTS[$STEP]:-}" ] && HAS_EXP=0
    if [ "$STEP" -eq 0 ]; then PREVLEN=0; else PREVLEN=${#PREV}; fi
    VERDICT="DIFF"
    if [ "$EXP" = "-" ] || [ "$HAS_EXP" -eq 0 ]; then
      VERDICT="SKIP"
    elif [ "$ACTUAL" = "$EXP" ]; then
      VERDICT="OK"
    elif [ "$ACTUAL" = "$PREV" ]; then
      VERDICT="NOCHANGE"
    elif [ "${#ACTUAL}" -lt "$PREVLEN" ]; then
      VERDICT="SHRANK"
    fi
    case "$VERDICT" in
      OK|SKIP) ;;
      *) FIELD_BAD=1; FAILURES=$((FAILURES + 1)) ;;
    esac
    printf '%-28s %-3s %-4s %-14s %-14s %s\n' "$FIELD" "$((STEP+1))" "$K" "'$ACTUAL'" "'$EXP'" "$VERDICT"
    printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$FIELD" "$((STEP+1))" "$K" "$ACTUAL" "$EXP" "$VERDICT" >> "$RESULTS"
    PREV="$ACTUAL"
    STEP=$((STEP + 1))
  done
  MIRROR="$(cross_check "$FIELD")"
  echo "    uiautomator 佐證: $MIRROR"
  echo "=== $FIELD ===" >> "$OUT_DIR/ic-calls.txt"
  ic_calls_since 60 >> "$OUT_DIR/ic-calls.txt"
  if [ "$FIELD_BAD" -ne 0 ]; then
    adbs exec-out screencap -p > "$OUT_DIR/fail-$FIELD.png" 2>/dev/null || true
    echo "    [!] 這一格有問題,截圖 $OUT_DIR/fail-$FIELD.png,IC 呼叫:"
    ic_calls_since 12 | sed 's/^/        /'
  fi
done

# ───────────────────────────────────────────── 組字中斷情境 ─────────────────
#
# 這些都是真實使用會遇到、而單一 EditText 靶永遠測不到的路徑。
# 這一段不做 pass/fail 斷言(不同方案的合理行為不同),而是把每一步的
# 狀態與 InputConnection 呼叫記錄下來,交由人判讀 + 供日後 diff。

scen() {
  echo; echo "── 情境:$* ──"; echo "── 情境:$* ──" >> "$SCEN_LOG"
  ensure_healthy || echo "    [WARN] 本情境開跑時 librime 不健康,結果僅供參考"
}
note() { echo "    $*"; echo "    $*" >> "$SCEN_LOG"; }

type_partial() { local s="$1" i=0; while [ $i -lt ${#s} ]; do tap_key "${s:$i:1}"; sleep 0.7; i=$((i+1)); done; }

if [ "$RUN_SCENARIOS" -eq 1 ]; then
  echo
  echo "======================= 組字中斷情境 ======================="

  # 1. 組字中切換到另一個輸入框
  scen "組字中切換輸入框"
  launch_field textCapSentencesMultiLine
  adbs logcat -c >/dev/null 2>&1
  type_partial "ni"
  note "切換前: $(last_state)"
  launch_field text
  sleep 1
  note "換框後: $(last_state)"
  note "IC: $(ic_calls_since 6 | tr '\n' ' ')"
  type_partial "h"
  note "新框輸入 h 後: $(last_state)"
  SWSTATE="$(state_text "$(last_state)")"
  if [ "$SWSTATE" = "h" ]; then
    note "[PASS] 新框從乾淨狀態開始,沒沾到上一個框的組字"
  else
    note "[FAIL] 新框應為 'h',實得 '$SWSTATE' —— 舊框的組字漏過來了"
    FAILURES=$((FAILURES + 1))
  fi

  # 2. 組字中按 home 再回來
  scen "組字中按 HOME 再回到 app"
  launch_field textCapSentencesMultiLine
  adbs logcat -c >/dev/null 2>&1
  type_partial "ni"
  note "離開前: $(last_state)"
  adbs shell input keyevent KEYCODE_HOME >/dev/null 2>&1; sleep 1.5
  adbs shell am start -n "$ACT" --es field textCapSentencesMultiLine >/dev/null 2>&1; sleep 2
  note "回來後: $(last_state)"
  type_partial "h"
  note "再輸入 h 後: $(last_state)"
  note "IC: $(ic_calls_since 10 | tr '\n' ' ')"

  # 3. 組字中旋轉螢幕
  scen "組字中旋轉螢幕"
  launch_field textCapSentencesMultiLine
  adbs logcat -c >/dev/null 2>&1
  type_partial "ni"
  note "旋轉前: $(last_state)"
  adbs shell settings put system accelerometer_rotation 0 >/dev/null 2>&1
  adbs shell settings put system user_rotation 1 >/dev/null 2>&1; sleep 3
  note "橫向後: $(last_state)"
  adbs shell settings put system user_rotation 0 >/dev/null 2>&1; sleep 3
  note "轉回直向: $(last_state)"
  note "IC: $(ic_calls_since 14 | tr '\n' ' ')"

  # 4. 游標放在既有文字中間再打字
  # launch_field 會點一下輸入框(WebView 需要焦點,也順便等完鍵盤進場動畫),
  # 而點擊會把游標帶到文字尾端。所以這裡改用方向鍵把游標移回中間 ——
  # 這也順便驗到方向鍵那條路徑。
  scen "游標在既有文字中間"
  launch_field text --es prefill "abcdef"
  adbs shell input keyevent KEYCODE_MOVE_END >/dev/null 2>&1
  adbs shell input keyevent 21 21 21 >/dev/null 2>&1   # DPAD_LEFT ×3 → 游標到 3
  sleep 0.8
  adbs logcat -c >/dev/null 2>&1
  note "初始(游標應在 3): $(last_state)"
  type_partial "ni"
  note "在中間打 ni: $(last_state)"
  note "IC: $(ic_calls_since 8 | tr '\n' ' ')"
  MIDSTATE="$(state_text "$(last_state)")"
  if [ "$MIDSTATE" = "abcnidef" ]; then
    note "[PASS] 組字插在游標處"
  else
    note "[FAIL] 應為 'abcnidef'(組字插在游標處),實得 '$MIDSTATE'"
    FAILURES=$((FAILURES + 1))
  fi

  # 5. 連續快速點擊(60ms 間隔,比真人再快一點)
  scen "連續快速點擊(無等待)"
  launch_field textCapSentencesMultiLine
  adbs logcat -c >/dev/null 2>&1
  for c in n i h a o; do tap_key "$c"; done
  sleep 2.5
  note "快速打完 nihao: $(last_state)"
  note "IC: $(ic_calls_since 12 | tr '\n' ' ')"

  # 6. 數字框收得下數字
  #    字母打不進去是宿主的過濾器在擋(正確);但數字必須進得去。這一格如果
  #    也空著,就代表 IME 對數字框整個失效 —— 那才是使用者說的「沒反應」。
  scen "數字框收得下數字(?123 層)"
  launch_field number
  adbs logcat -c >/dev/null 2>&1
  tap_key SYMBOLS; sleep 1.0          # 切到 numeric-symbol 佈局
  tap_digit 1; sleep 0.8
  tap_digit 2; sleep 0.8
  note "數字框打 12: $(last_state)"
  note "IC: $(ic_calls_since 8 | tr '\n' ' ')"
  NUMSTATE="$(state_text "$(last_state)")"
  if [ "$NUMSTATE" = "12" ]; then
    note "[PASS] 數字框收到 '12'"
  else
    note "[FAIL] 數字框應為 '12',實得 '$NUMSTATE'"
    FAILURES=$((FAILURES + 1))
    adbs exec-out screencap -p > "$OUT_DIR/fail-number-digits.png" 2>/dev/null || true
  fi

  # 7. 組字中按 send.text 的鍵(§9.4.1)
  #    這是本檔最重要的一條迴歸:`send.text` 繞過 librime 直接上屏,若不先
  #    rs_commit_composition() 就 commitText(),commitText 會**取代整個組字
  #    區** —— 使用者剛打的 preedit 無聲消失,而 librime 仍以為自己在組字,
  #    下一鍵會把消失的 preedit 畫回來。修正後正確的呼叫序列是:
  #        commitText(<上屏字>) → finishComposingText() → commitText(<標點>)
  #    路徑:拼音打 ni(組字中)→ ?123 → #+= → €(這顆是 send.text)。
  scen "組字中按 send.text 的鍵(§9.4.1)"
  launch_field text
  adbs logcat -c >/dev/null 2>&1
  type_partial "ni"
  note "組字中: $(last_state)"
  tap_key SYMBOLS; sleep 1.0        # → numeric-symbol 佈局
  tap_key TO_SYMBOL; sleep 1.0      # → symbol 層
  tap_key EURO; sleep 1.5           # € 是 send.text
  LITSTATE="$(state_text "$(last_state)")"
  note "按 € 之後: $(last_state)"
  note "IC: $(ic_calls_since 12 | tr '\n' ' ')"
  case "$LITSTATE" in
    "€")
      note "[FAIL] 組字區被 commitText 整段取代,preedit 無聲消失(§9.4.1 未落實)"
      FAILURES=$((FAILURES + 1))
      ;;
    *€)
      note "[PASS] preedit 先上屏再接標點:'$LITSTATE'"
      ;;
    *)
      note "[FAIL] 預期以 € 結尾,實得 '$LITSTATE'"
      FAILURES=$((FAILURES + 1))
      ;;
  esac

  # 8. 部署進行中(session 失效)打字
  #    rs_process_key 在 session 失效時回傳 false,於是每一鍵都落到
  #    fallbackKey()。這條路徑最可疑的失手方式是「所有鍵都被當成 BackSpace」
  #    —— 那正是使用者回報的「打一個字刪一個字」會長的樣子。所以這裡的
  #    斷言不是「打出什麼」,而是**絕對不可以出現 deleteSurroundingText**。
  scen "部署進行中(session 尚未就緒)打字"
  adbs shell am force-stop "$IME_PKG" >/dev/null 2>&1
  sleep 0.5
  launch_field textCapSentencesMultiLine
  adbs logcat -c >/dev/null 2>&1
  for c in n i h a o; do tap_key "$c"; sleep 0.3; done
  sleep 3
  note "session 未就緒時打 nihao: $(last_state)"
  SESSCALLS="$(ic_calls_since 30 | tr '\n' ' ')"
  note "IC: $SESSCALLS"
  note "IME 日誌: $(adbs logcat -d -s RimeIME:I 2>/dev/null | tr -d '\r' | tail -8 | tr '\n' ' ')"
  case "$SESSCALLS" in
    *deleteSurroundingText*)
      note "[FAIL] session 失效時按鍵被當成刪除鍵 —— 這就是「打字反而刪字」"
      FAILURES=$((FAILURES + 1))
      ;;
    *)
      note "[PASS] 沒有出現 deleteSurroundingText;按鍵回落成字面上屏"
      ;;
  esac
  SESSSTATE="$(state_text "$(last_state)")"
  if [ -n "$SESSSTATE" ]; then
    note "[PASS] 字有進去('$SESSSTATE'),沒有整段吞掉"
  else
    note "[FAIL] session 未就緒時按鍵被完全吞掉,使用者會看到「沒反應」"
    FAILURES=$((FAILURES + 1))
  fi
  # ⚠ 一定要把輸入法救回來。`am force-stop` 會把套件打成 Android 的 stopped
  #   狀態,IMMS 不會自己再把它綁回來 —— 不重新 `ime set` 的話,這之後的
  #   任何測試都會在「根本沒有輸入法」的情況下空跑,而且看起來還會像有結果。
  adbs shell ime enable "$IME_ID" >/dev/null 2>&1
  adbs shell ime set "$IME_ID" >/dev/null 2>&1
  sleep 2
  wait_for_rime_ready >/dev/null 2>&1 || note "[WARN] 情境結束後 librime 沒有回來"


  adbs shell settings put system accelerometer_rotation 1 >/dev/null 2>&1
fi

echo
echo "================================================================"
if [ "$FAILURES" -eq 0 ]; then
  echo "矩陣全綠:$(wc -l < "$RESULTS") 格全部符合預期"
else
  echo "矩陣有 $FAILURES 格不符預期,詳見 $RESULTS"
fi
echo "artifact: $OUT_DIR"
echo "  results.tsv    每一格的實際/預期/判定"
echo "  ic-calls.txt   每個欄位收到的 InputConnection 呼叫"
echo "  scenarios.txt  組字中斷情境的逐步狀態"
echo "================================================================"
[ "$FAILURES" -eq 0 ]
