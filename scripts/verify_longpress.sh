#!/usr/bin/env bash
#
# verify_longpress.sh — 「按住」之後鍵盤還是好的嗎
#
# 為什麼需要這一支(這是實測換來的):
#
#   `adb shell input tap` 的 down 與 up 之間幾乎沒有間隔。那個「按下之後
#   按鍵永久變灰」的缺陷,用 tap 一次都重現不出來 —— 要用
#   `input swipe <x> <y> <x> <y> <毫秒>`(起訖同點)模擬按住 100ms 以上才會出現。
#   換句話說,只會 tap 的自動化在這一整類缺陷面前是瞎的:它會報全綠,
#   而使用者按一下鍵盤就毀了。這個 bug 就是這樣漏到使用者手上的。
#
# 怎麼判定「壞了」:
#
#   鍵盤是自繪的,`uiautomator dump` **看不到它的節點**(它是 TYPE_INPUT_METHOD
#   的另一個視窗,dump 只 dump 前景 app)。所以只能看畫面:按住之前拍一張、
#   按住並清空之後再拍一張,比對鍵盤那塊矩形的像素。
#   一顆鍵永久變色 ≈ 該區域的 2%,遠高於門檻。
#
# 座標怎麼來(這一段是這支腳本最容易做錯的地方):
#
#   · **不能寫死。** 鍵盤高度是活的(主題、鍵盤高度偏好、自適應修正),
#     寫死的下場是測試在戳空氣而且照樣報綠。
#   · **也不能只靠 `dumpsys input_method` 的 touchableRegion。** 本專案的 IME
#     沒有覆寫 onComputeInsets,那個欄位是空的 SkRegion()、contentTopInsets 是 0。
#     本機讀得到是因為那台裝著 Gboard,量到的是它的視窗。
#   · **更不能隨便挑畫面下半部的幾個點。** 實測過:那樣會戳到工具列的齒輪,
#     叫出鍵盤內建的設定面板,然後這一關報「畫面差異 29.8%」——
#     一個看起來像重大缺陷、其實是測試自己按出來的假失敗。
#
#   所以改成**由下往上實際量**:在畫面中線由下往上逐點輕點,記下「戳了會打出
#   字」的那幾條橫列。會打出字 = 那裡是一顆做事的按鍵。由下往上而且找到三列
#   就停,所以永遠掃不到工具列(它在鍵盤最上緣),齒輪不會被誤觸。
#   量不到就中止 —— 沒有座標就不可能驗這件事,寧可紅也不要報一個沒驗到的綠。
#
# 比對器本身也會被驗:
#
#   一個「永遠說相同」的比對器會讓這支腳本變成裝飾品,而且不會有任何徵狀。
#   所以每次跑都會在事後那張圖上**植入一塊一顆鍵大小的灰色方塊**,
#   要求比對器對它報紅。植入的違規沒被抓到 → 這支腳本自己判定失敗。
#
# 用法:
#   ./verify_longpress.sh --ime org.rimequad.ime/.RimeInputMethodService \
#                         [--apk <path>] [--hold-ms 150] [--out <dir>]
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
HOLD_MS=150
LONG_MS=700
OUT_DIR="$ROOT/build/verify-longpress"
THRESHOLD="0.008"          # 差異像素比例上限。一顆鍵約佔比對區域的 2%。
DEPLOY_TIMEOUT=120
READY_LOG="phase . READY"
EXPECT_KEYS="nihao"
EXPECT_TEXT="你好"

while [ $# -gt 0 ]; do
  case "$1" in
    --ime)        IME_ID="$2"; shift 2 ;;
    --apk)        APKS+=("$2"); shift 2 ;;
    --hold-ms)    HOLD_MS="$2"; shift 2 ;;
    --long-ms)    LONG_MS="$2"; shift 2 ;;
    --out)        OUT_DIR="$2"; shift 2 ;;
    --threshold)  THRESHOLD="$2"; shift 2 ;;
    --ready-log)  READY_LOG="$2"; shift 2 ;;
    --keys)       EXPECT_KEYS="$2"; shift 2 ;;
    --expect)     EXPECT_TEXT="$2"; shift 2 ;;
    -h|--help)    sed -n '2,55p' "$0"; exit 0 ;;
    *) echo "未知參數: $1" >&2; exit 2 ;;
  esac
