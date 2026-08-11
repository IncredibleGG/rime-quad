#!/usr/bin/env bash
#
# windows/verify_tsf.sh — 在 CI 上**真的走一遍 TSF**
#
# ══ 這支腳本補的是哪一個洞 ═════════════════════════════════════════
#
# windows/verify_ime.sh 驗的是「經由具名管道打出你好」。那一條**繞過了 TSF**:
# 它證明引擎、資料、線路格式、IPC 都是好的,但完全沒有經過
#
#     登錄檔 → CoCreateInstance → 把 rime_tsf.dll 載入宿主進程
#            → ActivateEx → key event sink → edit session → 組字 → 上屏
#
# 而使用者實際回報的「裝好了、切過去、一個字都打不出來、也沒有任何 UI」,
# 撞到的正是這一段。CI 一路綠燈,因為它從來沒有走過這條路。
#
# 這支腳本用 windows/tests/tsf_host_main.cc 建出來的 rime_tsf_host.exe ——
# 一個假的文字編輯器 —— 逼系統走完上面那一整條。
#
# ══ 兩種模式 ═══════════════════════════════════════════════════════
#
#   (預設)   只要求「DLL 被載入」+「ActivateEx 被呼叫」。
#             不需要 librime、不需要詞庫、不需要服務進程 ——
#             所以它跑得起來的地方是**快速 job**,四分鐘就有答案。
#   --full    再要求「按鍵被吃掉」與「文件裡真的長出你好」。
#             需要裝好的服務與詞庫,所以只在 install 那個 job 裡跑。
#
# ══ 反向測試 ═══════════════════════════════════════════════════════
#
# 「註冊之後 ActivateEx 被呼叫」這句話要成立,得先證明**沒註冊時它不會被呼叫**。
# 否則一支「不管怎樣都印綠字」的驗證程式也會通過。
# 所以這支腳本在反註冊之後再跑一次,並要求它紅。
#
# 用法(Git Bash,需要系統管理員權限 —— runner 上本來就是):
#   windows/verify_tsf.sh --bin <放二進位的目錄> [--full] [--langid 0x0404]
#
set -euo pipefail

log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[error]\033[0m %s\n' "$*" >&2; exit 1; }
ok()   { printf '  ✓ %s\n' "$*"; }

BIN=""
FULL=0
LANGID="0x0404"
# 呼叫端有沒有明確指定。沒有的話 1b 會用 enable-user 自動選的那一份 ——
# 那才是使用者機器上會發生的事。
LANGID_EXPLICIT=""
while [ $# -gt 0 ]; do
  case "$1" in
    --bin)    BIN="$2"; shift 2 ;;
    --full)   FULL=1; shift ;;
    --langid) LANGID="$2"; LANGID_EXPLICIT=1; shift 2 ;;
    *) die "未知參數: $1" ;;
  esac
done
[ -n "${BIN}" ] || die "用法: windows/verify_tsf.sh --bin <目錄> [--full]"
[ -d "${BIN}" ] || die "找不到目錄 ${BIN}"

# ⚠ 一定要轉成絕對路徑,而且要在做任何事之前。
#
# 登錄檔的 InprocServer32 存的是**絕對路徑** —— COM 之後照著它去 LoadLibrary。
# 這裡若把相對路徑餵給 register,`cygpath -w` 會給出一個相對的 Windows 路徑
# (`third_party\build\...`),於是註冊「成功」,而任何宿主都載不到那個 DLL。
# 實測就是這樣:CI run #58 的 register 回報成功、三個語言也都列舉得到,
# 只有 check 的「InprocServer32 指到的不是這一份」那一條抓住它。
# 少了 check 那條斷言,這支腳本會在一個根本載不到 DLL 的狀態下繼續跑,
# 然後以「ActivateEx 沒有被呼叫」失敗 —— 指向一個完全錯的地方。
BIN="$(cd "${BIN}" && pwd)"

