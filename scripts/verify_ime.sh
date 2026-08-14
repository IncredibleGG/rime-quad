#!/usr/bin/env bash
#
# verify_ime.sh — 端到端驗證某個輸入法在模擬器上「真的能用」
#
# 流程:
#   1. 確保模擬器已啟動且開機完成
#   2. (可選)安裝待測 IME 的 APK
#   3. 確認 IME 出現在 `ime list -a` 中
#   4. 啟用並設為預設輸入法
#   5. 開啟一個有輸入框的畫面(預設用本專案自建的 dev.rime.imetest)
#   6. 等待軟鍵盤真的彈出(檢查 dumpsys input_method 與 InputMethod 視窗歸屬)
#   7. 截圖(鍵盤畫面)
#   8. 用 adb input 注入文字與 keyevent
#   9. 從 uiautomator dump 讀回輸入框內容,比對是否相符
#  10. 再截圖(含文字)
#
# 用法:
#   ./scripts/verify_ime.sh --apk app.apk                # 驗**本產品**的輸入法
#   ./scripts/verify_ime.sh --ime <pkg>/<service> --apk app.apk
#   ./scripts/verify_ime.sh --ime ... --text "nihao" --target settings
#
# 選項:
#   --ime <id>       要驗證的 IME id,格式 <package>/<service>
#                    預設 = scripts/lib/product.env 推導出來的本產品 IME id
#                    (從前預設是 Gboard —— 見下面 IME_ID 那一行的註解)
#   --apk <path>     驗證前先安裝這個 APK(可重複指定)
#   --text <str>     要注入的文字(預設 2024 —— 見下面 INPUT_TEXT 的註解:
#                    預設 IME 換成本產品之後,拉丁字串會真的走進 librime)
#   --expect <str>   預期輸入框內容(預設等於 --text)
#   --target <t>     輸入目標:testapp(預設,本專案自建)| settings(系統設定搜尋框)
#   --out <dir>      截圖與 artifact 輸出目錄(預設 <專案>/build/verify)
#   --no-start       不自動啟動模擬器(假設已在跑)
#   --restore-ime    驗證結束後把預設輸入法還原回原本的
#
# 離開碼:0 全部通過,非 0 表示某一步失敗(訊息會指出卡在哪)。

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
EMU="$SCRIPT_DIR/emu.sh"

ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Android/Sdk}}"
export ANDROID_SDK_ROOT
export ANDROID_HOME="$ANDROID_SDK_ROOT"
ADB="$ANDROID_SDK_ROOT/platform-tools/adb"
# shellcheck source=lib/device.sh
. "$SCRIPT_DIR/lib/device.sh"
# shellcheck source=lib/product.sh
. "$SCRIPT_DIR/lib/product.sh"
# ⛔ 這一支從前寫死 `emulator-${RIME_EMU_PORT:-5554}`,連 RIME_SERIAL 都不看,
#   也沒有 --serial —— 帶了也沒用。
#
# ⛔⛔ 2026-08-13:上一版把這裡改成 `SERIAL=""` 並加了 `--serial`,註解寫著
#     「現在三個入口都有」,而**全檔沒有一處呼叫 `rs_pick_serial`** ——
#     `RIME_SERIAL` / `ANDROID_SERIAL` 依然被完全忽略。實測(帶齊
#     `RIME_SERIAL=emulator-5558 RIME_EMU_PORT=5558 RIME_AVD=lumina_test2`
#     跑 `--no-start`):第 1 關過,然後 `set -e` 在第一個 `adbs` 當場結束,
#     RC=1 而**一個字都沒印** —— 因為送出去的是 `adb -s "" shell`。
#     「加了旗標卻沒有人讀」與「完全沒加」在畫面上長得一模一樣。
#     選序號的實作只有 lib/device.sh 那一份,下面那一行就是本檔唯一的入口。
SERIAL=""

# --------------------------------------------------------------- 參數解析 ---

