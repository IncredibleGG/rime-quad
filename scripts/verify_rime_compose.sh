#!/usr/bin/env bash
#
# verify_rime_compose.sh — 組字層級的驗證
#
# 為什麼需要這支腳本(重要):
#
#   verify_ime.sh 用的是 `adb shell input text`,那條路徑走的是 InputConnection
#   的 commitText —— 文字直接塞進輸入框,**完全繞過輸入法的按鍵處理**。
#   它能證明「IME 有被綁定、輸入框收得到字」,但即使 librime 根本沒被載入、
#   或 rs_process_key 全部回傳 false,那個測試一樣會通過。
#
#   本腳本改用 `input keyevent`,送的是真正的實體按鍵事件,會經過
#   InputMethodService.onKeyDown → JNI → rs_process_key → librime。
#   最後斷言輸入框內容等於預期的漢字。**這是唯一能證明 RIME 引擎真的在工作的測試。**
#
# 典型用法:
#   ./verify_rime_compose.sh \
#       --ime org.rimequad.ime/.RimeInputMethodService \
#       --apk android/app/build/outputs/apk/debug/app-debug.apk \
#       --keys nihao --select space --expect 你好
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
EMU="$HERE/emu.sh"
SDK="${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}"
ADB="$SDK/platform-tools/adb"

IME_ID=""
APKS=()
KEYS=""
SELECT_KEY="space"
EXPECT=""
EXPECT_MODE="exact"          # exact | contains
TARGET="testapp"
OUT_DIR="$ROOT/build/verify-compose"
AUTO_START=1
KEY_DELAY="0.15"
# 見下方 --ready-log。空字串 = 維持原本行為，不做就緒等待。
READY_LOG=""
# 首次啟動時 librime 要編譯 schema(部署),在模擬器上可能要一分鐘以上。
# verify_ime.sh 的 20 秒等待對第一次跑我們自己的 IME 是不夠的。
DEPLOY_TIMEOUT=120

usage() {
  cat <<EOF
用法: $(basename "$0") --ime <id> --keys <字母> --expect <預期文字> [選項]

必要:
  --ime <pkg/.Service>   待測輸入法的 IME id
  --keys <abc>           要注入的按鍵序列(僅 a-z 與 0-9),例如 nihao
  --expect <文字>        選字之後輸入框應有的內容,例如 你好

選項:
  --apk <path>           先安裝這個 APK(可重複)
  --select <鍵>          選字鍵:space(預設) / 1..9 / enter / none
  --expect-contains      改用「包含」比對而非完全相等
  --target <testapp|settings>   輸入目標畫面,預設 testapp
  --deploy-timeout <秒>  等待首次部署與鍵盤出現的上限,預設 $DEPLOY_TIMEOUT
  --ready-log <regex>    鍵盤出現後,再等 logcat 出現此樣式才開始打字。
                         非同步初始化的 IME 必須用這個,否則會在部署還沒完成
                         時就注入按鍵。指定時會先 force-stop 待測 IME 並清空
                         logcat,確保每次都觀察得到完整的啟動序列。
                         本專案用法:--ready-log 'phase . READY'
  --key-delay <秒>       每個按鍵之間的間隔,預設 $KEY_DELAY
  --no-start             不自動啟動模擬器
  --out <dir>            artifact 輸出目錄,預設 $OUT_DIR
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    --ime)             IME_ID="$2"; shift 2 ;;
    --apk)             APKS+=("$2"); shift 2 ;;
    --keys)            KEYS="$2"; shift 2 ;;
    --select)          SELECT_KEY="$2"; shift 2 ;;
    --expect)          EXPECT="$2"; shift 2 ;;
    --expect-contains) EXPECT_MODE="contains"; shift ;;
    --target)          TARGET="$2"; shift 2 ;;
    --deploy-timeout)  DEPLOY_TIMEOUT="$2"; shift 2 ;;
    --ready-log)       READY_LOG="$2"; shift 2 ;;
    --key-delay)       KEY_DELAY="$2"; shift 2 ;;
    --no-start)        AUTO_START=0; shift ;;
    --out)             OUT_DIR="$2"; shift 2 ;;
    -h|--help)         usage; exit 0 ;;
    *) echo "未知參數: $1" >&2; usage >&2; exit 2 ;;
  esac