done

[ -n "$IME_ID" ] || { echo "缺少 --ime" >&2; exit 2; }
IME_PKG="${IME_ID%%/*}"
mkdir -p "$OUT_DIR"

pass() { echo "  [PASS] $*"; }
fail() { echo "  [FAIL] $*" >&2; echo >&2; echo "artifact 在:$OUT_DIR" >&2; exit 1; }
step() { echo; echo "=== $* ==="; }

SERIAL="${RIME_SERIAL:-${ANDROID_SERIAL:-}}"
if [ -z "$SERIAL" ]; then
  SERIAL="$("$ADB" devices | awk '/emulator-[0-9]+\tdevice/{print $1; exit}')"
fi
[ -n "$SERIAL" ] || fail "找不到已連線的模擬器(可用 RIME_SERIAL 指定)"
adbs() { "$ADB" -s "$SERIAL" "$@"; }
"$ADB" -s "$SERIAL" get-state >/dev/null 2>&1 || fail "裝置 $SERIAL 未連線"

# 讀出前景 app 那個輸入框的內容。鍵盤本身 dump 不出來(見檔頭),但輸入框
# dump 得出來 —— 「戳這裡會不會打出字」就是靠它回答的。
read_field() {
  adbs shell "uiautomator dump /sdcard/rime_lp.xml >/dev/null 2>&1" >/dev/null 2>&1 || return 1
  adbs pull /sdcard/rime_lp.xml "$OUT_DIR/ui.xml" >/dev/null 2>&1 || return 1
  adbs shell rm -f /sdcard/rime_lp.xml >/dev/null 2>&1 || true
  python3 - "$OUT_DIR/ui.xml" <<'PY'
import sys, xml.etree.ElementTree as ET
root = ET.parse(sys.argv[1]).getroot()
best = ""
for n in root.iter("node"):
    if "EditText" not in n.get("class", ""):
        continue
    if n.get("focused") == "true":
        print(n.get("text", "")); sys.exit(0)
    if n.get("text") and not best:
        best = n.get("text")
print(best)
PY
}

clear_field() {
  local cmd="" i
  for i in $(seq 1 40); do cmd="${cmd}input keyevent 67; "; done
  adbs shell "$cmd" >/dev/null 2>&1 || true
}

echo "============================================"
echo " 按住(long press)回歸驗證"
echo " 裝置   : $SERIAL"
echo " IME    : $IME_ID"
echo " 按住   : ${HOLD_MS}ms 與 ${LONG_MS}ms(tap 的 down/up 間隔約 0ms,測不到)"
echo "============================================"

# --- 1. 準備 ----------------------------------------------------------------
step "1. 安裝與啟用"
for apk in ${APKS+"${APKS[@]}"}; do
  [ -f "$apk" ] || fail "找不到 APK: $apk"
  adbs install -r -g "$apk" >/dev/null 2>&1 || fail "安裝失敗: $apk"
  pass "已安裝 $(basename "$apk")"
done
adbs shell ime enable "$IME_ID" >/dev/null 2>&1 || true
adbs shell ime set "$IME_ID" >/dev/null 2>&1 || true
adbs shell ime list -s 2>/dev/null | grep -q "^$IME_ID\$" || fail "系統看不到 $IME_ID"
pass "已啟用並設為預設"

step "2. 開啟輸入目標並等待鍵盤"
adbs shell monkey -p dev.rime.imetest -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1 \
  || fail "無法啟動 dev.rime.imetest,先跑 scripts/build_testapp.sh"