# ⛔ 這裡的預設值從第一個 commit(723ea72)起就是 **Gboard**,而這一支是
#   「輸入法真的能用」的守門。實測 2026-08-13:裝上我們的 APK → 把系統預設
#   輸入法**設成 Gboard** → 用 `input text` 打字(走 commitText,繞過組字)
#   → 12 關全 PASS → RC=0。三輪覆核都沒有人發現,因為輸出裡每一行都是綠的。
#   更糟的是第 4 關會把**共用模擬器的預設輸入法換掉**,別條線接下來整輪都紅。
#
#   `input text` 走 commitText,連 librime 有沒有載入都驗不到 —— 拿它去驗
#   Gboard,等於驗「Android 的 IMF 還會不會動」。
#
#   預設改成本產品的 IME id(唯一來源:scripts/lib/product.env)。
#   真要拿 Gboard 當對照組,明著寫 `--ime com.google.android.inputmethod.latin/...`。
IME_ID="$RS_ANDROID_IME_ID"
# ⛔ 預設的注入文字**跟著上面那一行一起改**。
#   `hello.rime` 是給 Gboard(拉丁輸入法)寫的:一送一收、逐字相等。
#   換成本產品之後,`adb shell input text` 送進來的按鍵會**真的走進 librime**,
#   實測(emulator-5558/lumina_test2)輸入框拿到的是「和。ri me」——
#   那不是缺陷,那正是一個中文輸入法該做的事;說謊的是預設的預期值。
#
#   所以預設改成 `2024`:沒有在組字時數字原樣上屏(實測逐字相等),
#   於是這一支問的仍然是它檔頭寫的那件事 ——
#   「系統認不認得它、鍵盤彈不彈得出來、送進去的字到不到得了宿主輸入框」。
#
# ⚠ **這一支驗不到組字。** `input text` 走的是按鍵注入,而「打 nihao 選第一個
#   會不會上屏你好」由 `scripts/verify_rime_compose.sh` 回答
#   (`release_check.sh` 第 6 關就是它,而且註解已經寫明不可以拿這一支代替)。
INPUT_TEXT="2024"
EXPECT_TEXT=""
TARGET="testapp"
OUT_DIR="$PROJECT_ROOT/build/verify"
AUTO_START=1
RESTORE_IME=0
APKS=()

while [ $# -gt 0 ]; do
  case "$1" in
    --ime)     IME_ID="$2"; shift 2 ;;
    --apk)     APKS+=("$2"); shift 2 ;;
    --text)    INPUT_TEXT="$2"; shift 2 ;;
    --expect)  EXPECT_TEXT="$2"; shift 2 ;;
    --target)  TARGET="$2"; shift 2 ;;
    --out)     OUT_DIR="$2"; shift 2 ;;
    --no-start)    AUTO_START=0; shift ;;
    --restore-ime) RESTORE_IME=1; shift ;;
    --serial)  SERIAL="$2"; shift 2 ;;
    -h|--help) sed -n '2,45p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "未知選項:$1" >&2; exit 2 ;;
  esac
done
[ -n "$EXPECT_TEXT" ] || EXPECT_TEXT="$INPUT_TEXT"

# --serial 優先;沒給就走唯一的那份實作(RIME_SERIAL / ANDROID_SERIAL,
# 兩個都沒有而且**恰好一台**在線時才自動選)。
[ -x "$ADB" ] || { echo "找不到 adb:$ADB" >&2; exit 2; }
# ⛔ `--serial` 也要算「指名」。閘從前只看環境變數,於是這一行帶 `--serial`
#   就必死(RC=2,訊息說「是自動選來的」而那台正是命令列指名的)。
#   `rs_select_device` 把來源(flag / env / auto)一起記下來給閘看。
rs_select_device "$ADB" "$SERIAL" || exit 2
SERIAL="$RS_SERIAL"

# ⚠ 這一支不只用 adb,還會轉呼叫 `emu.sh`(start / status / install / shot /
#   ime-enable),而 `emu.sh` 是**從 RIME_EMU_PORT 自己組序號**的。兩邊各自
#   決定的話,`--serial emulator-5558` 配上環境裡殘留的 `RIME_EMU_PORT=5554`
#   就會一半打在這台、一半打在那台,而輸出看起來一切正常。
#   所以這裡把 port 由**選定的序號**推導出來並 export,兩邊只剩一個真相。
case "$SERIAL" in
  emulator-*)
    _WANT_PORT="${SERIAL#emulator-}"
    if [ -n "${RIME_EMU_PORT:-}" ] && [ "$RIME_EMU_PORT" != "$_WANT_PORT" ]; then
      echo "RIME_EMU_PORT=$RIME_EMU_PORT 與選定的 $SERIAL 對不上 —— 中止(不要讓兩支腳本打不同的機器)。" >&2
      exit 2
    fi
    export RIME_EMU_PORT="$_WANT_PORT"
    ;;
  *)
    # 實體機:emu.sh 管不到它,所以這一支只能在 --no-start 之下跑。
    if [ "$AUTO_START" -eq 1 ]; then
      echo "$SERIAL 不是模擬器,emu.sh 起不動它。請加 --no-start。" >&2
      exit 2
    fi
    ;;
