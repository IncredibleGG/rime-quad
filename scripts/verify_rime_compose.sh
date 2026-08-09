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
#       --ime "$RS_ANDROID_IME_ID" \      # 值見 scripts/lib/product.env
#       --apk android/app/build/outputs/apk/debug/app-debug.apk \
#       --keys nihao --select space --expect 你好
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
EMU="$HERE/emu.sh"
# 產品識別碼的唯一來源,見 scripts/lib/product.env。這支腳本的 --ime 是必填,
# 讀進來是為了 logcat 的過濾字樣(拿產品名去撈 log 的那一行)。
# shellcheck source=lib/product.sh
. "$HERE/lib/product.sh"
# 「把測試靶叫到前景並確認它真的有焦點」那一整套。檔頭寫了為什麼需要它。
# shellcheck source=lib/testtarget.sh
. "$HERE/lib/testtarget.sh"
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
  grep -Ei "rime|$RS_PRODUCT_NAME|${IME_PKG:-$RS_ANDROID_APP_ID}|deploy|ANR|FATAL|died" \
    "$OUT_DIR/logcat.txt" > "$OUT_DIR/logcat-rime.txt" 2>/dev/null || true
  # ⚠ 焦點現場也要留。上一輪(CI run 31310612204)查不出原因,就是因為
  #   artifact 裡只有 logcat 與 input_method,沒有 dumpsys window ——
  #   「那一刻前景到底是誰」只能猜,於是又浪費一輪。任何一種失敗都存。
  tt_dump_forensics fail
  echo "  [INFO] 已存 logcat:$OUT_DIR/logcat.txt($(wc -l < "$OUT_DIR/logcat.txt" 2>/dev/null || echo 0) 行)、logcat-rime.txt" >&2
  echo "  [INFO] 已存失敗當下的視窗現場:$OUT_DIR/${TT_LAST_DUMP_TAG}-window.txt、${TT_LAST_DUMP_TAG}-activities.txt、${TT_LAST_DUMP_TAG}-focus.txt、${TT_LAST_DUMP_TAG}-screen.xml、${TT_LAST_DUMP_TAG}-screen.png" >&2
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
# ⚠ 「啟動了」不等於「在前景」,更不等於「拿到焦點」。這三件事必須分開驗。
#
#   上一輪(CI run 31310612204)就死在這裡:測試靶啟動了、畫出來了、進程活著,
#   但視窗從頭到尾沒有輸入焦點(dumpsys input_method 的 mStartInputHistory 裡
#   一筆 dev.rime.imetest 都沒有,焦點一直在桌面 nexuslauncher 身上)。
#   沒有焦點 → IMM 沒有 served view → showSoftInput 直接被丟掉 →
#   120 秒後報「鍵盤沒有出現」,而那句話指向的是部署與輸入法,完全錯的方向。
#
#   所以這一關現在**先把焦點驗出來**,失敗就在這裡紅,並且說出焦點在誰身上。
step "4. 開啟輸入目標,並確認它真的在前景、真的有焦點"
case "$TARGET" in
  testapp)
    # 沒裝就別談焦點了。舊版是 monkey 失敗才報這句,改用 am start 之後
    # 「沒裝」與「叫不到前景」會長得一樣,所以先分開問。
    adbs shell pm list packages 2>/dev/null | tr -d "\r" | grep -qx "package:$TT_PKG" \
      || fail "裝置上沒有 $TT_PKG。先跑 scripts/build_testapp.sh,再 adb install 那個 APK。"
    if ! tt_acquire_foreground; then
      tt_focus_report focus
      fail "測試靶 $TT_PKG 沒有拿到視窗焦點,焦點在 ${TT_FOCUS_NOW:-<沒有任何視窗有焦點>}。
       這**不是**輸入法或首次部署的問題:視窗沒有焦點,IMM 就沒有 served view,
       showSoftInput 會被整個丟掉(logcat 裡是 ImeTracker onFailed at
       PHASE_CLIENT_VIEW_SERVED),鍵盤當然不會出現。
       先看 $OUT_DIR/${TT_LAST_DUMP_TAG}-screen.png 與 ${TT_LAST_DUMP_TAG}-window.txt:是誰蓋在上面。"
    fi
    pass "測試靶在前景且視窗有焦點($TT_FOCUS_NOW)"

    # 視窗有焦點還不夠 —— 游標必須真的在那個輸入框裡,IMM 才會有 served view。
    # 座標從 uiautomator 的節點樹量(不是寫死的 540 300),點完再讀回來確認
    # focused=true。點了不確認等於沒點。
    FF=0; tt_focus_field || FF=$?   # set -e:不能寫成 `cmd; FF=$?`,那會先中止
    case "$FF" in
      0) pass "輸入框拿到游標(uiautomator focused=true),實際座標 $TT_FIELD_XY" ;;
      1) tt_focus_report focus
         fail "畫面的節點樹裡找不到 $TT_PKG 的輸入框 —— 前景視窗雖然報是測試靶,
       畫面上卻不是它(可能有東西蓋在上面)。看 $OUT_DIR/${TT_LAST_DUMP_TAG}-screen.png。" ;;
      *) tt_focus_report focus
         fail "點了三次 $TT_FIELD_XY(輸入框的實際中心)之後,uiautomator 仍然說它沒有游標。
       焦點進不去欄位,鍵盤就不會被叫起來。看 $OUT_DIR/${TT_LAST_DUMP_TAG}-screen.xml。" ;;
    esac
    ;;
  settings)
    # 這條路徑 release_check.sh 沒有在用,維持原樣(它的靶是系統設定,
    # 不是我們的測試靶,tt_* 那一套的假設在這裡不成立)。
    adbs shell am start -a android.settings.SETTINGS >/dev/null 2>&1 || true
    sleep 2
    adbs shell input tap 540 300 >/dev/null 2>&1 || true
    sleep 2
    ;;
  *) fail "不支援的 --target '$TARGET'" ;;