sleep 2
SHOWN=0
for i in $(seq 1 "$DEPLOY_TIMEOUT"); do
  adbs shell dumpsys input_method > "$OUT_DIR/input_method.txt" 2>/dev/null || true
  grep -q "mIsInputViewShown=true" "$OUT_DIR/input_method.txt" && { SHOWN=1; break; }
  [ "$i" -eq 5 ] && adbs shell input tap 540 300 >/dev/null 2>&1 || true
  sleep 1
done
[ "$SHOWN" -eq 1 ] || fail "鍵盤在 ${DEPLOY_TIMEOUT}s 內沒有出現"
CUR_ID="$(grep -o 'mCurId=[^ ]*' "$OUT_DIR/input_method.txt" | head -1 | cut -d= -f2)"
[ "$CUR_ID" = "$IME_ID" ] || fail "目前綁定的是 $CUR_ID,不是待測的 $IME_ID"
pass "鍵盤已顯示,綁定正確"

if [ -n "$READY_LOG" ]; then
  READY=0
  for i in $(seq 1 "$DEPLOY_TIMEOUT"); do
    adbs logcat -d 2>/dev/null | grep -Eq "$READY_LOG" && { READY=1; break; }
    sleep 1
  done
  [ "$READY" -eq 1 ] && pass "引擎就緒" || echo "  [INFO] 沒等到就緒訊息($READY_LOG),繼續"
fi

# --- 3. 量出「戳了會打出字」的橫列 ------------------------------------------
step "3. 由下往上量出按鍵橫列(不寫死座標,也不猜)"
SZ="$(adbs shell wm size 2>/dev/null | tr -d '\r')"
WH="$(printf '%s\n' "$SZ" | sed -n 's/^Override size: \([0-9]*\)x\([0-9]*\)$/\1 \2/p' | head -1)"
[ -n "$WH" ] || WH="$(printf '%s\n' "$SZ" | sed -n 's/^Physical size: \([0-9]*\)x\([0-9]*\)$/\1 \2/p' | head -1)"
[ -n "$WH" ] || fail "讀不到 wm size"
set -- $WH; SW="$1"; SH="$2"
echo "  螢幕 ${SW}x${SH}"

CX=$((SW / 2))
STEP=$((SH / 40))                 # 掃描步距,約螢幕高的 2.5%
Y=$((SH * 96 / 100))              # 從最下面開始(再往下就是導覽列)
ROWS=""
NROW=0
LASTY=0
while [ "$Y" -gt $((SH * 45 / 100)) ] && [ "$NROW" -lt 3 ]; do
  clear_field
  adbs shell input tap "$CX" "$Y" >/dev/null 2>&1 || true
  sleep 0.6
  T="$(read_field || true)"
  if [ -n "$T" ]; then
    # 同一列會連中好幾次,離上一列太近的視為同一列。
    if [ "$LASTY" -eq 0 ] || [ $((LASTY - Y)) -gt $((SH / 40)) ]; then
      ROWS="$ROWS $Y"; LASTY="$Y"; NROW=$((NROW + 1))
      echo "  y=$Y 打出 '$T' → 這是一條按鍵橫列"
    fi
  fi
  Y=$((Y - STEP))
done
clear_field
[ "$NROW" -ge 2 ] || fail "由下往上掃到畫面中線都只量到 $NROW 條按鍵橫列。
       沒有座標就驗不了「按住」,而報一個沒驗到的綠燈比紅燈糟。
       先確認鍵盤真的在前景、而且按鍵按下去會出字。"
pass "量到 $NROW 條按鍵橫列:$ROWS"