command -v cygpath >/dev/null 2>&1 || die "必須在 Git Bash / MSYS2 下執行"
w() { cygpath -w "$1"; }

TOOL="${BIN}/rime_ime_setup.exe"
HOST="${BIN}/rime_tsf_host.exe"
DLL="${BIN}/rime_tsf.dll"
for f in "${TOOL}" "${HOST}" "${DLL}"; do
  [ -f "${f}" ] || die "找不到 ${f}"
done

WORK="${BIN}/tsf-verify"
mkdir -p "${WORK}"
fail=0
note_fail() { printf '\033[1;31m  !! %s\033[0m\n' "$*" >&2; fail=1; }

# ⚠ 一定要有 trap。這支腳本會把**這台機器**的登錄檔改掉(全機註冊)。
#   中途死掉而沒有反註冊的話,下一個 job / 下一輪跑在同一台快取機器上時,
#   verify_installer.sh 的第一條斷言(「安裝之前 check 必須紅」)會失敗,
#   而那個失敗看起來與這裡完全無關。
cleanup() {
  "${TOOL}" disable-user > "${WORK}/cleanup-user.log" 2>&1 || true
  "${TOOL}" unregister > "${WORK}/cleanup.log" 2>&1 || true
}
trap cleanup EXIT

# ══════════════════════════════════════════════════════════════════
#  0. 反向測試:**還沒註冊**時,宿主必須看不到我們
# ══════════════════════════════════════════════════════════════════
#
# 這一步是免費的,而且它擋掉一整類問題:如果 rime_tsf_host 不管怎樣都說
# 「載入了、ActivateEx 被呼叫了」,那後面那一步的綠燈一點都不算數。
log "0. 反向測試:註冊之前,rime_tsf_host 必須紅"
"${TOOL}" unregister > "${WORK}/pre-unreg.log" 2>&1 || true
set +e
"${HOST}" --langid "${LANGID}" --require-activate \
          --trace "$(w "${WORK}/trace-before.log")" --wait-ms 1500 \
          > "${WORK}/host-before.log" 2>&1
rc_before=$?
set -e
if [ "${rc_before}" -eq 0 ]; then
  cat "${WORK}/host-before.log"
  die "什麼都還沒註冊,rime_tsf_host 竟然以 0 結束 —— 它沒有在驗任何東西"
fi
if [ "${rc_before}" -eq 3 ]; then
  cat "${WORK}/host-before.log"
  die "這台機器上建不出 ITfThreadMgr —— TSF 在這個工作階段裡不可用。
  這支腳本在這種環境下什麼都驗不到。**不要**把這一步改成略過:
  略過等於把一個從來沒被驗過的東西寫成綠燈。要改的話請改成
  「在 workflow 裡明確地不跑這個 job,並在 README 寫清楚為什麼」。"
fi
ok "註冊之前 rime_tsf_host 以 ${rc_before} 結束(它看得出差別)"

# ══════════════════════════════════════════════════════════════════
#  1. 註冊
# ══════════════════════════════════════════════════════════════════
log "1. 全機註冊 + 使用者啟用"
"${TOOL}" register --dll "$(w "${DLL}")" > "${WORK}/register.log" 2>&1 \
  || { cat "${WORK}/register.log"; die "register 失敗"; }
tr -d '\r' < "${WORK}/register.log" | sed 's/^/    /'
"${TOOL}" enable-user > "${WORK}/enable.log" 2>&1 \
  || { cat "${WORK}/enable.log"; die "enable-user 失敗"; }
tr -d '\r' < "${WORK}/enable.log" | sed 's/^/    /'

