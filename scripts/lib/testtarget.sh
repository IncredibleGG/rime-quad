# shellcheck shell=bash
#
# testtarget.sh — 把輸入測試靶叫到前景,並且**確認它真的拿到了視窗焦點**
#
# 呼叫端要先準備好:adbs()、SERIAL、OUT_DIR。本檔只印訊息與回傳狀態碼,
# 不自己 exit —— 要不要 fail 由呼叫端決定(兩支腳本的 fail() 不一樣)。
#
# ── 為什麼有這個檔(這是量出來的,不是猜的)──────────────────────────────
#
#   CI run 31310612204(main 525eafe)的模擬器車道,第 6 與 6b 關都紅在
#   「鍵盤在 120s 內沒有出現」。從那一輪的 artifact 讀到的事實是:
#
#     · logcat 裡 ImeTracker 只有**一次** onRequestShow,而且下一行就是
#       onFailed at PHASE_CLIENT_VIEW_SERVED。那一次是測試靶 onCreate 裡叫的。
#       測試靶在 onWindowFocusChanged(true) 之後還會再叫七次 —— 一次都沒出現,
#       所以 onWindowFocusChanged(true) **從頭到尾沒有被呼叫過**。
#     · dumpsys input_method 的 mStartInputHistory 裡**完全沒有 dev.rime.imetest**。
#       最後一筆是 com.google.android.apps.nexuslauncher(11:30:29,啟動測試靶
#       之前五秒),mFocusedWindowClient 的 pid 1164 也正是桌面那個進程。
#     · 同一時間 dev.rime.imetest 的進程活著而且一直在畫(EGL 每 500ms 一幀,
#       那是輸入游標在閃),ActivityTaskManager 也印了 Displayed。
#       也就是說它**看得見、在跑,卻沒有輸入焦點**。
#
#   結論:鍵盤沒出現是**結果**,原因是測試靶的視窗沒拿到輸入焦點。
#   視窗沒有焦點 → IMM 沒有 served view → showSoftInput 被整個丟掉。
#   舊訊息說「鍵盤沒有出現(最後綁定:Gboard)」會把人帶去查部署與綁定,
#   而那兩件事在同一輪都是好的(第 5 關的核心層測試是 PASS 的)。
#   這個專案已經因為這種指錯方向的訊息浪費過好幾輪,所以下面**一定**要
#   先問「焦點在誰身上」,再談鍵盤。
#
# ⚠ 這裡刻意不用「拉長逾時」或「多試幾次」當修法。一次請求都沒成功不是慢,
#   是根本沒發生。下面的重試總預算很短,而且每一次都做**不一樣的事**。

TT_PKG="${TT_PKG:-dev.rime.imetest}"
TT_ACTIVITY="${TT_ACTIVITY:-${TT_PKG}/.MainActivity}"
TT_INPUT_DESC="${TT_INPUT_DESC:-rime_test_input}"
# 每一次啟動之後盯焦點的秒數。短是故意的:焦點是瞬間的事,不是慢慢來的事。
TT_FOCUS_WAIT="${TT_FOCUS_WAIT:-6}"

# 最後一次量到的值。呼叫端在失敗訊息裡直接引用這些變數,不要自己再 dump 一次
# —— 再 dump 就是另一個時間點,說出來的可能已經不是失敗當下的狀態。
TT_FOCUS_NOW=""
TT_FOCUS_APP=""
TT_FIELD_XY=""
TT_NONCE="-"

# ── 量測 ──────────────────────────────────────────────────────────────────

# 目前拿到輸入焦點的視窗。grep 在裝置上跑,傳回來的只有一行。
# (dumpsys window 整份有好幾百 KB,不能每秒整份拉回來。)
tt_focus_now() {
  adbs shell "dumpsys window | grep -m1 mCurrentFocus" 2>/dev/null \
    | tr -d '\r' | sed -e 's/^[[:space:]]*//' -e 's/^mCurrentFocus=//' || true
}

tt_focused_app() {
  adbs shell "dumpsys window | grep -m1 mFocusedApp" 2>/dev/null \
    | tr -d '\r' | sed -e 's/^[[:space:]]*//' -e 's/^mFocusedApp=//' || true
}

# 從 "Window{bb406c5 u0 dev.rime.imetest/dev.rime.imetest.MainActivity}"
# 取出套件名。取不乾淨也沒關係 —— 下游只拿它做 case 比對與顯示。
tt_focus_pkg() {
  local s="${1:-}"
  s="${s#*u0 }"
  s="${s%\}}"
  s="${s%%/*}"
  s="${s%% *}"
  printf '%s' "$s"
}