# --- 3b. 逐點確認,把會改變鍵盤狀態的鍵剔掉 ---------------------------------
# 同一條橫列上不是每顆鍵都「只是打出一個字」:`?123` 會換層、`中/En` 會換
# 模式**而且換佈局**、`⇧` 會換大小寫。按住那幾顆之後鍵盤本來就長得不一樣,
# 拿去做像素比對必定報一個假的「壞了」。
#
# 篩選條件很簡單而且剛好就是我們要的:**輕點它會不會打出字**。
# 會打出字的就是普通按鍵,換層/換模式的那幾顆一顆都不會 —— 它們自己把
# 自己排除掉了,不必在腳本裡寫死「哪一顆是模式鍵」(寫死就會跟著佈局腐爛)。
step "3b. 逐點確認(把換層、換模式、換大小寫的鍵剔掉)"
POINTS=""
NPT=0
for ry in $ROWS; do
  for fx in 25 50 75; do
    X=$((SW * fx / 100))
    clear_field
    adbs shell input tap "$X" "$ry" >/dev/null 2>&1 || true
    sleep 0.6
    T="$(read_field || true)"
    if [ -n "$T" ]; then
      POINTS="$POINTS ${X},${ry}"
      NPT=$((NPT + 1))
    else
      echo "  ($X,$ry) 輕點沒有打出字 → 多半是模式鍵,不拿它做比對"
    fi
  done
done
clear_field
[ "$NPT" -ge 3 ] || fail "只找到 $NPT 顆「輕點會打出字」的按鍵,樣本太少,不足以判定"
pass "確認 $NPT 顆普通按鍵:$POINTS"

# --- 3c. 把鍵盤打回已知狀態 -------------------------------------------------
# 上面的校準一定會在輸入法裡留下痕跡(打了字、可能不小心切過模式)。
# 基準畫面必須拍在乾淨的狀態上,否則後面比到的是校準的殘留而不是缺陷。
# 重啟輸入法是唯一能保證「回到出廠狀態」的做法 —— 而它在**拍基準之前**做,
# 所以不會把真正要抓的那個「按住之後留下的痕跡」一起洗掉。
step "3c. 重啟輸入法,把鍵盤打回已知狀態"
adbs shell am force-stop "$IME_PKG" >/dev/null 2>&1 || true
adbs logcat -c >/dev/null 2>&1 || true
sleep 1
adbs shell monkey -p dev.rime.imetest -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1 || true
SHOWN=0
for i in $(seq 1 "$DEPLOY_TIMEOUT"); do
  adbs shell dumpsys input_method 2>/dev/null | grep -q "mIsInputViewShown=true" && { SHOWN=1; break; }
  [ "$i" -eq 5 ] && adbs shell input tap 540 300 >/dev/null 2>&1 || true
  sleep 1
done
[ "$SHOWN" -eq 1 ] || fail "重啟後鍵盤沒有再出現"
if [ -n "$READY_LOG" ]; then
  for i in $(seq 1 "$DEPLOY_TIMEOUT"); do
    adbs logcat -d 2>/dev/null | grep -Eq "$READY_LOG" && break
    sleep 1
  done
fi
clear_field
pass "鍵盤回到初始狀態"

# 比對區域:最上面那一列再往上一個列距,到導覽列上緣。
# 刻意**不含工具列** —— 掃描不會掃到它,自然也不該拿它來比。
TOPROW="$(printf '%s\n' $ROWS | sort -n | head -1)"
KL=0; KR="$SW"
KT=$((TOPROW - STEP * 2)); [ "$KT" -lt 0 ] && KT=0
KB=$((SH * 97 / 100))
[ "$((KB - KT))" -gt 100 ] || fail "比對區域不合理:($KL,$KT)-($KR,$KB)"
pass "比對區域 = ($KL,$KT)-($KR,$KB),$((KR - KL))x$((KB - KT)) px"

# --- 4. 基準畫面 ------------------------------------------------------------
step "4. 拍下按住之前的鍵盤"
sleep 2
shot() { adbs exec-out screencap > "$1" || fail "screencap 失敗"; [ -s "$1" ] || fail "screencap 是空的"; }
shot "$OUT_DIR/before.raw"
pass "before.raw ($(wc -c < "$OUT_DIR/before.raw") bytes)"