# ⚠ 剛註冊完的當下 CTF 還看不到新的設定檔。
#
# 實測(見 windows/README.md):register 回傳成功之後 0.12 秒時
# EnumLanguageProfiles 看不到我們,22 秒後同一支程式跑同一段就全部看得到。
# 登錄檔是同步的,CTF 的可見性不是。
#
# 這裡不睡一個猜的秒數 —— 睡太短會間歇性紅(最難查的一種),
# 睡太長是每一輪 CI 都白等。改成輪詢到真的看得見為止。
log "2. 等 CTF 看得到我們(最多 60 秒)"
seen=0
for i in $(seq 1 60); do
  if "${TOOL}" check > "${WORK}/check.log" 2>&1; then
    seen=1
    ok "第 ${i} 秒:TSF 的列舉 API 看得到我們了"
    break
  fi
  sleep 1
done
if [ "${seen}" -ne 1 ]; then
  cat "${WORK}/check.log"
  die "60 秒之後 CTF 仍然看不到這個輸入法 —— 後面每一步都沒有意義"
fi

# ── 替**目前使用者**啟用那一份語言設定檔 ──────────────────────────
#
# ⚠ 這一步是 2026-08-09 加的,而它補的是一個**一直被隱式滿足**的前提。
#
#   在那之前,RegisterProfile 的 bEnabledByDefault 是 TRUE ——
#   那是「全機、對所有使用者預設啟用」。所以這支腳本只要 register 完就能
#   activate,從來不必問「這個使用者啟用了嗎」。
#
#   而 TRUE 正是使用者回報的「清單上三格」的來源之一,這一輪改成了 FALSE
#   (見 tsf/registration.cc)。改完之後這支腳本**當場紅了**,而且紅得
#   很有價值:ActivateProfile 仍然回 S_OK、ActivateEx 仍然被呼叫、
#   語言列按鈕仍然加得上,只有**按鍵一顆都沒有到達 OnTestKeyDown**,
#   而 GetActiveProfile 回報的是另一個 langid。
#   也就是說「沒有替使用者啟用」的症狀是**輸入法看起來活著但打不出字**。
#
#   這件事現在明著寫進流程:真實的順序是 register → enable-user → 使用,
#   安裝程式做的就是這三步。腳本照著做,才是在驗使用者會走的那條路。
#
# ⚠ 用 --lang 指定,不用自動判斷:runner 上一個中文語言都沒有,自動判斷
#   會落到退路(0x0804),而這支腳本待會要 activate 的是 ${LANGID}。
#   兩者不一致的話,失敗的原因會指向完全錯的地方。
log "1b. 替目前使用者啟用一份(真實順序是 register → enable-user → 使用)"
if [ -n "${LANGID_EXPLICIT}" ]; then
  "${TOOL}" enable-user --lang "${LANGID}" > "${WORK}/enable-user.log" 2>&1 || true
else
  "${TOOL}" enable-user > "${WORK}/enable-user.log" 2>&1 || true
fi
cat "${WORK}/enable-user.log"

# ⚠ **待會要 activate 的,必須是實際被啟用的那一份。**
#
#   runner 上使用者的語言清單裡一個中文都沒有,而已安裝的輸入語言有
#   0x0804(zh-CN)沒有 0x0404(zh-TW)。硬要 activate 0x0404 的話,
#   TSF 會去 activate 它找得到的那一個(實測回報 0x0804),
#   於是宿主與 DLL 對「現在是哪一份」的認知不一致,而症狀是
#   **按鍵一顆都沒有到達 OnTestKeyDown** —— 一個指向完全錯誤方向的紅燈。
#
#   這件事本身就是這一輪在修的問題的另一面:**註冊一個使用者沒有的語言,
#   他就找不到我們**。腳本硬寫一個 langid,等於在測一個不會發生的情境。
"${TOOL}" user-profiles > "${WORK}/user-profiles.log" 2>&1 || true
CHOSEN="$(tr -d '\r' < "${WORK}/user-profiles.log" \
          | sed -n 's/^ENABLED=\(0x[0-9A-Fa-f]*\)=.*/\1/p' | head -1)"