done

[ -n "$IME_ID" ] || { echo "缺少 --ime" >&2; exit 2; }
[ -n "$KEYS" ]   || { echo "缺少 --keys" >&2; exit 2; }
[ -n "$EXPECT" ] || { echo "缺少 --expect" >&2; exit 2; }

IME_PKG="${IME_ID%%/*}"
mkdir -p "$OUT_DIR"

pass() { echo "  [PASS] $*"; }
# 失敗時一定要把 logcat 抓下來。
# 這一版之前,「鍵盤沒出現」那條訊息叫人去看 $OUT_DIR/logcat.txt,而 logcat.txt
# 只在成功路徑的尾端才寫 —— 也就是**唯一需要它的時候它不存在**。CI 上因此有
# 一輪失敗完全查不出原因。診斷訊息指向的檔案必須真的會被產生出來。
dump_logcat_on_fail() {
  [ -n "${SERIAL:-}" ] || return 0
  [ -d "${OUT_DIR:-}" ] || return 0
  adbs logcat -d > "$OUT_DIR/logcat.txt" 2>/dev/null || true
  adbs shell dumpsys input_method > "$OUT_DIR/input_method.txt" 2>/dev/null || true
  # 部署訊息單獨抽一份出來,免得在幾萬行裡找。
  grep -Ei "rime|RimeQuad|org.rimequad|deploy|ANR|FATAL|died" \
    "$OUT_DIR/logcat.txt" > "$OUT_DIR/logcat-rime.txt" 2>/dev/null || true
  echo "  [INFO] 已存 logcat:$OUT_DIR/logcat.txt($(wc -l < "$OUT_DIR/logcat.txt" 2>/dev/null || echo 0) 行)、logcat-rime.txt" >&2
}
fail() { echo "  [FAIL] $*" >&2; dump_logcat_on_fail; echo >&2; echo "驗證失敗。artifact 在:$OUT_DIR" >&2; exit 1; }
step() { echo; echo "=== $* ==="; }

SERIAL=""
adbs() { "$ADB" -s "$SERIAL" "$@"; }

# --------------------------------------------------------------- 按鍵轉換 ---
# 把 "nihao" 轉成 "KEYCODE_N KEYCODE_I KEYCODE_H KEYCODE_A KEYCODE_O"。
# 只接受 a-z 與 0-9 —— 這支腳本的目的就是模擬實體按鍵,不處理符號。
keys_to_keycodes() {
  local s="$1" out="" c
  local i=0
  while [ "$i" -lt "${#s}" ]; do
    c="${s:$i:1}"
    case "$c" in
      [a-z]) out="$out KEYCODE_$(printf '%s' "$c" | tr 'a-z' 'A-Z')" ;;
      [A-Z]) out="$out KEYCODE_$c" ;;
      [0-9]) out="$out KEYCODE_$c" ;;
      *) echo "不支援的按鍵字元 '$c'(只接受 a-z 與 0-9)" >&2; return 1 ;;
    esac
    i=$((i + 1))
  done
  printf '%s' "${out# }"
}

select_keycode() {
  case "$1" in
    space)   echo "KEYCODE_SPACE" ;;
    enter)   echo "KEYCODE_ENTER" ;;
    [1-9])   echo "KEYCODE_$1" ;;
    none)    echo "" ;;
    *) echo "不支援的選字鍵 '$1'" >&2; return 1 ;;
  esac
}

