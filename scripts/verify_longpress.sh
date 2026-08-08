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
#   所以搬上 CI 的驗證不能只有 tap。這一支專門送「按住」。
#
# 怎麼判定「壞了」:
#
#   鍵盤是自繪的,`uiautomator dump` **看不到它的節點**(它是 TYPE_INPUT_METHOD
#   的另一個視窗,dump 只 dump 前景 app)。所以不能用節點樹斷言,只能看畫面。
#   做法:按住之前拍一張、按住並清空之後再拍一張,比對**鍵盤那塊矩形**的像素。
#   一顆鍵永久變色 ≈ 鍵盤面積的 2%,遠高於門檻。
#
#   鍵盤矩形不寫死 —— 從 `dumpsys input_method` 的 touchableRegion 讀。
#   寫死座標在這個專案已經害過一次:主題與鍵盤高度會把每一排挪動幾十 px,
#   於是測試改成在戳空氣,還報綠。
#
# 比對器本身也會被驗:
#
#   一個「永遠說相同」的比對器會讓這支腳本變成裝飾品,而且不會有任何徵狀。
#   所以每次跑都會**在事後那張圖上植入一塊一顆鍵大小的灰色方塊**,
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
THRESHOLD="0.008"          # 差異像素比例上限。一顆鍵約佔鍵盤面積 2%。
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
    -h|--help)    sed -n '2,40p' "$0"; exit 0 ;;
    *) echo "未知參數: $1" >&2; exit 2 ;;
  esac
done

[ -n "$IME_ID" ] || { echo "缺少 --ime" >&2; exit 2; }
IME_PKG="${IME_ID%%/*}"
mkdir -p "$OUT_DIR"

pass() { echo "  [PASS] $*"; }
fail() { echo "  [FAIL] $*" >&2; echo >&2; echo "artifact 在:$OUT_DIR" >&2; exit 1; }
step() { echo; echo "=== $* ==="; }

# 讀出前景 app 那個輸入框的內容。鍵盤自己 dump 不出來(見檔頭),但輸入框
# dump 得出來,而「按住有沒有打出東西」正是靠它回答的。
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

SERIAL="${RIME_SERIAL:-${ANDROID_SERIAL:-}}"
if [ -z "$SERIAL" ]; then
  SERIAL="$("$ADB" devices | awk '/emulator-[0-9]+\tdevice/{print $1; exit}')"
fi
[ -n "$SERIAL" ] || fail "找不到已連線的模擬器(可用 RIME_SERIAL 指定)"
adbs() { "$ADB" -s "$SERIAL" "$@"; }
"$ADB" -s "$SERIAL" get-state >/dev/null 2>&1 || fail "裝置 $SERIAL 未連線"

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

# 引擎就緒(鍵盤畫出來 ≠ 可以打字,首次部署時上面寫著「正在編譯詞庫」)
if [ -n "$READY_LOG" ]; then
  READY=0
  for i in $(seq 1 "$DEPLOY_TIMEOUT"); do
    adbs logcat -d 2>/dev/null | grep -Eq "$READY_LOG" && { READY=1; break; }
    sleep 1
  done
  [ "$READY" -eq 1 ] && pass "引擎就緒" || echo "  [INFO] 沒等到就緒訊息($READY_LOG),繼續"
fi

# --- 3. 鍵盤矩形 ------------------------------------------------------------
step "3. 決定鍵盤矩形"
RECT="$(adbs shell dumpsys input_method 2>/dev/null \
        | sed -n 's/.*touchableRegion=SkRegion((\([0-9]*\),\([0-9]*\),\([0-9]*\),\([0-9]*\)).*/\1 \2 \3 \4/p' \
        | head -1)"