n_on="$(tr -d '\r' < "${WORK}/user-profiles.log" | sed -n 's/^ENABLED_COUNT=//p' | head -1)"
if [ "${n_on}" = "1" ] && [ -n "${CHOSEN}" ]; then
  ok "已替目前使用者啟用 ${CHOSEN},而且**正好一份**(清單上只會有一格)"
  LANGID="${CHOSEN}"
else
  cat "${WORK}/user-profiles.log"
  note_fail "enable-user 之後啟用了 ${n_on:-?} 份(預期 1)。
     下面每一格都會以「打不出字」的形式紅,而真正的原因在這裡。"
fi
echo "  後面的宿主會 activate ${LANGID}"

# ══════════════════════════════════════════════════════════════════
#  3. 真的走一遍
# ══════════════════════════════════════════════════════════════════
log "3. 建 ITfThreadMgr、給文件焦點、啟用我們的設定檔、送按鍵"
# ⚠ --press-preserved-key 與 --check-preserved-key 是**兩件事**。
#   前者問「TSF 收下註冊了沒」,後者**真的把那顆鍵按下去**並要求
#   OnPreservedKey 留下記錄。實測過:把 tsf/text_service.cc 的 OnPreservedKey
#   在比對完 GUID 之後直接 return S_OK(註冊留著、hotkey_policy 留著、
#   服務端攔截也留著),CI 三個 job 全綠 —— 中英切換整條死掉而沒有人知道。
#   這個 job 沒有服務(logic 那一組不產 rime_service.exe),所以這裡只問
#   「有沒有走到我們身上」;按下去之後文件會怎樣在 verify_installer.sh §6f。
#
# ⚠ --press-shift 是**量測**,不是斷言。使用者實機回報「我用 shift 切換
#   中英文,但是他不變」,而要不要做那顆鍵取決於一個至今沒有人量過的
#   事實:TSF 到底會不會把純修飾鍵交給 key event sink?
#
#   這棵樹裡有兩份互相矛盾的答案,兩份都沒有實測在背書:
#     tsf/text_service.cc 的 OnTestKeyUp:「TSF 本來就不會…」
#     common/key_eat_policy.cc:「不太會…但交過來時」
#   擋著那條路的不是技術,是這一句沒有人量過的話。
#
#   它不會讓這個 job 紅 —— 它只把 SHIFT_TRACE_LINES 印出來,由那個數字
#   決定下一輪走哪一條(見下面的判讀)。
HOST_ARGS=(--langid "${LANGID}" --require-activate
           --check-preserved-key --press-preserved-key --press-shift
           --trace "$(w "${WORK}/trace-after.log")" --wait-ms 4000)
if [ "${FULL}" -eq 1 ]; then
  # nihao 之後按 1 選第一個候選。與 verify_ime.sh / verify_console.sh
  # 同一個案例 —— 三條路徑打同一個字,對得起來才有意義。
  HOST_ARGS+=(--keys nihao1 --expect 你好 --require-eaten)
else
  # 沒有服務與詞庫的環境。仍然送按鍵,因為**送了才驗得到 keysym 那一段**
  # (使用者撞到的正是它);只是不要求被吃掉。
  HOST_ARGS+=(--keys ni)
fi
set +e
"${HOST}" "${HOST_ARGS[@]}" > "${WORK}/host-after.log" 2>&1
rc_after=$?
set -e
tr -d '\r' < "${WORK}/host-after.log"

if [ "${rc_after}" -ne 0 ]; then
  note_fail "rime_tsf_host 以 ${rc_after} 結束 —— 見上面的分層結論"
fi

# 逐條斷言,不只看結束碼。結束碼只說「有東西不對」,
# 而我們要的是「哪一格不對」——那才是下一步該往哪查的答案。
after="$(tr -d '\r' < "${WORK}/host-after.log")"