esac
pass "已開啟"

# --- 5. 等待鍵盤(含首次部署)------------------------------------------------
# 首次啟動時 librime 要編譯 schema,可能要一分鐘以上,不能沿用 20 秒的等待。
step "5. 等待鍵盤出現(上限 ${DEPLOY_TIMEOUT}s,含首次 schema 部署)"
SHOWN=0
LOST_FOCUS=0
for i in $(seq 1 "$DEPLOY_TIMEOUT"); do
  adbs shell dumpsys input_method > "$OUT_DIR/input_method.txt" 2>/dev/null || true
  # ⚠ 「有鍵盤彈出來」不等於「彈出來的是我們的鍵盤」。
  #
  #   實際發生過:設定裡的預設輸入法確實已經是我們的(讀回確認過),但那個
  #   時間點宿主 app 綁定的還是前一個輸入法,於是 **Gboard 的鍵盤**彈了出來。
  #   舊寫法只等 mIsInputViewShown=true 就往下走,然後在下一行才發現
  #   mCurId 是 Gboard —— 等於用 Gboard 當作「鍵盤已顯示」的證據。
  #
  #   兩個條件要一起成立才算數。差別在於:分開檢查會把一個時序問題報成
  #   「綁定錯誤」,而人會去查 manifest;合起來檢查則會誠實地等到逾時,
  #   訊息也才指得對地方。
  if grep -q "mIsInputViewShown=true" "$OUT_DIR/input_method.txt"; then
    CUR_BOUND="$(grep -o "mCurId=[^ ]*" "$OUT_DIR/input_method.txt" | head -1 | cut -d= -f2)"
    if [ "$CUR_BOUND" = "$IME_ID" ]; then SHOWN=1; break; fi
    if [ $((i % 15)) -eq 0 ]; then
      echo "  ...鍵盤出現了但綁定的是 $CUR_BOUND,繼續等我們的"
    fi
  fi
  # 每 10 秒補點一次輸入框。只點一次不夠:視窗剛開時點下去可能還沒 layout 完,
  # 而那一次錯過就再也沒有人去叫鍵盤了。
  # 座標用第 4 關量到的那一組(輸入框的實際中心),不是寫死的 540 300。
  [ $((i % 10)) -eq 5 ] && tt_nudge_field || true
  # 每 15 秒確認焦點還在測試靶身上。
  # ⚠ 這不是「多等一會兒」,是**提早停損並指對方向**:焦點掉了之後再等下去,
  #   等到的一定是逾時,而逾時那句話會把人帶去查部署。連續三次(45 秒)
  #   都不在才算數,中途先自己搶救一次 —— 焦點在視窗動畫期間會短暫是別人的。
  if [ "$TARGET" = "testapp" ] && [ $((i % 15)) -eq 0 ]; then
    if tt_has_focus; then
      LOST_FOCUS=0
    else
      LOST_FOCUS=$((LOST_FOCUS + 1))
      echo "  [INFO] 焦點不在測試靶身上(第 $LOST_FOCUS 次):$TT_FOCUS_NOW"
      if [ "$LOST_FOCUS" -eq 1 ]; then
        adbs shell input keyevent KEYCODE_BACK >/dev/null 2>&1 || true
        tt_start
      elif [ "$LOST_FOCUS" -ge 3 ]; then
        tt_focus_report focus
        fail "測試靶在等鍵盤的期間失去了視窗焦點(連續 3 次都不是它),焦點在 $TT_FOCUS_NOW。
       有東西蓋到測試靶上面了。這不是鍵盤沒出現,是根本沒有人在等鍵盤 ——
       沒有焦點就沒有 served view,showSoftInput 一定被丟掉。
       看 $OUT_DIR/${TT_LAST_DUMP_TAG}-screen.png 與 ${TT_LAST_DUMP_TAG}-activities.txt。"
      fi
    fi
  fi
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
  LAST_BOUND="$(grep -o "mCurId=[^ ]*" "$OUT_DIR/input_method.txt" | head -1 | cut -d= -f2)"

  # ⚠ 順序很重要:**先問焦點,再談鍵盤**。
  #   「鍵盤沒有出現」是結果,不是原因。上一輪就是先講結果,於是所有人
  #   都跑去查部署與綁定 —— 而那一輪的部署與引擎都是好的(第 5 關 PASS)。
  if [ "$TARGET" = "testapp" ]; then
    IME_TGT="$(tt_ime_target_pkg "$OUT_DIR/input_method.txt")"
    WINFOCUS_LOG="$(grep -c "RimeImeTest.*windowFocus=true" "$OUT_DIR/logcat.txt" 2>/dev/null)" || WINFOCUS_LOG=0
    if ! tt_has_focus; then
      tt_focus_report focus
      fail "焦點不在測試靶身上,焦點在 ${TT_FOCUS_NOW:-<沒有任何視窗有焦點>}(應該是 $TT_PKG)。
       所以鍵盤當然沒出現 —— 這是**宿主端**的問題,不是輸入法、不是部署、
       也不是綁定。看 $OUT_DIR/${TT_LAST_DUMP_TAG}-screen.png:誰蓋在上面。"
    fi
    if [ "${WINFOCUS_LOG:-0}" -eq 0 ] || { [ -n "$IME_TGT" ] && [ "$IME_TGT" != "$TT_PKG" ]; }; then
      tt_focus_report focus
      fail "測試靶的視窗**從來沒有拿到過**輸入焦點:
       它自己回報 windowFocus=true 的次數是 ${WINFOCUS_LOG:-0},
       輸入法框架最後一次 startInput 的目標是 '${IME_TGT:-<一筆都沒有>}'(應該是 $TT_PKG)。
       視窗沒拿過焦點,IMM 就沒有 served view,showSoftInput 全部被丟掉
       (logcat:ImeTracker onFailed at PHASE_CLIENT_VIEW_SERVED)。
       這不是鍵盤的問題,是有東西擋著測試靶。看 $OUT_DIR/${TT_LAST_DUMP_TAG}-screen.png。"
    fi
  fi

  if grep -q "mIsInputViewShown=true" "$OUT_DIR/input_method.txt"; then
    fail "有鍵盤,但綁定的一直是 $LAST_BOUND,不是待測的 $IME_ID —— 系統沒有切換到我們的輸入法"
  fi
  fail "測試靶有焦點、輸入框有游標、也叫了 ${REQ:-?} 次 showSoftInput,鍵盤仍然在 ${DEPLOY_TIMEOUT}s 內沒有出現(最後綁定:$LAST_BOUND)。
       宿主端該做的都做了,這一次的嫌疑才真的在輸入法:查 $OUT_DIR/logcat.txt 中 rime 的部署訊息"
fi
pass "鍵盤已顯示"

# 迴圈的跳出條件已經包含綁定正確,這裡是再確認一次(dumpsys 是同一份快照)。
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
