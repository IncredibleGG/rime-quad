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
while [ $# -gt 0 ]; do
  case "$1" in
    --bin)    BIN="$2"; shift 2 ;;
    --full)   FULL=1; shift ;;
    --langid) LANGID="$2"; shift 2 ;;
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

# ══════════════════════════════════════════════════════════════════
#  3. 真的走一遍
# ══════════════════════════════════════════════════════════════════
log "3. 建 ITfThreadMgr、給文件焦點、啟用我們的設定檔、送按鍵"
HOST_ARGS=(--langid "${LANGID}" --require-activate
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