# --- 5. 按住 ----------------------------------------------------------------
# 起訖同點的 swipe = 真正的 down…等待…up。tap 沒有這段等待。
step "5. 在確認過的按鍵上按住(${HOLD_MS}ms,再 ${LONG_MS}ms)"
N=0
for pt in $POINTS; do
  X="${pt%,*}"; Y="${pt#*,}"
  adbs shell input swipe "$X" "$Y" "$X" "$Y" "$HOLD_MS" >/dev/null 2>&1 \
    || fail "input swipe 失敗(這個 Android 版本可能不支援 duration 參數)"
  N=$((N + 1))
  sleep 0.4
done
TYPED="$(read_field || true)"
[ -n "$TYPED" ] || fail "按住 $N 次之後輸入框仍是空的。
       同樣的位置**輕點**打得出字(第 3b 關剛驗過)、**按住**卻打不出來 ——
       那本身就是缺陷,而且正是只用 tap 的自動化看不見的那一種。"
pass "${HOLD_MS}ms 按住 $N 次都有反應(輸入框:'$TYPED')"

for pt in $POINTS; do
  # 超過系統長按門檻(500ms):走的是 onLongClick 與彈出盤那條路徑。
  X="${pt%,*}"; Y="${pt#*,}"
  adbs shell input swipe "$X" "$Y" "$X" "$Y" "$LONG_MS" >/dev/null 2>&1 || true
  N=$((N + 1))
  sleep 0.8
done
pass "已送出 $N 次按住(含 ${LONG_MS}ms 的長按)"

# --- 6. 復原畫面 ------------------------------------------------------------
step "6. 清空輸入並回到閒置狀態"
# 不送 BACK:BACK 在鍵盤顯示時會把鍵盤收起來,收起再叫出來會多一次視窗動畫,
# 比到的就變成殘影而不是缺陷。長按的彈出盤在手指放開時本來就會收掉。
clear_field
sleep 3
SHOWN2=0
for i in $(seq 1 20); do
  adbs shell dumpsys input_method 2>/dev/null | grep -q "mIsInputViewShown=true" && { SHOWN2=1; break; }
  adbs shell monkey -p dev.rime.imetest -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1 || true
  sleep 2
done
[ "$SHOWN2" -eq 1 ] || fail "清空之後鍵盤不見了,無法比對(這本身也可能是缺陷)"
sleep 2
pass "已清空,鍵盤仍在"

step "7. 拍下按住之後的鍵盤"
shot "$OUT_DIR/after1.raw"
sleep 1.5
shot "$OUT_DIR/after2.raw"
pass "after1.raw / after2.raw"

# --- 8. 比對(含植入違規的自我驗證)-----------------------------------------
step "8. 比對按鍵區域"
python3 - "$OUT_DIR" "$KL" "$KT" "$KR" "$KB" "$THRESHOLD" <<'PY'
import struct, sys, os

out, L, T, R, B = sys.argv[1], *map(int, sys.argv[2:6])
thr = float(sys.argv[6])

def load(path):
    raw = open(path, "rb").read()
    w, h, f = struct.unpack("<III", raw[:12])
    hdr = len(raw) - w * h * 4
    if hdr not in (12, 16):
        # 換算不出整齊的 header 就代表這不是我們以為的 RGBA_8888,
        # 硬比下去只會得到一個沒有意義的數字。寧可失敗。
        sys.exit("screencap 格式不符預期:%dx%d fmt=%d len=%d(算出 header %d)"
                 % (w, h, f, len(raw), hdr))
    return w, h, raw[hdr:]

def crop(w, h, buf, l, t, r, b):
    l, t = max(0, l), max(0, t)
    r, b = min(w, r), min(h, b)
    return (r - l), (b - t), b"".join(
        buf[(y * w + l) * 4:(y * w + r) * 4] for y in range(t, b))

def diff_ratio(a, b, npx):
    if len(a) != len(b):
        return 1.0
    n = 0
    # 逐像素比 RGB(忽略 alpha)。整數比對,不做容忍度 —— 要抓的是
    # 「一顆鍵變了顏色」,那是幾千個像素的整片差異。
    for i in range(0, len(a), 4):
        if a[i] != b[i] or a[i+1] != b[i+1] or a[i+2] != b[i+2]:
            n += 1
    return n / float(npx)