RECT_SRC="touchableRegion"
if [ -z "$RECT" ]; then
  # 本專案的 IME 沒有覆寫 onComputeInsets,所以系統根本不知道鍵盤佔哪一塊:
  # touchableRegion 是空的 SkRegion()、contentTopInsets 是 0。
  # (裝 Gboard 的機器上讀得到,所以本機開發時不會發現。)
  # 退而求其次用畫面下半部,但**絕不能就這樣相信它**:戳到桌布跟戳到鍵盤
  # 在像素比對上都會「沒有變化」,於是這一關會在什麼都沒驗到的情況下報綠。
  # 所以第 5 關之後會實際檢查那幾下按住有沒有打出東西 —— 沒有就判失敗。
  SZ="$(adbs shell wm size 2>/dev/null | tr -d '\r')"
  WH="$(printf '%s\n' "$SZ" | sed -n 's/^Override size: \([0-9]*\)x\([0-9]*\)$/\1 \2/p' | head -1)"
  [ -n "$WH" ] || WH="$(printf '%s\n' "$SZ" | sed -n 's/^Physical size: \([0-9]*\)x\([0-9]*\)$/\1 \2/p' | head -1)"
  [ -n "$WH" ] || fail "既讀不到 touchableRegion 也讀不到 wm size,不知道鍵盤在哪"
  set -- $WH
  # 下緣留 3%:那裡是導覽列,戳它會退出 app。
  RECT="0 $(( $2 * 55 / 100 )) $1 $(( $2 * 97 / 100 ))"
  RECT_SRC="畫面下半部（推定，${1}x${2}）"
  echo "  [INFO] dumpsys 的 touchableRegion 是空的（本 IME 不回報 insets）"
fi
set -- $RECT
KL="$1"; KT="$2"; KR="$3"; KB="$4"
[ "$((KR - KL))" -gt 100 ] && [ "$((KB - KT))" -gt 100 ] || fail "鍵盤矩形不合理:$RECT"
pass "鍵盤矩形 = ($KL,$KT)-($KR,$KB),$((KR - KL))x$((KB - KT)) px（來源:$RECT_SRC）"

# --- 4. 基準畫面 ------------------------------------------------------------
step "4. 拍下按住之前的鍵盤"
shot() { adbs exec-out screencap > "$1" || fail "screencap 失敗"; [ -s "$1" ] || fail "screencap 是空的"; }
shot "$OUT_DIR/before.raw"
pass "before.raw ($(wc -c < "$OUT_DIR/before.raw") bytes)"

# --- 5. 按住 ----------------------------------------------------------------
# 起訖同點的 swipe = 真正的 down…等待…up。tap 沒有這段等待。
step "5. 在鍵盤上按住(${HOLD_MS}ms × 9 點,再 ${LONG_MS}ms × 3 點)"
frac_x() { echo $((KL + (KR - KL) * $1 / 100)); }
frac_y() { echo $((KT + (KB - KT) * $1 / 100)); }
N=0
for fy in 35 55 75; do
  for fx in 25 50 75; do
    adbs shell input swipe "$(frac_x $fx)" "$(frac_y $fy)" "$(frac_x $fx)" "$(frac_y $fy)" "$HOLD_MS" \
      >/dev/null 2>&1 || fail "input swipe 失敗(這個 Android 版本可能不支援 duration 參數)"
    N=$((N + 1))
    sleep 0.4
  done
done
# ── 戳到的真的是鍵盤嗎 ──────────────────────────────────────────────
# 這一步是上面那個「推定矩形」的安全帶,少了它整關會變成裝飾品:
# 對著桌布按住 9 次,畫面當然不會有變化,於是像素比對報綠、這一關報通過,
# 而「按住」這條路徑一次都沒有被執行到。
# 按住之後輸入框裡必須出現東西 —— 那才證明按到的是會做事的按鍵。
TYPED="$(read_field || true)"
if [ -z "$TYPED" ]; then
  fail "按住 $N 次之後輸入框仍是空的。要嘛戳的位置不在鍵盤上(矩形推定錯了),
       要嘛按鍵根本不吃「按住」只吃「輕點」—— 兩種都必須修,不能當作通過。"
fi
pass "按住有打出東西（輸入框:'$TYPED'）—— 戳到的確實是鍵盤"