esac

IME_PKG="${IME_ID%%/*}"

STEP=0
pass() { STEP=$((STEP+1)); echo "  [PASS] $*"; }
fail() { echo "  [FAIL] $*" >&2; echo; echo "驗證失敗。已產生的 artifact 在:$OUT_DIR" >&2; exit 1; }
step() { echo; echo "=== $* ==="; }

adbs() { "$ADB" -s "$SERIAL" "$@"; }

# --------------------------------------------------------------- 主流程 ---

mkdir -p "$OUT_DIR"
echo "驗證輸入法 : $IME_ID"
echo "輸入目標   : $TARGET"
echo "注入文字   : $INPUT_TEXT"
echo "輸出目錄   : $OUT_DIR"

# --- 1. 模擬器 ---------------------------------------------------------------
step "1. 模擬器狀態"
if [ "$AUTO_START" -eq 1 ]; then
  "$EMU" start
else
  "$EMU" status >/dev/null || fail "模擬器未在執行(而且指定了 --no-start)"
fi
"$EMU" status
pass "模擬器開機完成"

ORIG_IME="$(adbs shell settings get secure default_input_method 2>/dev/null | tr -d '\r')"

# ⛔ 閘要在 `cleanup()` **定義之前**，不是在第 215 行。
#   `cleanup` 掛在 EXIT trap 上，也就是**任何**一條早退路徑都會跑到它，
#   包含第 215 行那道閘還沒執行的那些（例如安裝失敗）。而它做的
#   `ime set` 是破壞性動作：打在別條線的模擬器上，那條線接下來整輪都是紅的。
#   放在這裡兩件事一起成立：早退時已經過閘，而正常路徑上第 215 行那一道
#   仍然在原地（訊息不同、問的是同一件事，重複問不花成本）。
rs_assert_destructive_ok "$ADB" "$SERIAL" "ime set（EXIT trap 還原預設輸入法）" || exit 2

cleanup() {
  if [ "$RESTORE_IME" -eq 1 ] && [ -n "${ORIG_IME:-}" ] && [ "$ORIG_IME" != "null" ]; then
    echo "還原預設輸入法為 $ORIG_IME"
    adbs shell ime set "$ORIG_IME" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

# --- 2. 安裝 APK -------------------------------------------------------------
if [ "${#APKS[@]}" -gt 0 ]; then
  step "2. 安裝待測 APK"
  "$EMU" install "${APKS[@]}"
  pass "APK 安裝完成"
else
  step "2. 安裝待測 APK(未指定 --apk,略過)"
fi

# 「這一輪跑在哪一台、量的是哪一份 APK」—— 沒有它,綠燈事後無法覆核。
#
# ⛔ **裝完才寫。** 這一行從前在第 1 關(安裝之前),於是 `rs_write_device_stamp`
#   靠 `pm path` ＋ 裝置端 sha256 量到的 `pkg_apk_sha256` 是**上一次裝的那一份**
#   —— artifact 指著的是舊 APK,而這一輪量的是新的。eb3c588 的 commit 標題
#   就是「device.txt 要裝完才寫」,漏了這一支。
rs_write_device_stamp "$ADB" "$SERIAL" "$OUT_DIR/device.txt" "${APKS[0]:-}" "$IME_PKG"

# --- 3. IME 是否被系統看見 ---------------------------------------------------
step "3. 系統是否認得這個 IME"
adbs shell ime list -a -s | tr -d '\r' > "$OUT_DIR/ime-list.txt"
echo "系統目前可用的輸入法:"
sed 's/^/    /' "$OUT_DIR/ime-list.txt"
grep -qxF "$IME_ID" "$OUT_DIR/ime-list.txt" \
  || fail "IME '$IME_ID' 不在系統清單中。若這是自家 APK,請確認:(a) APK 已安裝 (b) manifest 有 BIND_INPUT_METHOD 權限與 android.view.InputMethod intent-filter (c) 有 method.xml meta-data"
pass "IME 已被 InputMethodManagerService 認得"

# --- 4. 啟用並設為預設 -------------------------------------------------------
step "4. 啟用並設為預設輸入法"
# ⚠ `ime set` 會改掉這台機器的系統預設輸入法 —— 打在別條線的模擬器上,
#   那條線接下來整輪都是紅的而且查不出為什麼。自動選來的那一台不准被改。
rs_assert_destructive_ok "$ADB" "$SERIAL" "ime enable / ime set" || exit 2
"$EMU" ime-enable "$IME_ID"
CUR="$(adbs shell settings get secure default_input_method | tr -d '\r')"
[ "$CUR" = "$IME_ID" ] || fail "預設輸入法設定失敗(目前是 $CUR)"
pass "預設輸入法 = $IME_ID"

# --- 5. 開啟輸入目標 ---------------------------------------------------------
step "5. 開啟有輸入框的畫面"
case "$TARGET" in
  testapp)
    TEST_PKG="dev.rime.imetest"
    if ! adbs shell pm path "$TEST_PKG" >/dev/null 2>&1; then
      echo "測試靶 App 尚未安裝,現在建置…"
      TEST_APK="$("$SCRIPT_DIR/build_testapp.sh" | tail -1)"
      "$EMU" install "$TEST_APK"
    fi
    adbs shell am force-stop "$TEST_PKG" >/dev/null 2>&1 || true
    adbs shell am start -W -n "$TEST_PKG/.MainActivity" >/dev/null
    ;;
  settings)
    adbs shell am start -a com.android.settings.action.SETTINGS_SEARCH >/dev/null
    ;;
  *)
    fail "未知的 --target:$TARGET(可用:testapp、settings)"
    ;;