# --------------------------------------------------------------- 讀輸入框 ---
read_field() {
  adbs shell "uiautomator dump /sdcard/rime_c.xml >/dev/null 2>&1" || return 1
  adbs pull /sdcard/rime_c.xml "$OUT_DIR/ui.xml" >/dev/null 2>&1 || return 1
  adbs shell rm -f /sdcard/rime_c.xml >/dev/null 2>&1 || true
  python3 - "$OUT_DIR/ui.xml" <<'PY'
import sys, xml.etree.ElementTree as ET
root = ET.parse(sys.argv[1]).getroot()
best = ""
for n in root.iter("node"):
    if "EditText" not in n.get("class", ""):
        continue
    txt = n.get("text", "")
    if n.get("focused") == "true":
        print(txt); sys.exit(0)
    if txt and not best:
        best = txt
print(best)
PY
}

clear_field() {
  local cur n codes i
  cur="$(read_field || true)"
  n="${#cur}"
  [ "$n" -eq 0 ] && return 0
  adbs shell input keyevent KEYCODE_MOVE_END >/dev/null 2>&1 || true
  codes=""
  for i in $(seq 1 $((n + 5))); do codes="$codes 67"; done
  # shellcheck disable=SC2086
  adbs shell input keyevent $codes >/dev/null 2>&1 || true
}

echo "============================================"
echo " RIME 組字驗證"
echo " IME    : $IME_ID"
echo " 按鍵   : $KEYS"
echo " 選字鍵 : $SELECT_KEY"
echo " 預期   : $EXPECT ($EXPECT_MODE)"
echo "============================================"

# --- 1. 模擬器 ---------------------------------------------------------------
step "1. 模擬器"
if ! "$EMU" status >/dev/null 2>&1; then
  [ "$AUTO_START" -eq 1 ] || fail "模擬器沒在跑,且指定了 --no-start"
  "$EMU" start
fi
# 不可以「抓第一台」。多條開發線並行時會互相搶裝置 —— 實測發生過 A 線的
# 驗證抓到 B 線的模擬器，force-stop 掉 B 正在測的 app，兩邊結果都不可信。
# 序號一律可指定，並在偵測到多台時明確警告。
if [ -n "${RIME_SERIAL:-${ANDROID_SERIAL:-}}" ]; then
  SERIAL="${RIME_SERIAL:-$ANDROID_SERIAL}"
  "$ADB" -s "$SERIAL" get-state >/dev/null 2>&1 || fail "指定的裝置 $SERIAL 未連線"
else
  SERIAL="$("$ADB" devices | awk '/emulator-[0-9]+\tdevice/{print $1; exit}')"
  NDEV="$("$ADB" devices | grep -cE 'emulator-[0-9]+' || true)"
  if [ "${NDEV:-0}" -gt 1 ]; then
    echo "  [警告] 偵測到 $NDEV 台模擬器，自動選了 $SERIAL。"
    echo "         並行測試時請用 RIME_SERIAL=emulator-XXXX 指定，否則會互相干擾。"
  fi
fi
[ -n "$SERIAL" ] || fail "找不到已連線的模擬器"
pass "模擬器 $SERIAL"

# --- 2. 安裝 ----------------------------------------------------------------
if [ "${#APKS[@]}" -gt 0 ]; then
  step "2. 安裝 APK"
  for apk in "${APKS[@]}"; do
    [ -f "$apk" ] || fail "找不到 APK: $apk"
    adbs install -r -g "$apk" >/dev/null 2>&1 || fail "安裝失敗: $apk"
    pass "已安裝 $(basename "$apk")"
  done
fi

# --- 3. 啟用 IME -------------------------------------------------------------
step "3. 啟用輸入法"
if [ -n "$READY_LOG" ]; then
  # 讓 IME 從頭啟動一次,否則若它早就跑完初始化,logcat 裡不會再出現就緒訊息。
  adbs shell am force-stop "$IME_PKG" >/dev/null 2>&1 || true
  adbs logcat -c >/dev/null 2>&1 || true
  pass "已 force-stop $IME_PKG 並清空 logcat(--ready-log 模式)"