for fx in 25 50 75; do
  # 超過系統長按門檻(500ms):走的是 onLongClick 與彈出盤那條路徑。
  adbs shell input swipe "$(frac_x $fx)" "$(frac_y 55)" "$(frac_x $fx)" "$(frac_y 55)" "$LONG_MS" \
    >/dev/null 2>&1 || true
  N=$((N + 1))
  sleep 0.8
done
pass "已送出 $N 次按住"

# --- 6. 復原畫面 ------------------------------------------------------------
step "6. 清空輸入並回到閒置狀態"
# 不送 BACK。BACK 在鍵盤顯示時的預設行為是收起鍵盤,收起再叫出來會換一次
# 視窗動畫,比到的就變成動畫殘影而不是缺陷。長按的彈出盤在手指放開時本來
# 就會收掉,不需要幫它收。
# 只做兩件事:退格清空,然後等動畫落地。
CMD=""; for i in $(seq 1 40); do CMD="${CMD}input keyevent 67; "; done
adbs shell "$CMD" >/dev/null 2>&1 || true
sleep 3
# 鍵盤若在這段期間被收掉了,後面比的是兩張不同的東西 —— 那會得到一個
# 很大的差異值,看起來像缺陷但其實是測試自己弄丟了鍵盤。所以先確認。
SHOWN2=0
for i in $(seq 1 20); do
  adbs shell dumpsys input_method 2>/dev/null | grep -q "mIsInputViewShown=true" && { SHOWN2=1; break; }
  adbs shell monkey -p dev.rime.imetest -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1 || true
  sleep 2
done
[ "$SHOWN2" -eq 1 ] || fail "清空之後鍵盤不見了,無法比對(這本身也可能是缺陷,看 $OUT_DIR)"
sleep 2
pass "已清空,鍵盤仍在"

step "7. 拍下按住之後的鍵盤"
shot "$OUT_DIR/after1.raw"
sleep 1.5
shot "$OUT_DIR/after2.raw"
pass "after1.raw / after2.raw"

# --- 8. 比對(含植入違規的自我驗證)-----------------------------------------
step "8. 比對鍵盤區域"
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
    rows = []
    for y in range(t, b):
        off = (y * w + l) * 4
        rows.append(buf[off:off + (r - l) * 4])
    return (r - l), (b - t), b"".join(rows)

def diff_ratio(a, b, npx):
    if len(a) != len(b):
        return 1.0
    n = 0
    # 逐像素比 RGB(忽略 alpha)。整數比對,不做模糊 —— 我們要抓的是
    # 「一顆鍵變了顏色」,那是幾千個像素的整片差異,不需要容忍度。
    for i in range(0, len(a), 4):
        if a[i] != b[i] or a[i+1] != b[i+1] or a[i+2] != b[i+2]:
            n += 1
    return n / float(npx)

w0, h0, b0 = load(os.path.join(out, "before.raw"))
cw, ch, c0 = crop(w0, h0, b0, L, T, R, B)
npx = cw * ch
if npx < 10000:
    sys.exit("裁出來的鍵盤區域太小(%dx%d),不可信" % (cw, ch))

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
    sys.exit("按住之後鍵盤畫面沒有復原:差異 %.4f%% > 門檻 %.4f%%。"
             "這正是「按住讓按鍵永久變色」那一類缺陷的樣子。"
             "raw 畫面留在 %s" % (best * 100, thr * 100, out))
print("  按住之後鍵盤回到原狀(差異 %.4f%% <= %.4f%%)" % (best * 100, thr * 100))
PY
pass "按住不會讓鍵盤留下痕跡,且比對器對植入的違規會報紅"

# --- 9. 按住之後還打得出字嗎 ------------------------------------------------
# 「畫面沒變」不等於「還能用」。按住若讓按鍵停在按下狀態而顏色又回來了,
# 上面那一關會放行,但鍵盤其實已經不吃事件了。所以最後實際打一次。
step "9. 按住之後仍然打得出字"
CMD=""
for i in $(seq 1 40); do CMD="${CMD}input keyevent 67; "; done
adbs shell "$CMD" >/dev/null 2>&1 || true
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