# 焦點在測試靶身上嗎。
tt_has_focus() {
  TT_FOCUS_NOW="$(tt_focus_now)"
  case "$TT_FOCUS_NOW" in
    *"$TT_PKG"*) return 0 ;;
  esac
  return 1
}

# 輸入法框架此刻認定的輸入目標是誰。用的是呼叫端已經 dump 好的
# input_method.txt,所以不花額外的一次 dumpsys。
# 這是 mStartInputHistory 的最後一筆 targetWin;沒有任何一筆就回空字串
# —— 「一筆都沒有」本身就是很強的訊號:框架連 startInput 都沒對它做過。
tt_ime_target_pkg() {
  local f="${1:-$OUT_DIR/input_method.txt}"
  [ -f "$f" ] || return 0
  sed -n 's/.*targetWin=[^[]*\[\([^]]*\)\].*/\1/p' "$f" | tail -1
}

# ── 現場保全 ──────────────────────────────────────────────────────────────
#
# 上一輪查不出原因,就是因為 artifact 裡沒有這幾份東西,只能靠猜。
# 失敗時一律留下來,下次不必再跑一輪才知道是誰蓋在上面。
# 同一次失敗常常會經過兩層(tt_focus_report 一次、fail() 又一次),而那兩次
# 拍到的是同一個畫面。30 秒內只留第一份 —— 重複的 2MB 截圖沒有任何資訊。
# ⚠ 跳過的時候一定要把 TT_LAST_DUMP_TAG 留成**真的存在**的那一個前綴。
#   訊息裡指向的檔案必須真的產生得出來 —— 這支腳本上一版就是因為
#   「叫人去看 logcat.txt,而那個檔只在成功路徑才寫」,害 CI 有一輪查不出原因。
TT_LAST_DUMP_AT=""
TT_LAST_DUMP_TAG=""
tt_dump_forensics() {
  local tag="${1:-focus}"
  [ -n "${SERIAL:-}" ] || return 0
  [ -d "${OUT_DIR:-}" ] || return 0
  if [ -n "$TT_LAST_DUMP_AT" ] && [ $((SECONDS - TT_LAST_DUMP_AT)) -lt 30 ]; then
    return 0
  fi
  TT_LAST_DUMP_AT="$SECONDS"
  TT_LAST_DUMP_TAG="$tag"
  adbs shell dumpsys window              > "$OUT_DIR/$tag-window.txt"     2>/dev/null || true
  adbs shell dumpsys activity activities > "$OUT_DIR/$tag-activities.txt" 2>/dev/null || true
  # 幾百 KB 裡最關鍵的那幾行單獨抽一份,免得又要在裡面找。
  adbs shell "dumpsys window | grep -nE 'mCurrentFocus|mFocusedApp|mFocusedWindow|imeInputTarget|imeControlTarget|imeLayeringTarget|mInputMethodTarget|Keyguard|mAwake|mScreenOnEarly'" \
      > "$OUT_DIR/$tag-focus.txt" 2>/dev/null || true
  adbs shell "dumpsys activity activities | grep -nE 'mResumedActivity|ResumedActivity|topResumedActivity|mFocusedApp|visible=true'" \
      >> "$OUT_DIR/$tag-focus.txt" 2>/dev/null || true
  # 當下畫面的節點樹。擋在上面的對話框長什麼樣、寫什麼字,只有這份看得到。
  if adbs shell "uiautomator dump /sdcard/rime_tt_forensic.xml >/dev/null 2>&1" >/dev/null 2>&1; then
    adbs pull /sdcard/rime_tt_forensic.xml "$OUT_DIR/$tag-screen.xml" >/dev/null 2>&1 || true
    adbs shell rm -f /sdcard/rime_tt_forensic.xml >/dev/null 2>&1 || true
  fi
  adbs exec-out screencap -p > "$OUT_DIR/$tag-screen.png" 2>/dev/null || true
  [ -s "$OUT_DIR/$tag-screen.png" ] || rm -f "$OUT_DIR/$tag-screen.png"
}