# ── Shift 的量測結果:印出來,不判合格 ──────────────────────────
#
# ⚠ 這一段刻意**不會讓 job 紅**。它問的是「事實是什麼」,不是
#   「合不合格」,而兩種結果都有對應的下一步:
#
#     > 0  key event sink 收得到純修飾鍵 → P1-乙 可以走 ShiftTapDetector
#          那條純函式的路(四支 sink 各餵一次,*eaten 一律 FALSE,
#          偵測到輕點就送既有的正規形式 Ctrl+空白鍵)。
#     = 0  收不到 → 走降級版(PreserveKey{VK_SHIFT, TF_MOD_ON_KEYUP}),
#          或**乾脆不做 Shift**,靠設定裡那一句文案把 Ctrl + 空白鍵
#          教出來。那是誠實的退路,不是失敗。
#
# ⚠ 而且它證不到「真實宿主的訊息迴圈會不會把 VK_SHIFT 送進 TSF」——
#   這支 harness 走的是 ITfKeystrokeMgr::KeyDown,那是**宿主呼叫的入口**。
#   後者只有人在記事本 / Chrome / Word 上試得出來,已列進 #48。
shift_lines="$(printf '%s\n' "${after}" | sed -n 's/^ *SHIFT_TRACE_LINES=//p' | head -1)"
if [ -n "${shift_lines}" ]; then
  echo
  echo "── Shift 量測(不判合格)────────────────────────────────"
  printf '%s\n' "${after}" | grep -E '^ *SHIFT_' || true
  if [ "${shift_lines}" -gt 0 ] 2>/dev/null; then
    echo "  → key event sink **收得到**純修飾鍵。text_service.cc 的"
    echo "    OnTestKeyUp 那句註解(「TSF 本來就不會…」)與這個結果矛盾,"
    echo "    要照這裡量到的改寫。P1-乙 可以走 ShiftTapDetector 那條路。"
  else
    echo "  → key event sink **收不到**純修飾鍵。P1-乙 走降級版,"
    echo "    或乾脆不做 Shift、靠文案把 Ctrl + 空白鍵教出來。"
  fi
  echo "────────────────────────────────────────────────────────"
  echo
else
  echo "  (沒有 SHIFT_TRACE_LINES —— rime_tsf_host 可能還不認得 --press-shift)"
fi

# ── Ctrl+空白鍵那顆保留鍵 ────────────────────────────────────────
#
# 使用者回報「ctrl+ 空格沒辦法切中英文」。這裡問的是**註冊**:
# PreserveKey 是在 ActivateEx 裡呼叫的,所以只有在真的被系統啟用過的
# 那一份文字服務上問得到。按下去會怎樣是另一半,在 verify_ime.sh。
#
# ⚠ 兩個方向都要:Ctrl+空白鍵**必須**在,Ctrl+C **必須不在** ——
#   註冊得太寬的話,使用者每一個程式裡的複製都會消失,而他不會知道
#   是輸入法幹的。兩條斷言都由 rime_tsf_host 自己印,這裡對它的字。
case "${after}" in
  *"IsPreservedKey(Ctrl+Space) hr=0x00000000 registered=1"*)
    ok "Ctrl+空白鍵真的被註冊成保留鍵了(在真的 ActivateEx 之後問到的)" ;;
  *) note_fail "Ctrl+空白鍵**沒有**註冊成保留鍵 —— 使用者按下去不會有任何事" ;;
esac
case "${after}" in
  *"IsPreservedKey(Ctrl+C)     hr=0x00000000 registered=1"*)
    note_fail "Ctrl+C 也被註冊成保留鍵了 —— 使用者的複製會消失" ;;
  *) ok "Ctrl+C 沒有被搶走" ;;
esac