fi
adbs shell ime enable "$IME_ID" >/dev/null 2>&1 || true
adbs shell ime set "$IME_ID" >/dev/null 2>&1 || true
adbs shell ime list -s > "$OUT_DIR/ime_list.txt" 2>/dev/null || true
grep -q "^$IME_ID\$" "$OUT_DIR/ime_list.txt" \
  || fail "系統看不到 $IME_ID。檢查 manifest 是否同時具備:BIND_INPUT_METHOD 權限、android.view.InputMethod intent-filter、指向含至少一個 subtype 的 method.xml 的 android.view.im meta-data"
pass "系統看得到 $IME_ID"

# ⚠ 這一段是後來補的,而補的理由值得留著:
#
#   原本這裡只確認「ime list -s 裡有這個 id」,然後就印 **「已啟用並設為預設」**。
#   但 `ime set` 的錯誤被 `|| true` 吞掉了,而且「被列出來」跟「是目前的輸入法」
#   是兩件事。實際發生過:ime set 沒生效,目前的輸入法還是 Gboard
#   (mCurId=com.google.android.inputmethod.latin/...),而關卡照樣印綠燈,
#   一路等到 120 秒之後才報「鍵盤沒有出現」—— 訊息還指向「首次部署卡住」,
#   把人帶去查一個完全無關的方向。
#
#   **一句宣稱「設為預設」的 PASS,就要真的去讀回來確認。**
SET_OK=0
for i in 1 2 3 4 5; do
  adbs shell settings get secure default_input_method 2>/dev/null \
    | tr -d "\r" > "$OUT_DIR/default_ime.txt" || true
  if grep -qx "$IME_ID" "$OUT_DIR/default_ime.txt"; then SET_OK=1; break; fi
  adbs shell ime set "$IME_ID" >/dev/null 2>&1 || true
  sleep 1
done
[ "$SET_OK" -eq 1 ] \
  || fail "ime set 沒有生效:目前的預設輸入法是 $(cat "$OUT_DIR/default_ime.txt" 2>/dev/null),不是 $IME_ID"
pass "已設為預設輸入法(讀回 secure default_input_method 確認)"

# --- 4. 開啟輸入目標 ---------------------------------------------------------
step "4. 開啟輸入目標"
case "$TARGET" in
  testapp)
    adbs shell monkey -p dev.rime.imetest -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1 \
      || fail "無法啟動 dev.rime.imetest,先跑 scripts/build_testapp.sh"
    ;;
  settings)
    adbs shell am start -a android.settings.SETTINGS >/dev/null 2>&1 || true
    sleep 2
    adbs shell input tap 540 300 >/dev/null 2>&1 || true
    ;;
  *) fail "不支援的 --target '$TARGET'" ;;
esac
sleep 2
pass "已開啟"

# --- 5. 等待鍵盤(含首次部署)------------------------------------------------
# 首次啟動時 librime 要編譯 schema,可能要一分鐘以上,不能沿用 20 秒的等待。
step "5. 等待鍵盤出現(上限 ${DEPLOY_TIMEOUT}s,含首次 schema 部署)"
SHOWN=0
for i in $(seq 1 "$DEPLOY_TIMEOUT"); do
  adbs shell dumpsys input_method > "$OUT_DIR/input_method.txt" 2>/dev/null || true
  if grep -q "mIsInputViewShown=true" "$OUT_DIR/input_method.txt"; then SHOWN=1; break; fi
  # 每 10 秒補點一次輸入框。只點一次不夠:視窗剛開時點下去可能還沒 layout 完,
  # 而那一次錯過就再也沒有人去叫鍵盤了。
  [ $((i % 10)) -eq 5 ] && adbs shell input tap 540 300 >/dev/null 2>&1 || true
  # 順便盯著預設輸入法有沒有被系統換掉。被換掉的話再等下去是浪費 —— 而且
  # 最後那句「鍵盤沒有出現」會把人帶去查部署,方向完全錯。
  if [ $((i % 20)) -eq 10 ]; then
    NOW_IME="$(adbs shell settings get secure default_input_method 2>/dev/null | tr -d "\r")"
    if [ -n "$NOW_IME" ] && [ "$NOW_IME" != "$IME_ID" ]; then
      echo "  [INFO] 預設輸入法在等待期間變成 $NOW_IME,重新設定"
      adbs shell ime set "$IME_ID" >/dev/null 2>&1 || true
    fi
  fi
  [ $((i % 15)) -eq 0 ] && echo "  ...已等待 ${i}s(首次部署較慢屬正常)"
  sleep 1