# 把量到的東西講出來,並保全現場。呼叫端接著自己 fail。
tt_focus_report() {
  local tag="${1:-focus}" imetgt
  [ -n "$TT_FOCUS_NOW" ] || TT_FOCUS_NOW="$(tt_focus_now)"
  [ -n "$TT_FOCUS_APP" ] || TT_FOCUS_APP="$(tt_focused_app)"
  echo "  [INFO] 焦點視窗       : ${TT_FOCUS_NOW:-<沒有任何視窗有焦點>}"
  echo "  [INFO] 前景 Activity  : ${TT_FOCUS_APP:-<無>}"
  echo "  [INFO] 應該是         : $TT_PKG"
  # 只有在真的有 dump 過 input_method 的時候才講這件事 —— 檔案不存在時
  # 「一筆都沒有」是我們自己還沒去看,不是框架沒做,講出來就是誤導。
  if [ -f "$OUT_DIR/input_method.txt" ]; then
    imetgt="$(tt_ime_target_pkg)"
    if [ -z "$imetgt" ]; then
      echo "  [INFO] 輸入法框架的 startInput 紀錄裡一筆都沒有 —— 框架從來沒把任何視窗當成輸入目標"
    elif [ "$imetgt" != "$TT_PKG" ]; then
      echo "  [INFO] 輸入法框架最後一次 startInput 的目標是 $imetgt,不是測試靶"
    fi
  fi
  # 測試靶自己講的話。它在 onWindowFocusChanged 會印一行,沒有那一行就代表
  # 它的視窗真的沒拿到焦點(這是客戶端事實,和 dumpsys 各自獨立)。
  if [ -f "$OUT_DIR/logcat.txt" ]; then
    local wf
    wf="$(grep -c "RimeImeTest.*windowFocus=true" "$OUT_DIR/logcat.txt" 2>/dev/null)" || wf=0
    echo "  [INFO] 測試靶回報「我的視窗拿到焦點了」的次數:${wf:-0}"
  fi
  tt_dump_forensics "$tag"
  local p="${TT_LAST_DUMP_TAG:-$tag}"
  echo "  [INFO] 現場已存:$OUT_DIR/$p-window.txt(dumpsys window)、$p-activities.txt、$p-focus.txt(只有關鍵幾行)、$p-screen.xml、$p-screen.png"
}

# ── 啟動 ──────────────────────────────────────────────────────────────────

# NEW_TASK(0x10000000) | RESET_TASK_IF_NEEDED(0x00200000) | CLEAR_TOP(0x04000000)
# CLEAR_TOP 讓重試時整個 Activity 重來一次(重新 onCreate、重新要鍵盤),
# 而不是回到一個已經處於奇怪狀態的舊實例。
TT_START_FLAGS="${TT_START_FLAGS:-0x14200000}"

tt_start() {
  adbs shell am start -W -n "$TT_ACTIVITY" \
      -a android.intent.action.MAIN -c android.intent.category.LAUNCHER \
      -f "$TT_START_FLAGS" --es tt_nonce "$TT_NONCE" "$@" \
      > "$OUT_DIR/am-start.txt" 2>&1 || true
}

tt_wait_focus() {
  local budget="${1:-$TT_FOCUS_WAIT}" i
  for i in $(seq 1 "$budget"); do
    tt_has_focus && return 0
    sleep 1
  done
  return 1
}

# 擋在最上面的那個東西,能不能關掉。
# 桌面、SystemUI、待測輸入法、測試靶自己都不能關 —— 關了只會讓畫面更亂,
# 而且會把「誰擋住」這個證據一起毀掉。
tt_stop_blocker() {
  local pkg ime="${IME_PKG:-__none__}"
  pkg="$(tt_focus_pkg "${TT_FOCUS_NOW:-}")"
  case "$pkg" in
    ''|*"$TT_PKG"*|android|com.android.systemui|com.android.launcher*|*launcher*|*Launcher*|"$ime")
      echo "  [INFO] 擋住的是 '${pkg:-未知}',不該把它關掉,改成再啟動一次測試靶"
      return 0 ;;
  esac
  echo "  [INFO] 把擋在最上面的 $pkg 關掉(它不是待測的東西,也不是桌面)"
  adbs shell am force-stop "$pkg" >/dev/null 2>&1 || true
  sleep 1
}