esac
sleep 2
pass "輸入畫面已開啟"

# --- 6. 軟鍵盤是否真的彈出 ---------------------------------------------------
step "6. 軟鍵盤是否彈出"
SHOWN=0
for i in $(seq 1 20); do
  adbs shell dumpsys input_method > "$OUT_DIR/input_method.txt" 2>/dev/null || true
  if grep -q "mIsInputViewShown=true" "$OUT_DIR/input_method.txt"; then SHOWN=1; break; fi
  # 有些畫面需要點一下輸入框才會叫出鍵盤
  [ "$i" -eq 5 ] && adbs shell input tap 540 300 >/dev/null 2>&1 || true
  sleep 1
done
[ "$SHOWN" -eq 1 ] || fail "軟鍵盤沒有彈出(mIsInputViewShown 一直是 false)。詳見 $OUT_DIR/input_method.txt"
pass "mIsInputViewShown=true"

CUR_ID="$(grep -o 'mCurId=[^ ]*' "$OUT_DIR/input_method.txt" | head -1 | cut -d= -f2)"
[ "$CUR_ID" = "$IME_ID" ] || fail "目前綁定的 IME 是 $CUR_ID,不是待測的 $IME_ID"
pass "目前綁定的 IME 就是待測的 IME"

# 進一步確認畫面上那個 InputMethod 視窗確實屬於待測 IME 的 package
adbs shell dumpsys window windows > "$OUT_DIR/window.txt" 2>/dev/null || true
if grep -q "InputMethod" "$OUT_DIR/window.txt"; then
  if grep -A2 "u0 InputMethod}" "$OUT_DIR/window.txt" | grep -q "package=$IME_PKG"; then
    pass "InputMethod 視窗屬於 $IME_PKG"
  else
    echo "  [WARN] 找到 InputMethod 視窗但無法確認 package 為 $IME_PKG(不視為失敗)"
  fi
else
  echo "  [WARN] dumpsys window 中沒看到 InputMethod 視窗(不視為失敗)"
fi

# --- 7. 截圖(鍵盤) ---------------------------------------------------------
step "7. 截圖(鍵盤畫面)"
"$EMU" shot "$OUT_DIR/01-keyboard.png"
pass "已存 $OUT_DIR/01-keyboard.png"

read_field() {
  adbs shell "uiautomator dump /sdcard/rime_ui.xml >/dev/null 2>&1" || return 1
  adbs pull /sdcard/rime_ui.xml "$OUT_DIR/ui.xml" >/dev/null 2>&1 || return 1
  adbs shell rm -f /sdcard/rime_ui.xml >/dev/null 2>&1 || true
  python3 - "$OUT_DIR/ui.xml" <<'PY'
import sys, xml.etree.ElementTree as ET
root = ET.parse(sys.argv[1]).getroot()
best = ""
for n in root.iter("node"):
    cls = n.get("class", "")
    if "EditText" not in cls:
        continue
    txt = n.get("text", "")
    if n.get("focused") == "true":
        print(txt); sys.exit(0)
    if txt and not best:
        best = txt
print(best)
PY
}