# ── ⚠ 註冊之外的另一半:那顆鍵**按下去**有沒有走到我們身上 ──────────
#
# 上面兩條問的都是「TSF 收下註冊了沒」,它們**不呼叫產品一行程式碼**。
# 實測過:把 tsf/text_service.cc 的 OnPreservedKey 在比對完 GUID 之後直接
# `return S_OK`(註冊留著、hotkey_policy 留著、服務端攔截也留著)——
# CI 三個 job 全綠。中英切換可以整條死掉而沒有人知道,因為中間那一段
#     OnPreservedKey → IPC → 服務 → 切模式 → 快照 → edit session → 文件
# 沒有任何自動化走過。
#
# 這裡補的是它的第一格:那顆鍵按下去,OnPreservedKey **有沒有被呼叫**。
# 判準是瘦 DLL 自己在那個函式裡寫下的記錄行(宿主數它多了幾筆),
# 不是回傳值 —— TSF 對「沒人處理」與「處理了但什麼都沒做」回的東西一樣。
#
# ⚠ 這個 job 沒有服務(logic 那一組刻意不產 rime_service.exe),所以這裡
#   問不到「模式有沒有真的變」;那一半在 verify_installer.sh 的 §6f,
#   它用裝好的服務斷言**文件內容**。兩半都要。
case "${after}" in
  *"PRESERVEDKEY_DELIVERED=1"*)
    ok "Ctrl+空白鍵按下去真的走到 OnPreservedKey 了(不只是註冊在那裡)" ;;
  *) note_fail "Ctrl+空白鍵按下去**沒有到達 OnPreservedKey**。
     註冊是好的(上面那一條過了),壞的是後面那一段 —— 使用者按下去
     不會有任何事發生,而 IsPreservedKey 仍然回答『註冊了』。" ;;
esac
case "${after}" in
  *"PRESERVEDKEY_ROUTE=real"*)
    ok "  走的是**真的按鍵**那條路(TSF 自己把它認成保留鍵交回來)" ;;
  *"PRESERVEDKEY_ROUTE=simulated"*)
    echo "  · 真的按鍵沒有被 TSF 轉成保留鍵,改用 SimulatePreservedKey 走到"
    echo "    (runner 是非互動的工作階段,按鍵路由與桌面上不保證一樣)——"
    echo "    產品那一段仍然被完整走過,只是入口不同。" ;;
esac

case "${after}" in
  *"系統把 rime_tsf.dll 載入了這個進程"*)
    ok "DLL 被系統載入了(登錄檔 → COM → LoadLibrary 這一段是好的)" ;;
  *) note_fail "DLL **沒有**被載入 —— 註冊那一段有問題" ;;
esac
case "${after}" in
  *"ActivateEx 被呼叫了"*)
    ok "ActivateEx 被呼叫了(這一格從本輪之前一直是紙上的)" ;;
  *) note_fail "ActivateEx **沒有**被呼叫" ;;
esac

# ── keysym 那一段 ────────────────────────────────────────────────
#
# ⚠ 這一條是這一輪最重要的新斷言。
#
# 我們的文字服務被啟用時,GetKeyboardLayout(0) 拿到的不保證是一份真的
# 鍵盤佈局。問不出字的話 MapKey 回 keysym 0,於是每一顆按鍵都被原樣放行:
# 引擎收不到、連線不建立、**服務不會被啟動** —— 使用者同時失去輸入與全部 UI。
# 記錄檔裡的 keysym 欄位就是那件事的直接證據。
trace="$(tr -d '\r' < "${WORK}/trace-after.log" 2>/dev/null || true)"
if [ -z "${trace}" ]; then
  note_fail "除錯記錄是空的 —— 瘦 DLL 的落地記錄機制本身壞了"
else
  case "${trace}" in
    *"keysym=0x0 "*)
      note_fail "有按鍵映出 keysym=0 —— 目前的鍵盤佈局問不出字。
     這正是「一個字都打不出來,而且沒有任何 UI」的根因形狀。
     記錄裡的那幾行:
$(printf '%s\n' "${trace}" | grep -a '按鍵' | sed 's/^/       /')" ;;
    *"按鍵 vk="*)
      ok "按鍵映得出非零的 keysym(佈局問得出字)" ;;
    *) note_fail "記錄裡沒有任何按鍵行 —— OnTestKeyDown **一次都沒有被呼叫**。
     這與「按鍵映不出 keysym」是兩件完全不同的事:按鍵根本沒有被交給我們。
     要查的是 ActivateEx 裡的 AdviseKeyEventSink(記錄裡有 key sink 那一行),
     不是佈局、也不是連線。" ;;
  esac
  case "${trace}" in
    *"完全問不出字"*)
      note_fail "記錄說這個工作階段的鍵盤佈局完全問不出字" ;;
  esac

  # ── 語言列上的那顆按鈕 ──────────────────────────────────────────
  #
  # ⚠ 這一條是這一輪加的。它補的是一個一直被當成「驗不了」的東西:
  #   「使用者的語言列上到底看不看得到它」。
  #
  #   長什麼樣仍然驗不到(那要有人看)。但**它有沒有被建出來、有沒有被
  #   語言列收下**是 ActivateEx 裡的一個明確結果,而瘦 DLL 自己會把答案
  #   寫進落地記錄(text_service.cc 的「語言列按鈕:…」那一行)。
  #   在這之前那一行只有出事之後才有人去看 —— 沒有人主動問過它。
  #
  #   加不上去**不算失敗**:某些宿主根本沒有語言列項目管理員,那時
  #   系統匣那條路才是入口。但「記錄裡連提都沒提」是失敗 ——
  #   那代表 ActivateEx 沒走到那一段,而那是另一回事。
  case "${trace}" in
    *"語言列按鈕:已加入"*)
      ok "語言列按鈕被建出來而且被宿主收下了(設定的入口之一)" ;;
    *"語言列按鈕:加不上"*)
      echo "  · 語言列按鈕加不上(這個假宿主沒有語言列項目管理員)——"
      echo "    不算失敗,但代表這一輪**沒有**驗到那個入口在真宿主上會出現。"
      echo "    系統匣與「開始」功能表那兩條路是獨立的,見 verify_installer.sh §12。" ;;
    *)
      note_fail "記錄裡完全沒有「語言列按鈕」那一行 —— ActivateEx 沒有走到那一段。
     那不是「這個宿主沒有語言列」,是我們根本沒有嘗試加。
     症狀會是語言列上永遠沒有設定入口,而使用者只剩系統匣與開始功能表。" ;;
  esac
fi

if [ "${FULL}" -eq 1 ]; then
  case "${after}" in
    *"文件裡真的是「你好」"*)
      ok "**經由真的 TSF**把「你好」寫進了文件" ;;
    *) note_fail "沒有經由 TSF 打出「你好」" ;;
  esac
fi

# ══════════════════════════════════════════════════════════════════
#  4. 反向測試:反註冊之後必須再度紅
# ══════════════════════════════════════════════════════════════════
log "4. 反向測試:反註冊之後 rime_tsf_host 必須再度紅"
"${TOOL}" unregister > "${WORK}/unreg.log" 2>&1 || true
set +e
"${HOST}" --langid "${LANGID}" --require-activate \
          --trace "$(w "${WORK}/trace-final.log")" --wait-ms 1500 \
          > "${WORK}/host-final.log" 2>&1
rc_final=$?
set -e
if [ "${rc_final}" -eq 0 ]; then
  cat "${WORK}/host-final.log"
  note_fail "反註冊之後 rime_tsf_host 竟然還通過 —— 上面的綠燈都不算數"
else
  ok "反註冊之後以 ${rc_final} 結束"
fi

echo
[ "${fail}" -eq 0 ] || die "TSF 驗證失敗,見上。"
log "TSF 驗證通過 ✓(這一輪起,「ActivateEx 有沒有被呼叫」不再是紙上的)"