# 把測試靶叫到前景並拿到焦點。
# 回傳 0 = 拿到了;1 = 沒拿到(TT_FOCUS_NOW / TT_FOCUS_APP 是失敗當下的值)。
#
# 四次嘗試,每一次做**不一樣**的事,總預算約 30 秒:
#   1. 直接啟動
#   2. 送一次 BACK(把擋在上面的對話框收掉)再啟動
#   3. 關系統對話框 + 回桌面(把半開的面板清掉)再啟動
#   4. 關掉擋住的那個 app(桌面與待測物除外)再啟動
tt_acquire_foreground() {
  local attempt
  TT_NONCE="tt-$$-$(date +%s)"
  # 螢幕先亮著、鎖屏先解掉。模擬器剛開機時這兩件事都可能還沒做,
  # 而螢幕沒亮的時候沒有任何視窗會拿到輸入焦點。
  adbs shell input keyevent KEYCODE_WAKEUP >/dev/null 2>&1 || true
  adbs shell wm dismiss-keyguard >/dev/null 2>&1 || true
  for attempt in 1 2 3 4; do
    case "$attempt" in
      1) : ;;
      2) echo "  [INFO] 第 1 次啟動後焦點在 ${TT_FOCUS_NOW:-未知};送一次 BACK 把擋住的東西收掉再試"
         adbs shell input keyevent KEYCODE_BACK >/dev/null 2>&1 || true
         sleep 1 ;;
      3) echo "  [INFO] 焦點還在 ${TT_FOCUS_NOW:-未知};關掉系統對話框、回桌面,再啟動一次"
         adbs shell am broadcast -a android.intent.action.CLOSE_SYSTEM_DIALOGS >/dev/null 2>&1 || true
         adbs shell input keyevent KEYCODE_HOME >/dev/null 2>&1 || true
         sleep 1 ;;
      4) tt_stop_blocker ;;
    esac
    tt_start "$@"
    tt_wait_focus && return 0
  done
  TT_FOCUS_APP="$(tt_focused_app)"
  return 1
}

# ── 輸入框 ────────────────────────────────────────────────────────────────
#
# 座標一律從 uiautomator 的節點樹量,不寫死。
# 舊版寫死的是 540 300 —— 那是「1080 寬、輸入框在上面」時湊出來的數字,
# 換一個解析度、換一個版面、或者前景根本不是測試靶時,它就是在戳空氣,
# 而且戳到別人身上還會製造新的假失敗。
tt_field_center() {
  adbs shell "uiautomator dump /sdcard/rime_tt.xml >/dev/null 2>&1" >/dev/null 2>&1 || return 1
  adbs pull /sdcard/rime_tt.xml "$OUT_DIR/target-ui.xml" >/dev/null 2>&1 || return 1
  adbs shell rm -f /sdcard/rime_tt.xml >/dev/null 2>&1 || true
  python3 - "$OUT_DIR/target-ui.xml" "$TT_INPUT_DESC" "$TT_PKG" <<'PY'
import re, sys, xml.etree.ElementTree as ET
path, desc, pkg = sys.argv[1], sys.argv[2], sys.argv[3]
try:
    root = ET.parse(path).getroot()
except Exception:
    sys.exit(1)
BOUNDS = re.compile(r"\[(-?\d+),(-?\d+)\]\[(-?\d+),(-?\d+)\]")
best = None
for n in root.iter("node"):
    if n.get("package") != pkg:
        continue
    if "EditText" not in n.get("class", ""):
        continue
    m = BOUNDS.match(n.get("bounds", ""))
    if not m:
        continue
    x1, y1, x2, y2 = map(int, m.groups())
    if x2 - x1 < 8 or y2 - y1 < 8:
        continue
    cand = ((x1 + x2) // 2, (y1 + y2) // 2, n.get("focused") == "true")
    if n.get("content-desc", "") == desc:
        best = cand
        break
    if best is None:
        best = cand
if best is None:
    sys.exit(2)
print(best[0], best[1], "1" if best[2] else "0")
PY
}

# 確認輸入框真的拿到了游標。拿不到就點它的**實際**座標,再讀回來確認。
# 「點下去之後焦點真的到了那個欄位」才算數 —— 只點不確認等於沒點。
# 回傳 0 = 欄位有游標;1 = 節點樹裡沒有測試靶的輸入框(畫面上不是它);
#      2 = 有這個框但點了三次都拿不到游標。
tt_focus_field() {
  local i out x y f
  for i in 1 2 3; do
    out="$(tt_field_center)" || out=""
    if [ -z "$out" ]; then
      TT_FIELD_XY=""
      return 1
    fi
    # shellcheck disable=SC2086
    set -- $out
    x="$1"; y="$2"; f="$3"
    TT_FIELD_XY="$x,$y"
    [ "$f" = "1" ] && return 0
    adbs shell input tap "$x" "$y" >/dev/null 2>&1 || true
    sleep 1
  done
  return 2
}

# 等鍵盤的迴圈裡拿來補戳一下用的。座標用量到的那一組,量不到就重量一次。
tt_nudge_field() {
  local x y
  if [ -n "$TT_FIELD_XY" ]; then
    x="${TT_FIELD_XY%,*}"; y="${TT_FIELD_XY#*,}"
    adbs shell input tap "$x" "$y" >/dev/null 2>&1 || true
    return 0
  fi
  tt_focus_field >/dev/null 2>&1 || true
}