w0, h0, b0 = load(os.path.join(out, "before.raw"))
cw, ch, c0 = crop(w0, h0, b0, L, T, R, B)
npx = cw * ch
if npx < 10000:
    sys.exit("裁出來的區域太小(%dx%d),不可信" % (cw, ch))

best = None
for name in ("after1.raw", "after2.raw"):
    w, h, buf = load(os.path.join(out, name))
    if (w, h) != (w0, h0):
        sys.exit("前後畫面尺寸不同(%dx%d vs %dx%d),裝置轉向了?" % (w0, h0, w, h))
    _, _, c = crop(w, h, buf, L, T, R, B)
    r = diff_ratio(c0, c, npx)
    print("  %s 差異 %.4f%%" % (name, r * 100))
    best = r if best is None else min(best, r)

# ── 植入違規:比對器要抓得到「一顆鍵變灰」──────────────────────────
# 沒有這一段,一個永遠回 0 的比對器會讓上面那行永遠是 0.0000% 而沒人發現。
key_w, key_h = max(1, cw // 10), max(1, ch // 5)     # 約一顆鍵
bad = bytearray(c0)
for y in range(ch // 3, min(ch, ch // 3 + key_h)):
    off = (y * cw + cw // 3) * 4
    for x in range(key_w):
        bad[off + x*4:off + x*4 + 3] = b"\x80\x80\x80"
inj = diff_ratio(c0, bytes(bad), npx)
print("  [自我驗證] 植入一顆鍵大小的灰塊 → 差異 %.4f%%(門檻 %.4f%%)" % (inj * 100, thr * 100))
if inj <= thr:
    sys.exit("比對器抓不到植入的違規(%.4f%% <= 門檻 %.4f%%)。"
             "這代表這一關即使真的壞了也會報綠 —— 判定失敗。" % (inj * 100, thr * 100))

if best > thr:
    sys.exit("按住之後按鍵區域沒有復原:差異 %.4f%% > 門檻 %.4f%%。"
             "這正是「按住讓按鍵永久變色」那一類缺陷的樣子。"
             "raw 畫面留在 %s" % (best * 100, thr * 100, out))
print("  按住之後按鍵區域回到原狀(差異 %.4f%% <= %.4f%%)" % (best * 100, thr * 100))
PY
pass "按住不會讓按鍵留下痕跡,且比對器對植入的違規會報紅"

# --- 9. 按住之後還打得出字嗎 ------------------------------------------------
# 「畫面沒變」不等於「還能用」。按住若讓按鍵停在按下狀態而顏色又回來了,
# 上面那一關會放行,但鍵盤其實已經不吃事件了。所以最後實際打一次。
step "9. 按住之後仍然打得出字"
clear_field
CMD=""
i=0
while [ "$i" -lt "${#EXPECT_KEYS}" ]; do
  c="${EXPECT_KEYS:$i:1}"
  CMD="${CMD}input keyevent KEYCODE_$(printf '%s' "$c" | tr 'a-z' 'A-Z'); sleep 0.15; "
  i=$((i + 1))
done
CMD="${CMD}sleep 0.5; input keyevent KEYCODE_SPACE; "
adbs shell "$CMD" >/dev/null 2>&1 || fail "注入按鍵失敗"
sleep 1.5
ACTUAL="$(read_field || true)"
echo "  實際: '$ACTUAL'  預期含: '$EXPECT_TEXT'"
adbs exec-out screencap -p > "$OUT_DIR/after-longpress.png" 2>/dev/null || true
case "$ACTUAL" in
  *"$EXPECT_TEXT"*) pass "按住 $N 次之後,鍵盤仍然打得出「$EXPECT_TEXT」" ;;
  *) fail "按住之後打不出字了(輸入框是 '$ACTUAL')。畫面看起來正常但鍵盤已經不吃事件 —— 這正是只用 tap 驗不出來的那一類缺陷。" ;;
esac

echo
echo "按住回歸驗證通過。"