# 有些畫面(例如系統設定搜尋)會留著上一次的內容,先清空再測,
# 否則比對結果會被殘留文字汙染。
clear_field() {
  local cur n
  cur="$(read_field || true)"
  n="${#cur}"
  [ "$n" -eq 0 ] && return 0
  adbs shell input keyevent KEYCODE_MOVE_END >/dev/null 2>&1 || true
  # 一次送多個 keycode 比逐次呼叫 adb 快很多;多按幾下確保清乾淨
  local codes="" i
  for i in $(seq 1 $((n + 5))); do codes="$codes 67"; done
  adbs shell input keyevent $codes >/dev/null 2>&1 || true
}

# --- 8. 清空輸入框並注入文字 -------------------------------------------------
step "8. 清空輸入框並注入文字"
clear_field
LEFTOVER="$(read_field || true)"
# 註:uiautomator 在輸入框為空時會回報 hint 文字(例如 "type here"),
#     所以這裡不是空字串未必代表沒清乾淨,僅作為除錯資訊印出。
[ -z "$LEFTOVER" ] || echo "  [INFO] 清空後讀到 '$LEFTOVER'(空白輸入框會回報 hint 文字,屬正常)"
# adb input text 不接受空白,用 %s 代替
ESCAPED="$(printf '%s' "$INPUT_TEXT" | sed 's/ /%s/g')"
adbs shell input text "$ESCAPED"
sleep 2
pass "已送出 input text"

# --- 9. 讀回輸入框內容 -------------------------------------------------------
step "9. 讀回輸入框內容並比對"
ACTUAL=""
for i in $(seq 1 10); do
  ACTUAL="$(read_field || true)"
  [ -n "$ACTUAL" ] && break
  sleep 1
done
echo "輸入框內容: '$ACTUAL'"
echo "預期內容  : '$EXPECT_TEXT'"
case "$ACTUAL" in
  *"$EXPECT_TEXT"*) pass "文字已確實進入輸入框" ;;
  *) fail "輸入框內容與預期不符(實際 '$ACTUAL',預期含 '$EXPECT_TEXT')" ;;
esac

# --- 10. keyevent(退格)------------------------------------------------------
step "10. keyevent 測試(退格)"
adbs shell input keyevent KEYCODE_DEL
sleep 1
AFTER_DEL="$(read_field || true)"
echo "退格後內容: '$AFTER_DEL'"
if [ "${#AFTER_DEL}" -lt "${#ACTUAL}" ]; then
  pass "KEYCODE_DEL 生效(長度 ${#ACTUAL} -> ${#AFTER_DEL})"
else
  echo "  [WARN] 退格後長度沒變小,可能該 IME 自行處理了 DEL(不視為失敗)"
fi

# --- 11. 最終截圖 ------------------------------------------------------------
step "11. 截圖(含文字)"
"$EMU" shot "$OUT_DIR/02-typed.png"
pass "已存 $OUT_DIR/02-typed.png"

# --- 12. 收集 logcat ---------------------------------------------------------
step "12. 收集 IME 相關 logcat"
adbs logcat -d -v time > "$OUT_DIR/logcat-full.txt" 2>/dev/null || true
grep -iE "$IME_PKG|InputMethodManagerService|rime" "$OUT_DIR/logcat-full.txt" > "$OUT_DIR/logcat-ime.txt" 2>/dev/null || true
CRASHES="$(grep -cE "FATAL EXCEPTION|ANR in $IME_PKG" "$OUT_DIR/logcat-full.txt" || true)"
if [ "${CRASHES:-0}" -gt 0 ]; then
  echo "  [WARN] logcat 中有 $CRASHES 筆 FATAL/ANR,請檢查 $OUT_DIR/logcat-full.txt"
else
  pass "logcat 中沒有 FATAL EXCEPTION / ANR"
fi

echo
echo "============================================"
echo "驗證通過:$IME_ID"
echo "Artifact:$OUT_DIR"
echo "  01-keyboard.png  鍵盤彈出畫面"
echo "  02-typed.png     輸入文字後畫面"
echo "  ime-list.txt / input_method.txt / window.txt / logcat-*.txt"
echo "============================================"