done
if [ "$SHOWN" -ne 1 ]; then
  # 分辨兩件很不一樣的事:
  #   (a) 宿主 app 根本沒把鍵盤叫起來 → ImeTracker 會記 onFailed,
  #       而且輸入法進程完全不會被啟動。那是測試靶或宿主的問題。
  #   (b) 叫了、輸入法也起來了,但畫面沒出現 → 那才是我們的 bug。
  # 少了這一句,兩者在報告裡長得一模一樣。
  adbs logcat -d > "$OUT_DIR/logcat.txt" 2>/dev/null || true
  # ⚠ 不可寫成 `X=$(grep -c ... || echo 0)`:grep 沒命中時會**同時**輸出 "0"
  # 並回非零,於是 || 那邊再補一個 "0",變數變成兩行 "0\n0",後面的
  # `[ "$X" -eq 0 ]` 就炸成 "integer expression expected"。
  # 這個 bug 就長在我自己上一輪加的診斷裡,而它讓診斷本身變成雜訊。
  count_in_log() {   # count_in_log <pattern>
    local n
    n=$(grep -cE "$1" "$OUT_DIR/logcat.txt" 2>/dev/null) || n=0
    printf '%s' "${n:-0}"
  }
  REQ=$(count_in_log "ImeTracker.*onRequestShow")
  BAD=$(count_in_log "ImeTracker.*onFailed")
  IMEUP=$(count_in_log "$IME_PKG.*nativeloader|Start proc.*$IME_PKG")
  CUR_NOW="$(adbs shell settings get secure default_input_method 2>/dev/null | tr -d "\r")"
  echo "  [INFO] ImeTracker:請求 $REQ 次、失敗 $BAD 次;輸入法進程啟動跡象 $IMEUP 次"
  echo "  [INFO] 此刻的預設輸入法:$CUR_NOW(待測:$IME_ID)"
  [ "$CUR_NOW" = "$IME_ID" ] \
    || echo "  [INFO] 預設輸入法不是待測的那個 —— 系統把它換掉了,查這裡而不是查部署"
  if [ "$REQ" -gt 0 ] && [ "$BAD" -ge "$REQ" ] && [ "$IMEUP" -eq 0 ]; then
    echo "  [INFO] 每一次請求都失敗、而且輸入法進程從沒被啟動 —— 指向宿主/測試靶沒把鍵盤叫起來,不是輸入法本身"
  fi
  fail "鍵盤在 ${DEPLOY_TIMEOUT}s 內沒有出現。若是首次部署卡住,查 $OUT_DIR/logcat.txt 中 rime 的部署訊息"
fi
pass "鍵盤已顯示"

CUR_ID="$(grep -o 'mCurId=[^ ]*' "$OUT_DIR/input_method.txt" | head -1 | cut -d= -f2)"
[ "$CUR_ID" = "$IME_ID" ] || fail "目前綁定的是 $CUR_ID,不是待測的 $IME_ID"
pass "綁定的 IME 正確"

# --- 5b. 等待輸入法引擎就緒 -------------------------------------------------
# 鍵盤「畫出來了」不等於「可以打字了」。非同步初始化的 IME(本專案就是)
# 會先把鍵盤畫出來、上面寫著「正在編譯詞庫」,此時 session 還不存在,
# 送進去的按鍵會原封不動落到宿主應用。少了這一步,冷啟動必定假失敗。
if [ -n "$READY_LOG" ]; then
  step "5b. 等待輸入法引擎就緒(樣式:$READY_LOG)"
  READY=0
  for i in $(seq 1 "$DEPLOY_TIMEOUT"); do
    adbs logcat -d > "$OUT_DIR/logcat_ready.txt" 2>/dev/null || true
    if grep -Eq "$READY_LOG" "$OUT_DIR/logcat_ready.txt"; then READY=1; break; fi
    [ $((i % 15)) -eq 0 ] && echo "  ...已等待 ${i}s(首次部署較慢屬正常)"
    sleep 1
  done
  [ "$READY" -eq 1 ] || fail "在 ${DEPLOY_TIMEOUT}s 內沒等到就緒訊息。看 $OUT_DIR/logcat_ready.txt"
  pass "引擎已就緒"
fi

# --- 6. 清空 ----------------------------------------------------------------
step "6. 清空輸入框"
clear_field
pass "已清空"

# --- 7. 注入按鍵(這一步是重點)----------------------------------------------
step "7. 以實體按鍵事件注入「$KEYS」"
KEYCODES="$(keys_to_keycodes "$KEYS")" || fail "按鍵序列無法轉換"
echo "  keycodes: $KEYCODES"
# 全部包成單一 adb 呼叫,中間加延遲:逐次呼叫 adb 每個要 200-500ms,
# 對輸入法來說反而不像真人打字,也讓整體慢上一個量級。
CMD=""
for kc in $KEYCODES; do
  CMD="${CMD}input keyevent $kc; sleep $KEY_DELAY; "
done
adbs shell "$CMD" >/dev/null 2>&1 || fail "注入按鍵失敗"
sleep 1
pass "已送出 $(echo "$KEYCODES" | wc -w) 個按鍵事件"

# --- 8. 組字中狀態 -----------------------------------------------------------
step "8. 組字中的狀態"
"$EMU" shot "$OUT_DIR/01-composing.png" >/dev/null 2>&1 || true
COMPOSING="$(read_field || true)"
echo "  輸入框目前內容: '$COMPOSING'"
# 這裡不做硬斷言:不同 schema 的組字預覽差異很大(有的顯示拼音、有的顯示注音、
# 有的什麼都不顯示),做成硬性條件只會製造假失敗。留作除錯資訊與截圖。
if [ "$COMPOSING" = "$KEYS" ]; then
  echo "  [INFO] 輸入框內容與注入的字母完全相同。這可能是正常的組字預覽,"
  echo "         也可能代表按鍵**沒有被輸入法攔截**而直接上屏了。"
  echo "         最終判定看第 10 步。"
fi
pass "已記錄組字中狀態(截圖 01-composing.png)"

# --- 9. 選字 ----------------------------------------------------------------
SEL="$(select_keycode "$SELECT_KEY")" || fail "選字鍵無效"
if [ -n "$SEL" ]; then
  step "9. 選字($SELECT_KEY)"
  adbs shell input keyevent "$SEL" >/dev/null 2>&1 || fail "送出選字鍵失敗"
  sleep 1
  pass "已送出 $SEL"
else
  step "9. 選字(略過,--select none)"
fi

# --- 10. 最終斷言 ------------------------------------------------------------
step "10. 斷言輸入框內容"
ACTUAL="$(read_field || true)"
echo "  實際: '$ACTUAL'"
echo "  預期: '$EXPECT' ($EXPECT_MODE)"
"$EMU" shot "$OUT_DIR/02-committed.png" >/dev/null 2>&1 || true
adbs logcat -d > "$OUT_DIR/logcat.txt" 2>/dev/null || true

case "$EXPECT_MODE" in
  exact)
    [ "$ACTUAL" = "$EXPECT" ] || fail "內容不符。若實際內容就是注入的字母本身,代表按鍵沒有被輸入法消費(rs_process_key 一路回傳 false,或 JNI 沒接上 librime)"
    ;;
  contains)
    case "$ACTUAL" in
      *"$EXPECT"*) ;;
      *) fail "內容不含預期字串" ;;
    esac
    ;;
esac
pass "組字與上屏正確 —— librime 確實在工作"

echo
echo "============================================"
echo " 全部通過"
echo " artifact: $OUT_DIR"
echo "   01-composing.png  組字中"
echo "   02-committed.png  上屏後"
echo "   logcat.txt"
echo "============================================"
