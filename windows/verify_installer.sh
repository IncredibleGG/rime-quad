#!/usr/bin/env bash
#
# windows/verify_installer.sh — 在真的 Windows 上裝一次、驗一次、解除安裝一次
#
# ══ 這支腳本把「驗不了」變成「驗得了」════════════════════════════
#
# 上一輪的 windows/README.md 把這些列在「只有人做得到」那一欄:
#
#     regsvr32 是否真的註冊成功、輸入法是否出現在系統的清單上
#
# 那個判斷有一半是錯的。GitHub 的 windows-latest runner 上我們是系統管理員,
# 所以下面這一整條是跑得動的:
#
#   靜默安裝 → 斷言登錄檔真的長出東西 → 用 TSF 的 API 列舉出自己
#   → 用**安裝好的**服務與**安裝好的**資料經由真的具名管道打出「你好」
#   → 靜默解除安裝 → 斷言登錄檔清乾淨了、而使用者詞典還在
#
# 「輸入法有沒有被系統接受」這件事的驗證價值,比安裝程式本身還高 ——
# 這個專案最貴的失敗一直都是「編得出來、測試全綠、使用者一裝就不能用」。
#
# ── 仍然驗不到的(這一節請不要縮水)────────────────────────────
#
#   · 在記事本 / 瀏覽器 / Office / 市集 App 裡真的打不打得出字
#     (§6c 與 §6d 走的是我們自己寫的假宿主 rime_tsf_host.exe)
#   · 候選窗長什麼樣、位置對不對、高 DPI 與多螢幕
#   · 使用者的語言列上看不看得到它(還取決於使用者的語言清單,
#     而 runner 上沒有辦法製造那個情境)
#   · 語言列按鈕上那個「未啟動」的狀態**長什麼樣** ——
#     它的判斷邏輯有單元測試,但沒有人看過它畫出來
#   · 修飾鍵組合(Ctrl+C / Alt+F4)真的沒有被吃掉 ——
#     假宿主的送鍵路徑不帶修飾鍵狀態,所以那一格只有真值表驗得到
#
# ── 這一輪從上面那一欄**搬下來**的(現在驗得到了)────────────────
#
#   · 切過去之後 ActivateEx 有沒有被呼叫(§6c)
#   · **服務由瘦 DLL 自己啟動**(§5c)—— 以前每一次都是這支腳本
#     自己先把服務跑起來,所以那條路從來沒有被驗過一次
#   · **冷啟動**:空的使用者資料目錄 + 沒有人先部署 → 第一次打字(§5c)
#   · 「每一顆鍵是不是都真的做了它宣稱的事」(§6d 的按鍵矩陣)
#   · **組字進行中按 Ctrl+空白鍵**(§6f)—— 註冊之外的那一半。
#     以前只問「TSF 收下註冊了沒」,而那不呼叫產品一行程式碼:
#     把 OnPreservedKey 改成比對完 GUID 就 return S_OK,三個 job 全綠。
#
# 用法(Git Bash,需要系統管理員權限):
#   windows/verify_installer.sh --setup <安裝程式.exe> \
#                               --probe <rime_probe.exe> \
#                               --tool  <rime_ime_setup.exe>
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[error]\033[0m %s\n' "$*" >&2; exit 1; }

# ── 開機佇列的純文字判準(可以在 Linux 上自我驗證)────────────────
#
# ⚠ §13 那一節的結論寫的是「升級沒有把**任何東西**排進開機佇列」,
#   而它實際上只問了**一個檔名**:rime_tsf.dll。安裝目錄底下還有
#   rime_service.exe、rime_ime_setup.exe、data\shared 的四十份 yaml、
#   opencc 的 .ocd2 —— 任何一個被排進佇列,使用者都會在安裝程式最後一頁
#   被問要不要重新啟動,而磁碟上那一份仍然是舊的。也就是說,那一節宣稱
#   的事情比它驗的大很多,而中間的差距全部是綠的。
#
#   所以現在問的是**整個安裝目錄**。唯一的例外是我們自己排的
#   rime_tsf.dll.old-<時間戳> 清理:那是刻意排的,刪掉一個沒有人會載入的
#   舊檔不會讓安裝程式要求重開機。
#
#   這一支刻意做成「讀 stdin、吐 stdout」的純文字函式 —— 它才驗得到。
#   `--self-check-pending` 在任何一台機器上都跑得動(見底下)。
# ⚠ **不要用 `awk -v dir="$1"`。** -v 的值會被 awk 再做一次跳脫處理,而安裝
#   目錄裡全是反斜線:gawk(Git Bash / windows-latest 上的 awk)把 `\N`
#   當成 plain `N`,於是 `C:\Program Files\X` 變成 `C:Program FilesX`,
#   比對**永遠不命中** —— 也就是這一整條判準在真的 runner 上會安靜地
#   回「零個違規」。本機的 mawk 不做那個處理,所以本機是全綠的。
#   (實際發生過:CI run 31332458753,而且是被下面那個自檢抓到的。)
#   環境變數不經過跳脫處理,所以走 ENVIRON。
pending_filter() {  # $1 = 安裝目錄(Windows 路徑);讀 stdin
  # ⚠ RIME_AWK 刻意**不預設**在這裡:下面的自檢靠「它是不是空的」
  #   分辨自己是外圈(要對每一種 awk 各跑一次)還是內圈。
  RIME_PENDING_DIR="$1" "${RIME_AWK:-awk}" '
    BEGIN { d = tolower(ENVIRON["RIME_PENDING_DIR"]) }
    {
      line = $0
      sub(/\r$/, "", line)
      if (line == "") next
      l = tolower(line)
      # 我們自己排的舊 DLL 清理不算 —— 它不會讓安裝程式要求重開機。
      if (l ~ /rime_tsf\.dll\.old-/) next
      if (index(l, d) > 0) print line
    }
  '
}

# ── 反向測試:證明這條判準真的抓得到「不是 rime_tsf.dll」的那些 ──
if [ "${1:-}" = "--self-check-pending" ]; then
  # ⚠ 對**這台機器上找得到的每一種 awk** 都跑一次。
  #   上面那個 -v 的坑就是「本機 mawk 綠、runner gawk 全 0」——
  #   只跑一種 awk 的自檢看不到它。busybox awk 與 gawk 在這一點上同族,
  #   所以在開發機上它就是 runner 的替身。
  if [ -z "${RIME_AWK:-}" ]; then
    _awk_dir="$(mktemp -d)"
    _awks=""
    for _c in awk mawk nawk gawk original-awk; do
      command -v "${_c}" >/dev/null 2>&1 || continue
      printf '#!/bin/sh\nexec %s "$@"\n' "${_c}" > "${_awk_dir}/${_c}"
      chmod +x "${_awk_dir}/${_c}"
      _awks="${_awks} ${_awk_dir}/${_c}"
    done
    if command -v busybox >/dev/null 2>&1; then
      printf '#!/bin/sh\nexec busybox awk "$@"\n' > "${_awk_dir}/busybox-awk"
      chmod +x "${_awk_dir}/busybox-awk"
      _awks="${_awks} ${_awk_dir}/busybox-awk"
    fi
    _rc=0
    for _a in ${_awks}; do
      printf '\033[1;34m==>\033[0m awk = %s\n' "$(basename "${_a}")"
      RIME_AWK="${_a}" "$0" --self-check-pending || _rc=1
    done
    rm -rf "${_awk_dir}"
    exit "${_rc}"
  fi
  # ⚠ 刻意用一個**不存在的假目錄**,不是真的安裝目錄:
  #   (a) 這一支驗的是判準本身,目錄只是它的參數;
  #   (b) 產品識別碼在腳本裡寫死是被 scripts/verify_product_ids.sh 擋的
  #       (唯一來源是 scripts/lib/product.env)—— 這個自檢跑在
  #       product_win.sh 被 source 之前,所以它不該去碰真的名字。
  DIR='Z:\NotAProduct\FakeInstallDir'
  sc_fail=0
  sc() {  # 名稱 期望命中數 佇列內容
    local name="$1" want="$2" body="$3" got
    got="$(printf '%s\n' "${body}" | pending_filter "${DIR}" | grep -c . || true)"
    if [ "${got}" -eq "${want}" ]; then
      printf '  \033[1;32mok\033[0m   %s(命中 %s)\n' "${name}" "${got}"
    else
      printf '\033[1;31m  !! %s:預期命中 %s,實際 %s\033[0m\n' "${name}" "${want}" "${got}" >&2
      sc_fail=1
    fi
  }
  # 舊版的判準,拿來對照 —— 用它跑同一批,證明差距真的存在。
  old_rule() { grep -ciE 'rime_tsf\.dll$' || true; }

  sc "空佇列" 0 ""
  sc "只有我們自己排的舊 DLL 清理(不算)" 0 \
     'Z:\NotAProduct\FakeInstallDir\rime_tsf.dll.old-20260810120000'
  sc "rime_tsf.dll 被排進去" 1 \
     'Z:\NotAProduct\FakeInstallDir\rime_tsf.dll'
  sc "rime_service.exe 被排進去" 1 \
     'Z:\NotAProduct\FakeInstallDir\rime_service.exe'
  sc "詞典被排進去" 1 \
     'Z:\NotAProduct\FakeInstallDir\data\shared\luna_pinyin.dict.yaml'
  sc "別人的檔案(不在安裝目錄底下)不算" 0 \
     'C:\Windows\System32\somebody_else.dll'
  sc "混在一起:兩個我們的 + 一個清理 + 一個別人的" 2 \
     'Z:\NotAProduct\FakeInstallDir\rime_service.exe
Z:\NotAProduct\FakeInstallDir\rime_tsf.dll.old-20260810120000
C:\Windows\System32\somebody_else.dll
Z:\NotAProduct\FakeInstallDir\data\shared\essay.txt'

  # 舊判準漏掉的那幾種,逐一指名 —— 這就是這次修的東西。
  for entry in \
      'Z:\NotAProduct\FakeInstallDir\rime_service.exe' \
      'Z:\NotAProduct\FakeInstallDir\data\shared\luna_pinyin.dict.yaml' \
      'Z:\NotAProduct\FakeInstallDir\rime_ime_setup.exe'
  do
    n_old="$(printf '%s\n' "${entry}" | old_rule)"
    n_new="$(printf '%s\n' "${entry}" | pending_filter "${DIR}" | grep -c . || true)"
    if [ "${n_old}" -eq 0 ] && [ "${n_new}" -eq 1 ]; then
      printf '  \033[1;32mok\033[0m   舊判準漏掉、新判準抓到:%s\n' "${entry##*\\}"
    else
      printf '\033[1;31m  !! %s:舊=%s 新=%s(預期 舊=0 新=1)\033[0m\n' \
             "${entry}" "${n_old}" "${n_new}" >&2
      sc_fail=1
    fi
  done
  [ "${sc_fail}" -eq 0 ] || { printf '\033[1;31m開機佇列判準的反向測試沒過\033[0m\n' >&2; exit 1; }
  printf '\033[1;32m開機佇列判準的反向測試全部通過\033[0m\n'
  exit 0
fi

# ── 更新之後把服務叫回來:那一段的規則(純文字,Linux 上就驗得到)──
#
# ⚠ 這一條守的**不是**「檔案裡有沒有某個字」,是三件會把行為弄壞的事:
#
#   1. **不可以用 Exec 啟動服務。** 這個安裝程式是提權的;Exec 生出來的
#      服務會繼承提權的權杖,接著用系統管理員的身分去讀寫使用者的檔案,
#      擁有者從此換成不對的人,一般權限的那一支服務再也寫不進去。
#      症狀是「更新過一次之後,輸入法就再也記不住東西」——
#      而且要好幾天後才發現。理由完整寫在 common/elevation_policy.h。
#   2. **必須被 /RESTARTIME 守著。** 沒有守衛的話,使用者自己雙擊安裝程式
#      也會被塞一個服務進程,而那不是他要的。
#   3. **那一段不可以消失。** 少了它,應用內更新按下去之後設定視窗會關掉、
#      然後什麼都不再發生 —— 看起來就是「更新把輸入法弄壞了」。
#
#   另外驗第四件:.iss 有沒有真的安裝 version.txt。少了它,裝出來的那一套
#   永遠查不到更新,而輸入法本身完全正常 —— 沒有人會回報這種事。
#
# 做成「讀 stdin、印違規」的純函式,所以 --self-check-restart 在任何一台
# 機器上都跑得動,而且可以真的植入違規要求它變紅。
restart_rule_violations() {  # 讀 stdin,印出違規(一行一條);沒有就什麼都不印
  # ⚠ 指令碼要先落到檔案再跑。把 heredoc 直接餵給 python3 - 的話，它會把**指令碼自己**當成 stdin，
  #   於是待驗的內容永遠是空的 —— 而空的內容當然「沒有違規」，
  #   五條反向測試會全部回同一個答案。實測踩過。
  local _py; _py="$(mktemp)"
  cat > "${_py}" <<'PYRESTART'
import re, sys

src = sys.stdin.read()

# Pascal Script 的一句話以 ; 結尾。把換行折起來再切,呼叫寫成兩行也看得到。
flat = re.sub(r"\s+", " ", src)
stmts = flat.split(";")

start_calls = [t for t in stmts if "rime_service.exe" in t and "Exec" in t]

if not start_calls:
    print("NOSTART=更新之後沒有任何一句會把 rime_service.exe 叫回來")

for t in start_calls:
    if re.search(r"(?<!AsOriginalUser)\bExec\s*\(", t):
        print("ELEVATED=用 Exec 啟動服務(會繼承提權權杖):" + t.strip()[:120])

# 守衛:啟動那一句要在 /RESTARTIME 的判斷之後、而且在同一個程序裡
# (以下一個行首的 end; 當程序結束)。
g = src.find("CmdLineParamExists('/RESTARTIME')")
if g < 0:
    print("NOGUARD=沒有 /RESTARTIME 這個守衛 —— 使用者自己雙擊也會被啟動服務")
else:
    tail = src[g:]
    stop = tail.find("\nend;")
    scope = tail[:stop if stop >= 0 else len(tail)]
    if "rime_service.exe" not in scope:
        print("OUTSIDE=啟動服務那一句不在 /RESTARTIME 的守衛裡")

# version.txt 要真的被裝進安裝目錄。
if not re.search(r'Source:\s*"[^"]*version\.txt";\s*DestDir:\s*"\{app\}"', src):
    print("NOVERSIONTXT=.iss 沒有把 version.txt 裝進安裝目錄 —— 裝出來的那一套永遠查不到更新")
PYRESTART
  "${RIME_PY:-python3}" "${_py}"
  rm -f "${_py}"
}

if [ "${1:-}" = "--self-check-restart" ]; then
  # ⚠ 這一段與上面那個開機佇列的自檢不同:它**必須讀真的 .iss**,
  #   所以要先把產品識別載進來。檔名寫死的話,改名那一天這支腳本會
  #   安靜地找不到檔案 —— 而「找不到就 die」看起來會像環境問題。
  . "${SCRIPT_DIR}/product_win.sh"
  ISS="${SCRIPT_DIR}/${RS_WIN_ISS_REL}"
  [ -f "${ISS}" ] || die "找不到 ${ISS}"
  rc_fail=0
  rc_case() {  # 名稱 期望的違規標籤(空 = 不該有違規) 檔案內容來源命令
    local name="$1" want="$2"; shift 2
    local got
    got="$("$@" | restart_rule_violations | cut -d= -f1 | sort -u | tr '\n' ',' )"
    got="${got%,}"
    if [ "${got}" = "${want}" ]; then
      printf '  \033[1;32mok\033[0m   %s(違規=%s)\n' "${name}" "${got:-無}"
    else
      printf '\033[1;31m  !! %s:預期違規「%s」,實際「%s」\033[0m\n' \
             "${name}" "${want:-無}" "${got:-無}" >&2
      rc_fail=1
    fi
  }
  # ⚠ 第一條是**恆假防護**:現況必須是綠的。少了它,底下三條全部會因為
  #   「反正一直都有違規」而永遠通過。
  rc_case "現況(不該有任何違規)" "" cat "${ISS}"
  rc_case "把 ExecAsOriginalUser 換成 Exec" "ELEVATED" \
          sed 's/ExecAsOriginalUser(ExpandConstant(.{app}\\rime_service.exe.)/Exec(ExpandConstant('"'"'{app}\\rime_service.exe'"'"')/' "${ISS}"
  rc_case "拿掉 /RESTARTIME 守衛" "NOGUARD" \
          sed "s/CmdLineParamExists('\/RESTARTIME')/True/" "${ISS}"
  rc_case "整段啟動服務拿掉" "NOSTART,OUTSIDE" \
          sed '/rime_service.exe.,$/d;/rime_service.exe/{/Source:/!d}' "${ISS}"
  rc_case "不安裝 version.txt" "NOVERSIONTXT" \
          sed '/Source: "{#PayloadDir}\\version.txt"/d' "${ISS}"
  [ "${rc_fail}" -eq 0 ] || {
    printf '\033[1;31m重啟服務那一段的反向測試沒過\033[0m\n' >&2; exit 1; }
  printf '\033[1;32m重啟服務那一段的反向測試全部通過\033[0m\n'
  exit 0
fi

SETUP=""; PROBE=""; TOOL=""; HOST=""
while [ $# -gt 0 ]; do
  case "$1" in
    --setup) SETUP="$2"; shift 2 ;;
    --probe) PROBE="$2"; shift 2 ;;
    --tool)  TOOL="$2";  shift 2 ;;
    # rime_tsf_host.exe。給了就多做一件事:**經由真的 TSF** 打一次字
    # (見底下 §6c)。刻意做成可選的 —— 這支腳本在只有安裝程式的環境下
    # 仍然要跑得動。
    --host)  HOST="$2";  shift 2 ;;
    # 已經在上面處理掉了;留在這裡只是為了讓 --help 之類的讀者看得到。
    --self-check-pending) shift ;;
    --self-check-restart) shift ;;
    *) die "未知參數: $1" ;;
  esac
done
[ -f "${SETUP}" ] || die "找不到安裝程式: ${SETUP}"
[ -f "${PROBE}" ] || die "找不到 rime_probe.exe: ${PROBE}"
[ -f "${TOOL}" ]  || die "找不到 rime_ime_setup.exe: ${TOOL}"
[ -z "${HOST}" ] || [ -f "${HOST}" ] || die "找不到 rime_tsf_host.exe: ${HOST}"

command -v cygpath >/dev/null 2>&1 || die "必須在 Git Bash / MSYS2 下執行"
w() { cygpath -w "$1"; }

# 產品識別:值的唯一來源是 scripts/lib/product.env。
#
# ⚠ 這一段的每一個名字都與**使用者磁碟上真的存在的東西**綁著:安裝目錄、
#   %APPDATA% 底下的資料夾、設定檔名、安裝記錄裡的字首。腳本自己抄一份的話,
#   改名之後這支腳本會去檢查一個不存在的資料夾 —— 而它的每一項檢查都是
#   「這個東西應該在」或「這個東西應該不在」,後者在資料夾根本不存在時
#   **全部會通過**。也就是說:漏改的症狀是一片綠燈,而不是紅字。
# shellcheck disable=SC1091
. "${SCRIPT_DIR}/product_win.sh"

# 安裝目錄不寫死 C:\。Inno 的 {autopf} 在 64 位元安裝模式下展開成
# %ProgramW6432%(而不是 (x86) 那一份),所以就照那個算 ——
# runner 的系統碟哪天不是 C: 的話,寫死的版本會以「檔案不存在」的形式失敗,
# 而那看起來像安裝程式壞了。
PF_W="${ProgramW6432:-${PROGRAMFILES:-C:\\Program Files}}"
INSTALL_DIR_W="${PF_W}\\${RS_WIN_INSTALL_FOLDER}"
INSTALL_DIR="$(cygpath -u "${PF_W}")/${RS_WIN_INSTALL_FOLDER}"
USER_DIR="${APPDATA:-}"
[ -n "${USER_DIR}" ] || die "APPDATA 是空的"
USER_DIR="$(cygpath -u "${USER_DIR}")/${RS_WIN_DATA_FOLDER}"
WORK="$(dirname "${SETUP}")/verify"
mkdir -p "${WORK}"

fail=0
note_fail() { printf '\033[1;31m  !! %s\033[0m\n' "$*" >&2; fail=1; }
ok()        { printf '  ✓ %s\n' "$*"; }

# ── 登錄檔查詢的小工具 ────────────────────────────────────────────
#
# 用 reg.exe 而不是 PowerShell 的 Get-ItemProperty:reg.exe 的結束碼是
# 「鍵在不在」的直接答案,不必解析輸出,也不會因為 PowerShell 的錯誤動作
# 設定不同而在不同 runner 上表現不一樣。
# /reg:64 明著指定 64 位元檢視 —— 少了它,在 32 位元的宿主底下查會落進
# WOW6432Node,而「查不到」與「沒註冊」長得一模一樣。
reg_key_exists() { reg query "$1" //reg:64 >/dev/null 2>&1; }

# ── 「開機時刪除」的佇列 ──────────────────────────────────────────
#
# 檔案在解除安裝當下被別的程式握著時,Inno 刪不掉它,只好呼叫
# MoveFileEx(..., MOVEFILE_DELAY_UNTIL_REBOOT) —— 那會在這個登錄檔值裡
# 留下一筆。**這就是使用者看到那個「必須重新啟動」對話框的原因**,
# 也是「為什麼登出不夠」的答案:這份佇列只有 Session Manager 在
# **開機**時處理,登出登入不會碰它。
#
# 這兩個函式讓那件事變成可以斷言的東西,而不是我們寫在文案裡的推測。
PFRO='HKLM\SYSTEM\CurrentControlSet\Control\Session Manager'
pending_renames() {
  reg query "${PFRO}" //v PendingFileRenameOperations //reg:64 2>/dev/null \
    | tr -d '\r' || true
}
pending_has_our_dll() {
  pending_renames | grep -qi 'rime_tsf\.dll'
}

# PendingFileRenameOperations 是 REG_MULTI_SZ,reg.exe 把分隔印成字面的 \0。
# 拆開之後每一行是一筆路徑;它們**成對**出現(來源、目的),
# 目的為空 = 開機時刪除。
pending_entries() {
  pending_renames | sed 's/\\0/\n/g' | sed 's/^\\??\\//'
}

# ⚠ 這一個是升級那一節(§13)真正的紅線:佇列裡有一筆的路徑**正好是**
#   那個固定的 rime_tsf.dll。那代表 Inno 沒能換掉檔案,只好排隊等開機 ——
#   也就是說磁碟上那一顆還是舊的,**新開的進程也會拿到舊的**,
#   一直到使用者重新開機為止,而且安裝程式會問他要不要現在重啟。
#   結尾是 .old-<時間戳> 的那些是我們自己排的清理,無害,不算數。
pending_targets_live_dll() {
  pending_entries | grep -qiE 'rime_tsf\.dll$'
}

# ⚠ §13 的紅線用的是**這一支**,不是上面那一支。理由見檔案上方
#   pending_filter 的說明:那一節宣稱「沒有把任何東西排進開機佇列」,
#   而只問一個檔名的話,rime_service.exe / 四十份 yaml / .ocd2 全部
#   落在宣稱與驗證之間的空隙裡。
pending_in_install_dir() {
  pending_entries | pending_filter "${INSTALL_DIR_W}"
}

# ── 升級那幾節共用的小工具 ────────────────────────────────────────
DLL_U="${INSTALL_DIR}/rime_tsf.dll"
DLL_W="${INSTALL_DIR_W}\\rime_tsf.dll"

# NTFS 的檔案 id。用它回答一個沒有別的辦法回答的問題:
# 「現在躺在 rime_tsf.dll 那個路徑上的,還是不是舊 host 手上那一顆檔案?」
# 路徑一樣、內容也可能一樣(同一次建置),**檔案 id 不一樣就是換過了**。
#
# ⚠ `|| true` 不是裝飾。fsutil 在檔案不存在(或這台機器上問不到)時以非零
#   結束,而這個函式的結果是拿去做變數指派的 —— 配上 set -e,
#   整支腳本會在那裡當場死掉,而錯誤訊息完全不會提到 fsutil。
#   問不到就回空字串,呼叫端自己處理(它會退回別的證據)。
file_id() {
  (fsutil file queryfileid "$1" 2>/dev/null || true) \
    | tr -d '\r' | awk '{print $NF}'
}
stale_list() {
  (ls -1 "${INSTALL_DIR}"/rime_tsf.dll.old-* 2>/dev/null || true)
}
stale_count() { stale_list | wc -l | tr -d ' '; }

# ── 建立 session 有沒有超過用戶端的預算 ───────────────────────────
#
# 這一格把「有時候不能打中文」變成一個**數字**。
#
#   建立 session 的往返預算是 300 毫秒(ipc_client.cc 的 kConnectTimeoutMs),
#   而那一趟跑在**宿主的 UI 執行緒**上 —— 調大等於讓使用者按下第一顆鍵時
#   整個程式卡住那麼久,所以它不能調大。超過預算的下場是 fail-open:
#   那個宿主整個工作階段都打不出中文,而使用者只看到英文、沒有錯誤訊息。
#
# ⚠ 這一條刻意**不是**「平均值」或「95 百分位」。只要有**一次**超過,
#   就有一個使用者的某一個程式打不出中文,而他不會知道為什麼。
#
# ⚠ 掃**所有**服務記錄:
#     ${WORK}/*.log        本腳本啟動的那幾支(§6、以及 §13 的 p13-svc-*)
#     diagnostics/service.log  **瘦 DLL 啟動的**那一支(§5c/§5d 冷啟動走的,
#                          也是使用者機器上真正跑的那一支;它是
#                          DETACHED_PROCESS,以前印的每個字都掉進黑洞)
#   只掃一部分的下場是實際發生過的:2026-08-09 那一輪最慢的三次全在
#   §13 的服務裡(250/297/**328**),而 §6e 只讀 §6 那一份,
#   於是報告「最久 109 ms、0 次超過」,隔壁的 §13 正因為那個 328 紅著。
DIAG_SVC_LOG="$(cygpath -u "${LOCALAPPDATA}")/${RS_WIN_DATA_FOLDER}/diagnostics/service.log"
assert_session_new_budget() {   # $1 = 這一次掃描的標籤
  local lines max n over
  lines="$( (grep -ahao 'SESSION_NEW_MS=[0-9]*' \
               "${WORK}"/*.log "${DIAG_SVC_LOG}" 2>/dev/null || true) \
            | sed 's/.*=//')"
  if [ -z "${lines}" ]; then
    note_fail "$1:服務記錄裡一行 SESSION_NEW_MS= 都沒有 —— 這一格沒有在量任何東西。
     (是 pipe_server.cc 那一行不見了,還是記錄檔不在預期的位置?)"
    return
  fi
  max="$(printf '%s\n' "${lines}" | sort -n | tail -1)"
  n="$(printf '%s\n' "${lines}" | wc -l | tr -d ' ')"
  over="$(printf '%s\n' "${lines}" | awk '$1 >= 300' | wc -l | tr -d ' ')"
  log "  $1:SESSION_NEW ${n} 次,最久 ${max} ms,超過 300ms 的有 ${over} 次"
  if [ "${over}" -eq 0 ]; then
    ok "$1:**沒有任何一次建立 session 超過用戶端 300ms 的預算**(最久 ${max} ms)
     —— 也就是沒有宿主會因此 fail-open 成「打不出中文」。"
  else
    # ⚠ 把「慢工作」那幾行一起印出來 —— 引擎只有一條執行緒,所以
    #   「我為什麼慢」的答案幾乎一定是「**別人**擋在前面」,而那個別人
    #   叫什麼名字只有這幾行說得出來(service/engine.cc 的 ReportSlowJob)。
    #   少了它們,報告只會說「有一次 1328 ms」,而那 1.2 秒引擎在做什麼
    #   仍然是個謎 —— 2026-08-09 那一輪就是這樣卡住的。
    echo "      --- 建 session 的耗時 ---"
    (grep -ah 'SESSION_NEW_MS=\|SESSION_NEW 失敗' "${WORK}"/*.log \
       "${DIAG_SVC_LOG}" 2>/dev/null || true) | tail -20 | sed 's/^/      /'
    echo "      --- 同一段時間裡引擎在忙什麼(慢工作)---"
    (grep -ah '慢工作' "${WORK}"/*.log "${DIAG_SVC_LOG}" 2>/dev/null || true) \
      | tail -25 | sed 's/^/      /'
    note_fail "$1:有 ${over} 次建立 session 超過 300ms(最久 ${max} ms)。
     每一次都代表一個宿主進程 fail-open —— 使用者在那個程式裡打不出中文,
     而且沒有任何錯誤訊息。這正是「選了輸入法之後有時候不能打中文」。
     ⚠ 修的方向**不是**把預算調大、也不是加重試(那只會讓它更難查)。
     ⚠ 也**不是**把貴的工作丟到佇列裡非同步做 —— 那個已經試過而且量到
       更糟:成本不會消失,只會搬進按鍵那條預算(現在是 150 毫秒,
       windows/common/key_deadline.h 的 kKeyTimeoutMs;服務端那一側是
       kKeyDeadlineMs = 100)。用完的下場**不是**「這顆鍵慢了」——
       是 ipc_client.cc 的 Fail() → Close() 把整條連線丟掉,而那個宿主
       接下來要重連、重建 session、重套方案,期間每一顆鍵都是英文。
     ⚠ 也**不是**在測試裡多等一會 —— 那只是讓關卡變綠。
     量到的成因(2026-08-09):引擎只有一條執行緒,而前一個宿主離開時的
     rs_session_destroy(寫回使用者詞典)一旦**開始**就停不下來,
     下一個宿主的 SESSION_NEW 只能等。見 service/engine.cc 的
     kLowPriorityIdleMs。"
  fi
}

reg_value() {
  # $1 = 鍵, $2 = 值名(空字串代表預設值)
  local out
  if [ -z "${2:-}" ]; then
    out="$(reg query "$1" //ve //reg:64 2>/dev/null || true)"
  else
    out="$(reg query "$1" //v "$2" //reg:64 2>/dev/null || true)"
  fi
  # reg.exe 的資料列:「    (Default)    REG_SZ    C:\path\x.dll」。
  # 值本身可能含空白,所以取第三欄之後的全部,而不是第三欄。
  printf '%s' "${out}" | tr -d '\r' \
    | awk '/REG_SZ|REG_EXPAND_SZ|REG_DWORD/ { $1=""; $2=""; sub(/^[ \t]+/, ""); print; exit }'
}

# ── 使用者的語言清單 ──────────────────────────────────────────────
#
# HKCU\Control Panel\International\User Profile 的 REG_MULTI_SZ `Languages`。
# reg.exe 把 MULTI_SZ 的分隔印成字面的 \0,所以換成空白就是一份清單。
#
# ⚠ 這個位置**沒有**官方文件(微軟文件裡的對應物是 PowerShell 的
#   Get-WinUserLanguageList)。這裡只拿它當**觀測用**,不拿它當判斷依據 ——
#   判斷走的是產品自己的 `rime_ime_setup.exe user-profiles`。
#   讀不到就印「(讀不到)」,不讓這一節因此變紅:它是量測,不是斷言。
user_language_list() {
  reg query 'HKCU\Control Panel\International\User Profile' //v Languages //reg:64 2>/dev/null \
    | tr -d '\r' \
    | awk '/REG_MULTI_SZ/ { $1=""; $2=""; sub(/^[ \t]+/, ""); print; exit }' \
    | sed 's/\\0/ /g'
}

# ── 設定視窗現在開著嗎 ────────────────────────────────────────────
#
# 問視窗類別名,不是找進程、也不是找標題:
#   · 找進程只證明「服務在跑」,那與「視窗開出來了」是兩件事 ——
#     而使用者按下去看到的是後者。
#   · 標題會隨語言與版本變,類別名不會。
#
# ⚠ 走產品自己的 `rime_ime_setup.exe find-window`,**不走 PowerShell**。
#   第一版是 PowerShell 的 FindWindowW,而類別名含中文:
#   Git Bash → powershell.exe 的命令列會經過一次代碼頁轉換,中文被換掉之後
#   它找的是一個不存在的類別,於是**永遠回報找不到** ——
#   而那看起來與「設定視窗真的沒開出來」一模一樣(CI run #86 就是這樣)。
#
# 類別名的唯一來源仍然是 product.env(見 verify_product_names.sh 的
# 「設定窗類別名」那一列),由這裡推導後傳進去。
SETTINGS_CLASS="${RS_PRODUCT_NAME}SettingsWindow"
# ⚠ **一定要 --visible。** 設定視窗在服務一啟動時就被建好但不顯示
#   (service/settings_window.cc:按下去要立刻看到窗,不能等它建)。
#   所以「視窗存在」對任何一支跑著的服務都恆真 —— CI run #87 就是這樣:
#   12b 兩秒就「通過」了,而 12c 立刻發現「第一支不帶 --settings 的服務
#   也把視窗開出來了」。使用者看得到的是**顯示出來**,不是存在。
settings_window_present() {
  "${INSTALL_DIR}/rime_ime_setup.exe" find-window \
    --class "${SETTINGS_CLASS}" --visible >/dev/null 2>&1
}

# ⚠ 「顯示中」與「在最前面」是兩件不同的事,而使用者只在乎第二件。
#
#   把視窗叫出來的是**已經在跑的那一支服務**,而它不符合
#   SetForegroundWindow 的任何一條放行條件(不是前景進程、不是被前景進程
#   啟動的、沒收到最後一個輸入事件)。少了 service/main.cc 與
#   tsf/text_service.cc 那兩處 AllowSetForegroundWindow(精確 pid),
#   系統只會讓工作列按鈕閃一下 —— 視窗**顯示出來了,卻停在別的視窗後面**。
#   使用者的體感與「按了設定沒反應」一模一樣。
#
#   而 settings_window_present() 只問 IsWindowVisible,對那個狀態回真:
#   壞掉與修好在報表上會是同一格綠。這一個就是補上那一格。
settings_window_foreground() {
  "${INSTALL_DIR}/rime_ime_setup.exe" find-window \
    --class "${SETTINGS_CLASS}" --visible --foreground >/dev/null 2>&1
}

# ══ 桌面狀態:誰佔著前景 ════════════════════════════════════════════
#
# ⚠ 這三個函式是 run 31896143629 的 §13 逼出來的,理由值得寫滿。
#
#   那一次 §13 的兩支 TSF 宿主都搶不到前景,前景一直是同一個 handle
#   0x10202;沒有執行緒焦點,TSF 就不把按鍵交給文字服務,於是
#   「舊 DLL 連不回新服務(試了 599 次五分鐘)」這句紅字被印了出來 ——
#   而服務端同一段時間的記錄是「連線 #1 存活=300829ms 握手=1 按鍵=0」,
#   管道全程是好的。那句紅字指的三個方向(管道名 / 版本協商 / ABI)
#   全部是好的東西。
#
#   查下去發現兩件事:
#     1. **整條工具鏈問不出「現在的前景視窗是誰」。** find-window 只問得到
#        我們自己指定的類別。所以只能推論。→ 補 foreground-window 動詞。
#     2. **§12 收尾是在設定視窗正是前景視窗的狀態下 taskkill //F 掉擁有者。**
#        那一批第一次讓設定視窗真的當上前景(service/main.cc 的
#        AllowSetForegroundWindow + rime_service.exe 改成 GUI 子系統),
#        §12 四支服務的狀態列都記下了「前景 = 設定視窗的類別」那一行
#        (見 artifact 的 settings-*.log:15 的 [bar] 那一行)。
#        硬殺之後前景沒有交還給任何人 —— 這台 runner 沒有 Explorer 可以
#        接手、也沒有任何輸入事件,於是 SetForegroundWindow 唯一還成立的
#        那條放行條件(「目前沒有前景視窗」)從此消失,後面誰都搶不到。
#        → §12 每一節收尾改成**先請服務自己下台**,再記一行桌面狀態。
#
#   一個驗證段落把桌面狀態弄髒了留給下一段,而下一段因此紅 ——
#   這本身就是缺陷,與產品好不好無關。

# 記一行「現在前景是誰」。**只記錄,不判斷** —— 判斷是下面兩個的事。
foreground_note() {   # $1 = 這一刻的標籤
  log "  [桌面] ${1}:"
  "${INSTALL_DIR}/rime_ime_setup.exe" foreground-window 2>&1 \
    | sed 's/^/    /' || true
}

# 前景**不是**我們的設定視窗 → 0。是我們的 → 1。
foreground_not_ours() {
  "${INSTALL_DIR}/rime_ime_setup.exe" foreground-window \
    --class "${SETTINGS_CLASS}" >/dev/null 2>&1
}

# ── §12 每一節的收尾:把桌面還原,而不是只有 taskkill //F ──────────
#
# 順序不能反過來,理由與 setup 的 stop-service 同源:
#   1. **先請服務自己下台**(具名結束事件 → 它的訊息迴圈自己收掉,
#      設定視窗走正規的 DestroyWindow,前景才會被交還)。
#      stop-service 內部已經是「先好好請、5 秒沒反應才 TerminateProcess」。
#   2. 再收掉這一節自己 fork 出來的 shell 工作(傳進來的 pid)。
#   3. 最後才是 taskkill //F 兜底 —— 它是**保險**,不是主要手段。
#   4. 收完記一行桌面狀態,並斷言前景不再是我們的類別。
#
# ⚠ 第 4 步是斷言,不是記錄。收完之後前景還是我們的設定視窗,代表
#   下一節一定拿不到焦點 —— 那時要紅在這裡,不要讓下一節去背這個鍋
#   然後講一個關於版本協商的故事。
settings_section_teardown() {   # $@ = 這一節 fork 出來的 pid(可以是空的)
  local p
  # 追加而不是覆寫:這個函式一輪會被叫四五次,而「哪一次沒收乾淨」
  # 正是要看的東西 —— 覆寫的話只留得下最後一次。
  printf -- '--- settings_section_teardown (pids: %s) ---\n' "$*" \
    >> "${WORK}/settings-teardown.log"
  "${INSTALL_DIR}/rime_ime_setup.exe" stop-service --dir "${INSTALL_DIR_W}" \
    >> "${WORK}/settings-teardown.log" 2>&1 || true
  for p in "$@"; do
    [ -n "${p}" ] && kill "${p}" 2>/dev/null || true
  done
  taskkill //IM rime_service.exe //F >/dev/null 2>&1 || true
  sleep 2
  if foreground_not_ours; then
    ok "收尾之後前景已經不是設定視窗了(桌面狀態還乾淨,下一節量得到東西)"
  else
    foreground_note "收尾之後"
    note_fail "收尾之後**前景仍然是我們的設定視窗**。
     這代表視窗沒有正規下台,前景這個欄位卡在我們手上(或卡在我們留下
     的殘留控制代碼上)。後面每一節的 TSF 宿主都會拿不到執行緒焦點,
     而 TSF 不給焦點就不交按鍵 —— 那些段落會紅在「打不出字」上,
     講的卻是別的故事(見 §13c)。⚠ **要修的是這裡的收尾,不是那些段落。**"
  fi
}

# ── 捷徑(.lnk)指到哪裡、帶什麼參數 ──────────────────────────────
#
# ⚠ 這是這一輪非加不可的一項,而它**第一次跑就抓到一個一直存在的缺陷**:
#   捷徑名字裡的冒號是半形的,而 NTFS 把「名字:something」解讀成交替資料流,
#   於是磁碟上長出來的是一個名字被截斷的**空檔案** ——
#   「開始」功能表裡那一項什麼都不會做。那正是我們叫使用者去點的診斷入口。
#
# ⚠ **不要在命令列上傳中文給 PowerShell。** Git Bash → powershell.exe 會經過
#   一次代碼頁轉換,而「開始」功能表的資料夾名是產品的中文名 ——
#   轉壞之後 CreateShortcut 拿到一個不存在的路徑,回傳空字串,
#   而那看起來與「捷徑指到錯的地方」一模一樣(CI run #86 就是這樣)。
#   所以改成:把腳本寫成檔案(UTF-8 with BOM,PowerShell 讀得對),
#   讓它自己去列舉整個「開始」功能表,結果寫成 UTF-8 的 TSV,bash 再讀。
#   命令列上一個非 ASCII 字元都沒有。
dump_start_menu_shortcuts() {   # $1 = 輸出檔(TSV: 名字 \t 目標 \t 參數)
  local ps1="${WORK}/dump-lnk.ps1"
  printf '\xEF\xBB\xBF' > "${ps1}"
  cat >> "${ps1}" <<'PS1EOF'
$ErrorActionPreference = 'Stop'
$roots = @(
  (Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs'),
  (Join-Path $env:AppData     'Microsoft\Windows\Start Menu\Programs')
)
$sh = New-Object -ComObject WScript.Shell
$out = New-Object System.Collections.ArrayList
foreach ($r in $roots) {
  if (-not (Test-Path $r)) { continue }
  Get-ChildItem -Path $r -Recurse -Filter *.lnk -ErrorAction SilentlyContinue | ForEach-Object {
    try {
      $l = $sh.CreateShortcut($_.FullName)
      [void]$out.Add(($_.BaseName + "`t" + $l.TargetPath + "`t" + $l.Arguments))
    } catch { }
  }
}
[System.IO.File]::WriteAllLines($env:RIMEWIN_LNK_OUT, $out, (New-Object System.Text.UTF8Encoding($false)))
PS1EOF
  RIMEWIN_LNK_OUT="$(cygpath -w "$1")" \
    powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass \
               -File "$(cygpath -w "${ps1}")" >/dev/null 2>&1 || true
}

# ══════════════════════════════════════════════════════════════════
#  0. 產品自己宣稱的登錄檔路徑與 GUID
# ══════════════════════════════════════════════════════════════════
#
# 路徑向產品本身要(rime_ime_setup.exe paths),不在腳本裡另抄一份 ——
# 抄一份就會漂移,而漂移的症狀是「斷言查的是別的鍵,所以永遠通過」。
# 但 GUID 在下面**另外寫死一份做交叉比對**:GUID 改了就是換了一個輸入法,
# 使用者原本選好的那一項會從清單上消失,那時 CI 就該紅。
log "0. 取得產品宣稱的登錄檔路徑"
"${TOOL}" paths > "${WORK}/paths.txt" 2>&1 || die "rime_ime_setup.exe paths 失敗"
tr -d '\r' < "${WORK}/paths.txt" | tee "${WORK}/paths.clean"
get_path() { awk -F= -v k="$1" '$1==k { sub("^" k "=", ""); print; exit }' "${WORK}/paths.clean"; }

CLSID="$(get_path CLSID)"
HKLM_CLSID="HKLM\\$(get_path HKLM_CLSID)"
HKLM_INPROC="HKLM\\$(get_path HKLM_INPROC)"
HKLM_CTF="HKLM\\$(get_path HKLM_CTF)"
HKLM_CTF_CAT="HKLM\\$(get_path HKLM_CTF_CATEGORY)"
CATEGORY_COUNT="$(get_path CATEGORY_COUNT)"
CATEGORY_ITEMS="$(get_path CATEGORY_ITEMS)"
PROFILE_COUNT="$(get_path PROFILE_COUNT)"
# 一行一個:PROFILE=0x0404={4F78BA11-…}
PROFILES="$(grep '^PROFILE=' "${WORK}/paths.clean" | sed 's/^PROFILE=//')"

# 交叉比對:這幾個值一旦發布出去就不能改。
#
# ⚠ 這一份是**刻意寫死的第二意見**,不從 tsf/guids.cc 讀進來 ——
#   讀進來就變成拿同一個來源跟自己比,而那種比對永遠會過。
#   產品那一側的值由 `rime_ime_setup.exe paths` 報出來(上面的 $CLSID)。
#
# ⚠ 2026-08-09:產品定名時這五個 GUID(CLSID、三份語言設定檔、AppId)
#   **全部換過一次**,理由與後果見 tsf/guids.h 檔頭與 windows/README.md
#   的升級章節。換完之後「不能改」重新生效。
EXPECT_CLSID='{7D02992E-B213-4E06-B62E-CCC6338DA98A}'
# 語言 → profile GUID。**這張表就是「輸入法出現在哪些語言底下」。**
# 使用者回報過:只註冊 0x0404 的話,系統語言是簡體中文的人在自己的語言
# 底下找不到這個輸入法(它掛在「繁体中文(中国台湾)」那一欄)。
EXPECT_PROFILES="$(printf '%s\n' \
  '0x0404={4F78BA11-E997-4BD7-8B97-F4553ABC0B18}' \
  '0x0804={84420A61-0A08-4A68-9D60-292EFD31C7BC}' \
  '0x0C04={C6B736EB-38E3-4041-B59B-ECF91AD8E28A}')"

[ "${CLSID}" = "${EXPECT_CLSID}" ] \
  || die "CLSID 變了:${CLSID} != ${EXPECT_CLSID}
  改 CLSID 等於換成另一個輸入法 —— 使用者原本選好的那一個會從清單上消失,
  而且舊的那筆註冊沒有東西去清掉它。若是刻意的,請同時更新這支腳本。"
if [ "$(printf '%s\n' "${PROFILES}" | sort)" != "$(printf '%s\n' "${EXPECT_PROFILES}" | sort)" ]; then
  echo "產品宣稱:"; printf '%s\n' "${PROFILES}" | sed 's/^/    /'
  echo "腳本預期:"; printf '%s\n' "${EXPECT_PROFILES}" | sed 's/^/    /'
  die "註冊的語言清單變了。若是刻意的,請同時更新這支腳本 —— 這張表決定
  「輸入法出現在哪些語言底下」,少一個就是那個語言的使用者找不到它。"
fi
ok "CLSID 與 ${PROFILE_COUNT} 份語言設定檔的 GUID 都與預期一致"

# ⚠ 能力類別的數字也要一份寫死的第二意見 —— 上面那一則的理由一字不改地
#   適用,而這兩個值原本沒有跟上。
#
#   實測(2026-08-12,run 31526574022 / install-x64):把
#   tsf/registration.cc 的 kTipCapCategories 拿掉
#   &GUID_TFCAT_TIPCAP_SYSTRAYSUPPORT 一行,下面那兩條斷言印的是
#
#       ✓ 能力類別 5 類(GUID_TFCAT_TIP_KEYBOARD + 5 個能力類別)
#       ✓ 能力類別 5 類 / 13 筆齊全
#
#   **兩條都綠。** 原因是預期值 CATEGORY_COUNT / CATEGORY_ITEMS 由被測的
#   那支 rime_ime_setup.exe 自己報出來(setup_main.cc → registration.cc 的
#   RegisteredCategoryCount(),它就是 sizeof(kTipCapCategories)),
#   於是登錄檔少一筆、預期也跟著少一筆,兩邊永遠相等。
#
#   而「少一個」正是那兩條斷言宣稱在擋的事:少了 IMMERSIVESUPPORT,
#   市集 App 與 Edge 裡完全用不了;少了 SECUREMODE,提權視窗上打不了字。
#   症狀是「在某些程式裡沒反應」,而且與程式碼無關 —— 只是沒註冊。
#   換句話說:這兩條斷言原本擋不住它們唯一存在的理由。
#
# ⚠ 改這兩個數字 = 改變輸入法在哪些宿主裡活得下去。要改請連同
#   tsf/registration.cc 的 kTipCapCategories 一起,並在這裡寫下為什麼。
EXPECT_CATEGORY_COUNT=6      # GUID_TFCAT_TIP_KEYBOARD + 5 個能力類別
EXPECT_CATEGORY_ITEMS=16
if [ "${CATEGORY_COUNT}" != "${EXPECT_CATEGORY_COUNT}" ] || \
   [ "${CATEGORY_ITEMS}" != "${EXPECT_CATEGORY_ITEMS}" ]; then
  # ⚠ 這裡是 note_fail 而不是 die,與上面 CLSID / PROFILES 那兩條不同 ——
  #   分界線是「後面那 180 幾條還算不算數」。CLSID 換了的話,底下每一條
  #   登錄檔斷言查的都是別人的鍵,留著只會產生假訊號,所以那兩條 die。
  #   能力類別少一類不會讓其餘的斷言失去意義:輸入法照樣裝得起來、
  #   照樣打得出「你好」,只是在市集 App 裡活不了。為了這一個數字把
  #   整份報告砍掉,下一輪要修的人就得再等一次 CI 才看得到第二個問題。
  note_fail "產品報出來的能力類別是 ${CATEGORY_COUNT} 類 / ${CATEGORY_ITEMS} 筆,
     這支腳本寫死的第二意見是 ${EXPECT_CATEGORY_COUNT} 類 / ${EXPECT_CATEGORY_ITEMS} 筆。
     少一類的症狀是「在市集 App / Edge / 提權視窗裡沒有這個輸入法」,
     而且與程式碼無關 —— 只是沒註冊。若是刻意的,請同時更新這支腳本。"
else
  ok "能力類別的預期值與寫死的第二意見一致(${CATEGORY_COUNT} 類 / ${CATEGORY_ITEMS} 筆)"
fi

# ── 安裝之前的語言清單(§4b 要拿它比對)────────────────────────────
#
# 這一份是本輪唯一能回答「**啟用一份語言設定檔會不會替使用者新增一個語言**」
# 的東西。那個問題微軟沒有文件(EnableLanguageProfile 那一頁只有簽章與
# 兩個回傳碼,沒有 Remarks),而使用者截圖上那兩欄只掛著本輸入法的
# 繁體中文,強烈指向「是我們加上去的」。與其猜,不如量。
LANGS_BEFORE="$(user_language_list)"
log "安裝前的使用者語言清單:${LANGS_BEFORE:-(讀不到)}"

# ══════════════════════════════════════════════════════════════════
#  1. 反向測試:**安裝之前**檢查必須是紅的
# ══════════════════════════════════════════════════════════════════
#
# 這一步是免費的,而且它擋掉一整類問題:若 check 恆為真(例如判斷寫反、
# 或它其實什麼都沒查),那麼後面「安裝之後 check 通過」就一點都不算數。
# 這個專案有過「測試是綠的,因為它沒在測」。
log "1. 反向測試:安裝之前,check 必須紅"
if reg_key_exists "${HKLM_CLSID}"; then
  die "這台機器上已經註冊過了(${HKLM_CLSID})—— 環境不乾淨,後面的斷言不算數"
fi
if "${TOOL}" check > "${WORK}/check-before.log" 2>&1; then
  cat "${WORK}/check-before.log"
  die "什麼都還沒裝,check 竟然通過 —— 這道檢查沒有在檢查"
fi
ok "未安裝時 check 以非零結束"

# ── doctor 的反向測試 ─────────────────────────────────────────────
#
# `rime_ime_setup.exe doctor` 是這一輪要交給**使用者**的東西:他只要跑一次、
# 把輸出貼過來,我們就知道是九種「不能用」裡的哪一種。
#
# ⚠ 一支只會印綠字的診斷工具比沒有更糟 —— 它讓人以為有人在看。
#   所以它在這裡有正反兩面的斷言,而反面這一條放在最前面:
#   **什麼都還沒裝的時候,它必須是紅的,而且必須指出是註冊那一格。**
#   --no-engine / --no-scan:這一步只驗「它會不會紅」,
#   跑引擎與掃進程要好幾十秒,而那兩格在裝好之後才有意義。
set +e
"${TOOL}" doctor --no-engine --no-scan > "${WORK}/doctor-before.log" 2>&1
rc_doc=$?
set -e
if [ "${rc_doc}" -eq 0 ]; then
  tr -d '\r' < "${WORK}/doctor-before.log"
  die "什麼都還沒裝,doctor 竟然以 0 結束 —— 這支診斷工具沒有在診斷"
fi
if tr -d '\r' < "${WORK}/doctor-before.log" | grep -q '^  \[FAIL\] 全機註冊不完整'; then
  ok "未安裝時 doctor 以 ${rc_doc} 結束,並指出「全機註冊不完整」"
else
  echo "--- doctor 的輸出 ---"
  tr -d '\r' < "${WORK}/doctor-before.log"
  note_fail "doctor 紅了,但沒有指出是註冊那一格 —— 它紅得沒有指向性,
     而「不知道為什麼紅」與「不知道為什麼壞」一樣沒有用。"
fi

# ══════════════════════════════════════════════════════════════════
#  2. 靜默安裝
# ══════════════════════════════════════════════════════════════════
#
# /VERYSILENT 連進度視窗都不顯示;/SUPPRESSMSGBOXES 讓任何對話框直接用
# 預設值走掉(否則 CI 會卡到逾時,而症狀看起來像「安裝程式當掉」)。
log "2. 靜默安裝"
set +e
"${SETUP}" //VERYSILENT //SUPPRESSMSGBOXES //NORESTART \
           "//LOG=$(w "${WORK}/install.log")"
rc=$?
set -e
[ "${rc}" -eq 0 ] || {
  [ -f "${WORK}/install.log" ] && tail -80 "${WORK}/install.log"
  die "安裝程式以 ${rc} 結束"
}
ok "安裝程式以 0 結束"

# 安裝程式自己記下來的那幾行(註冊、enable-user 的兩種身分各自的結果)。
# 一律印出來,不是只在失敗時 —— 「enable-user 沒跑到」與「跑了但回錯」
# 在報表上長得一模一樣,而要分辨得再等一輪 CI。
if [ -f "${WORK}/install.log" ]; then
  echo "  --- 安裝程式的記錄 ---"
  tr -d '\r' < "${WORK}/install.log" | grep -a "${RS_WIN_LOG_TAG}" | sed 's/^/    /' || true

  # ⚠⚠ 這一條是本輪最重要的斷言之一。
  #
  # /SUPPRESSMSGBOXES 之下,[Code] 裡的 RaiseException **不會**讓 Setup
  # 以非零結束:對話框被自動按掉、例外只留在安裝記錄裡,而 Setup 照樣
  # 回報成功。實測就是這樣 —— 安裝程式以 0 結束、CI 一路綠燈,
  # 而 CurStepChanged 其實在中途就炸了,後面的 enable-user 一次都沒跑到。
  #
  # 也就是說「安裝程式以 0 結束」根本不足以證明安裝做完了。
  # 這一條把那個被吞掉的例外變回一個會紅的東西。
  if tr -d '\r' < "${WORK}/install.log" | grep -aq 'raised an exception'; then
    echo "  --- 安裝記錄裡的例外 ---"
    tr -d '\r' < "${WORK}/install.log" | grep -a -A 12 'raised an exception' | sed 's/^/    /'
    note_fail "安裝程式的 [Code] 丟出了例外(但 /SUPPRESSMSGBOXES 把它吞掉,Setup 仍以 0 結束)。
     安裝其實沒有做完 —— 例外之後的步驟一個都沒跑到。"
  else
    ok "安裝程式的 [Code] 沒有丟出例外"
  fi

  # enable-user 有沒有真的被執行過。沒有這一條的話,「那一步被跳過」
  # 與「跑了但沒生效」在報表上長得一模一樣。
  if tr -d '\r' < "${WORK}/install.log" | grep -aq "${RS_WIN_LOG_TAG} enable-user"; then
    ok "安裝程式執行過 enable-user"
  else
    note_fail "安裝記錄裡沒有 enable-user —— 那一步根本沒被執行"
  fi
fi

# ── 檔案真的在嗎 ──────────────────────────────────────────────────
#
# ⚠ data\shared 是這一段的重點。少了它,前面每一步都會成功、服務起得來、
#   輸入法註冊得上,**然後一個字都打不出來,而且沒有任何錯誤訊息。**
for f in rime_tsf.dll rime_service.exe rime_ime_setup.exe \
         data/shared/default.yaml \
         data/shared/luna_pinyin_tw.schema.yaml \
         data/shared/bopomofo_tw.schema.yaml \
         data/shared/luna_pinyin.schema.yaml \
         data/shared/t9_pinyin.schema.yaml \
         data/shared/luna_pinyin.dict.yaml \
         data/shared/essay.txt \
         data/user/default.custom.yaml
do
  [ -f "${INSTALL_DIR}/${f}" ] && ok "${f}" || note_fail "缺少 ${INSTALL_DIR}/${f}"
done
n_ocd2="$( (ls "${INSTALL_DIR}"/data/shared/opencc/*.ocd2 2>/dev/null || true) | wc -l | tr -d ' ')"
[ "${n_ocd2}" -gt 0 ] && ok "opencc: ${n_ocd2} 個 .ocd2" \
                      || note_fail "沒有裝到任何 .ocd2 —— 簡繁與臺灣字形轉換會失效"

# ══════════════════════════════════════════════════════════════════
#  3. 登錄檔斷言(逐個鍵值點名)
# ══════════════════════════════════════════════════════════════════
# ── 安裝目錄的基準相片(路徑 + 大小 + 修改時間)──────────────────
#
# ⚠ 照在**這裡**,而不是 §6 之前:§5c / §5d 是冷啟動與「瘦 DLL 自己把
#   服務拉起來」,也就是使用者機器上真正跑的那條路。基準線照在它們後面的話,
#   它們寫進 Program Files 的東西會被當成本來就在,永遠比對得過。
#   (理由與實測見 §6 那一段的註解。)
snapshot() { (cd "${INSTALL_DIR}" && find . -type f -printf '%p\t%s\t%T@\n' | sort); }
snapshot > "${WORK}/before.txt"

log "3. 登錄檔"

reg_key_exists "${HKLM_CLSID}" && ok "${HKLM_CLSID}" \
  || note_fail "${HKLM_CLSID} 不存在 —— COM 類別沒註冊"

inproc="$(reg_value "${HKLM_INPROC}" "")"
if [ "${inproc}" = "${INSTALL_DIR_W}\\rime_tsf.dll" ]; then
  ok "InprocServer32 = ${inproc}"
else
  note_fail "InprocServer32 是「${inproc}」,預期「${INSTALL_DIR_W}\\rime_tsf.dll」"
fi

tm="$(reg_value "${HKLM_INPROC}" ThreadingModel)"
[ "${tm}" = "Apartment" ] && ok "ThreadingModel = Apartment" \
  || note_fail "ThreadingModel 是「${tm}」,預期 Apartment"

reg_key_exists "${HKLM_CTF}" && ok "${HKLM_CTF}" \
  || note_fail "${HKLM_CTF} 不存在 —— TSF 沒有收下這個文字服務"

# 每一個語言各查一次。**「至少有一個」不算過** —— 使用者回報的問題正好
# 就是「只有 0x0404」,而那個狀態下「至少有一個」照樣成立。
#
# ⚠ 用 for 迴圈跑而不是 `... | while read`:管道右邊是**子 shell**,
#   在裡面設的 fail=1 傳不回來 —— 斷言會照常印出紅字然後整支腳本以 0 結束。
#   那正是這個專案抓過最多次的失敗模式的一種變形。
profile_langkey() { printf '0x%08x' "$((16#${1#0x}))"; }
# 用 here-string 餵 while,不用 `for line in ${PROFILES}` 靠 IFS 斷詞:
# 後者的正確性取決於 IFS 沒被動過,而且在別的 shell 裡行為不同(zsh 預設
# 根本不斷詞)。here-string **不是**管道,所以迴圈裡設的 fail=1 傳得回來 ——
# 這一點比可讀性重要:管道右邊是子 shell,斷言會照常印紅字然後腳本以 0 結束。
while IFS= read -r line; do
  [ -z "${line}" ] && continue
  lang="${line%%=*}"          # 0x0404
  guid="${line#*=}"           # {4F78BA11-…}
  key="${HKLM_CTF}\\LanguageProfile\\$(profile_langkey "${lang}")\\${guid}"
  if reg_key_exists "${key}"; then
    ok "${key}"
  else
    note_fail "缺少 ${key}
     使用 ${lang} 這個語言的人在自己的語言底下找不到這個輸入法"
  fi
done <<< "${PROFILES}"

# ⚠ 不要用「數 ^HKEY_ 的行數再減一」。
#
# reg.exe 只有在被查的鍵**有值**時才會印出鍵自己那一行標頭;沒有值(只有子鍵)
# 時就直接列子鍵。Category\Category 正好是後者,所以「減一」會少算一個 ——
# 而症狀是「註冊完全正確,CI 卻說能力類別不齊」,看起來像產品壞了。
#
# 改成只數「以這個鍵的完整路徑加一個反斜線開頭」的行。用 awk 的 index()
# 而不是 grep 的正規式:登錄檔路徑裡全是反斜線與大括號,escape 到正規式裡
# 是另一種會安靜出錯的東西。
#
# ⚠ 前綴走**環境變數**(ENVIRON)而不是 awk -v:awk 會對 -v 的值做跳脫序列
#   處理,而這個值裡全是反斜線 —— \M \C \T \{ 這些會被當成未定義的跳脫,
#   結果比對字串悄悄變形,計數永遠是 0。環境變數的值不會被處理。
#   (rime-runs.sh 把 SHA 餵給 python 也是走環境變數,同一個理由。)
n_cat="$(reg query "${HKLM_CTF_CAT}" //reg:64 2>/dev/null | tr -d '\r' \
         | CATPREFIX="HKEY_LOCAL_MACHINE\\$(get_path HKLM_CTF_CATEGORY)\\" \
           awk 'index($0, ENVIRON["CATPREFIX"]) == 1 { n++ } END { print n + 0 }')"
[ "${n_cat}" -eq "${CATEGORY_COUNT}" ] \
  && ok "能力類別 ${n_cat} 類(GUID_TFCAT_TIP_KEYBOARD + $((n_cat - 1)) 個能力類別)" \
  || note_fail "Category\\Category 底下 ${n_cat} 個,預期 ${CATEGORY_COUNT} 個
     少一個的症狀是「在市集 App / Edge / 提權視窗裡沒有這個輸入法」"

# ── 新增或移除程式 ────────────────────────────────────────────────
#
# Inno 的 ARP 鍵名是 <AppId>_is1。AppId 改了的話舊版會永遠留在
# 「新增或移除程式」裡而且解除安裝不掉,所以這裡也寫死一份做交叉比對。
ARP='HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{4D16C4D6-444A-40A7-953D-57BF873E8689}_is1'
reg_key_exists "${ARP}" && ok "新增或移除程式:有這一筆" \
  || note_fail "新增或移除程式裡找不到 ${ARP}"
UNINST="$(reg_value "${ARP}" UninstallString | tr -d '"')"
DISPNAME="$(reg_value "${ARP}" DisplayName)"
DISPVER="$(reg_value "${ARP}" DisplayVersion)"
[ -n "${DISPNAME}" ] && ok "DisplayName = ${DISPNAME}"     || note_fail "ARP 沒有 DisplayName"
[ -n "${DISPVER}" ]  && ok "DisplayVersion = ${DISPVER}"   || note_fail "ARP 沒有 DisplayVersion"
[ -n "${UNINST}" ]   && ok "UninstallString = ${UNINST}"   || note_fail "ARP 沒有 UninstallString"

# ══════════════════════════════════════════════════════════════════
#  4. TSF 的 API 看不看得到我們
# ══════════════════════════════════════════════════════════════════
#
# 登錄檔長出東西只證明「我們寫進去了」;由 TSF 自己的列舉 API 把我們列出來,
# 才證明「**系統接受了**這個輸入法」。兩者不是同一件事。
log "4. TSF 列舉 + 完整註冊檢查"
# 分成兩次呼叫,而不是一次帶 --user。兩件事的失敗原因完全不同:
#   全機那一段紅 = 註冊本身壞了(安裝程式的問題)
#   使用者那一段紅 = enable-user 沒跑到,或提權身分跑錯了 HKCU
# 合成一次的話,報表上只看得到「check 失敗」,而要分辨得再等一輪 CI。
if "${INSTALL_DIR}/rime_ime_setup.exe" check > "${WORK}/check-after.log" 2>&1; then
  cat "${WORK}/check-after.log"
  ok "全機註冊檢查通過(登錄檔 + TSF 列舉,含全部 ${PROFILE_COUNT} 個語言)"
else
  cat "${WORK}/check-after.log"
  note_fail "安裝之後全機註冊檢查仍然失敗"
fi

# 使用者那一側(啟用了幾份)搬到 §4b —— 那裡有「正好一份」的斷言
# 與失敗時該說的話。這裡若再做一次,同一個問題會用兩種說法報兩次。

# ══════════════════════════════════════════════════════════════════
#  4b. 語言切換清單上我們佔幾格
# ══════════════════════════════════════════════════════════════════
#
# ⚠ 這一節是這一輪的主題。使用者的截圖:
#
#     简体中文(中国大陆)            <本輸入法>
#     简体中文(中国大陆)            微软拼音
#     简体中文(中国大陆)            小狼毫
#     繁体中文(中国台湾)            <本輸入法>
#     繁体中文(中国香港特别行政区)   <本輸入法>
#
# 微软拼音與小狼毫各佔一格,我們佔三格。他的話:「輸入法不應該顯示那麼多。」
#
# 契約:**註冊三份(HKLM)、啟用一份(HKCU)**。
#   · 三份註冊讓「不管他有哪一種中文,都在他自己的語言底下找得到我們」——
#     那是這一端最早的 bug(只註冊繁中,簡體使用者完全找不到),不可以退回去。
#   · 一份啟用決定清單上有幾格。
#
# 舊的斷言是「HKCU 底下至少一份」,而三份全開的狀態下它照樣成立 ——
# 也就是說**這個回歸本來就穿得過舊的關卡**。現在是「正好一份」。
log "4b. 語言切換清單上我們佔幾格(**正好一格**)"

"${INSTALL_DIR}/rime_ime_setup.exe" user-profiles > "${WORK}/profiles.log" 2>&1 || true
cat "${WORK}/profiles.log"
prof_clean="$(tr -d '\r' < "${WORK}/profiles.log")"
ENABLED_COUNT="$(printf '%s' "${prof_clean}" | sed -n 's/^ENABLED_COUNT=//p' | head -1)"
ENABLED_LANGS="$(printf '%s' "${prof_clean}" | sed -n 's/^ENABLED=\(0x[0-9A-Fa-f]*\)=.*/\1/p' | tr '\n' ' ')"
# ⚠ 後面 §6c / §6d / §11 的 TSF 宿主必須 activate **實際被啟用的那一份**。
#
#   在這一輪之前,RegisterProfile 的 bEnabledByDefault 是 TRUE(全機、
#   對所有使用者),所以那幾節寫死 0x0404 也照樣過。改成 FALSE 之後
#   「哪一份可以被 activate」完全由 enable-user 決定 —— 而 runner 上
#   一個中文語言都沒有,所以它選的是退路 0x0804,不是 0x0404。
#   繼續寫死的話那幾節會以「按鍵一顆都沒到達」失敗,而原因與它們無關。
ACTIVE_LANGID="$(printf '%s' "${ENABLED_LANGS}" | awk '{print $1}')"
[ -n "${ACTIVE_LANGID}" ] || ACTIVE_LANGID=0x0404
echo "  後面的 TSF 宿主會 activate ${ACTIVE_LANGID}"

if [ "${ENABLED_COUNT}" = "1" ]; then
  ok "這個使用者啟用了 1 份(${ENABLED_LANGS})—— 清單上只有一格"
else
  note_fail "這個使用者啟用了 ${ENABLED_COUNT:-?} 份(${ENABLED_LANGS})。
     預期正好 1 份。使用者的 Win + 空白鍵清單上會出現 ${ENABLED_COUNT:-?} 格本輸入法,
     而微软拼音、小狼毫各只佔一格 —— 這正是他回報的問題。"
fi

# 產品自己的檢查也要同意。兩邊都問是刻意的:上面讀的是 user-profiles 的
# 輸出格式,這裡走的是 check --user 的斷言路徑,而安裝程式與 doctor 用的是後者。
if "${INSTALL_DIR}/rime_ime_setup.exe" check --user --no-enum \
     > "${WORK}/check-user.log" 2>&1; then
  ok "check --user 通過(正好一份)"
else
  cat "${WORK}/check-user.log"
  note_fail "check --user 沒過 —— 啟用的份數不是 1,或一份都沒有。
     前者是「清單上好幾格」,後者是「裝完了但清單裡看不到」。"
fi

# ── 量測:啟用一份會不會替使用者新增一個語言 ──────────────────────
#
# ⚠ 這一段**只印不判**(除了最後一條)。它回答的是一個沒有文件的問題,
#   而 runner 上的情境只有一種(英文、沒有任何中文),所以它證明不了
#   一般情形。把觀測寫進日誌,是為了讓下一個人不必再猜一次。
LANGS_AFTER="$(user_language_list)"
echo "  --- 量到的事實 ---"
echo "    安裝前:${LANGS_BEFORE:-(讀不到)}"
echo "    安裝後:${LANGS_AFTER:-(讀不到)}"
if [ "${LANGS_BEFORE}" != "${LANGS_AFTER}" ]; then
  echo "    → 使用者的語言清單**被改動了**。runner 上沒有任何中文,"
  echo "      所以啟用一份中文設定檔確實會替他新增那個語言。"
  echo "      這也就解釋了截圖上那兩欄只掛著本輸入法的繁體中文是怎麼來的。"
else
  echo "    → 語言清單沒有變。"
fi
# 唯一的斷言:不管清單怎麼變,**我們只能佔一格**。
n_ours="$(printf '%s' "${ENABLED_COUNT:-0}")"
case "${n_ours}" in
  1) ok "不論語言清單如何變動,我們只啟用一份" ;;
  *) note_fail "我們啟用了 ${n_ours} 份" ;;
esac

# ══════════════════════════════════════════════════════════════════
#  4c. 升級:舊版啟用了三份,新版必須把多的收回去
# ══════════════════════════════════════════════════════════════════
#
# ⚠ 這一節是**這一輪的必要條件**,不是加分項。
#
#   使用者機器上已經裝著一個啟用了三份的版本。如果升級只是「新版只啟用
#   一份」而沒有清掉舊的兩份,他會看到「新版說只有一格,但清單裡還是三格」——
#   對他而言等於這個問題根本沒修,而我們會以為修好了。
#
#   做法:**真的把三份都打開**(模擬舊版裝完的狀態),再跑一次 enable-user,
#   然後斷言回到一份。中間那一步是重點 —— 直接跑兩次 enable-user 的話,
#   第二次面對的是已經只有一份的狀態,那條清理路徑從來不會被走到。
log "4c. 升級清理:先重現舊版的三份全開,再跑 enable-user"

# 製造前提。走的是產品自己的 enable-user-legacy-all —— 那一段程式碼
# **就是舊版跑的那一段**(對三份各呼叫一次 EnableLanguageProfile)。
# 手寫登錄檔去模擬的話,驗到的是我們對舊版的猜測,不是舊版。
"${INSTALL_DIR}/rime_ime_setup.exe" enable-user-legacy-all \
  > "${WORK}/legacy3.log" 2>&1 || true
cat "${WORK}/legacy3.log"

"${INSTALL_DIR}/rime_ime_setup.exe" user-profiles > "${WORK}/profiles-dirty.log" 2>&1 || true
dirty="$(tr -d '\r' < "${WORK}/profiles-dirty.log" | sed -n 's/^ENABLED_COUNT=//p' | head -1)"
echo "  收斂前:啟用了 ${dirty:-?} 份"

if [ "${dirty:-0}" -lt 2 ] 2>/dev/null; then
  # 前提沒做出來 → 這一節什麼都沒驗到。**明著紅**,不要讓它靜靜地綠。
  # 一個前提不成立的斷言必定通過,那比沒有這一節更糟。
  cat "${WORK}/profiles-dirty.log"
  note_fail "沒能重現舊版的三份全開(只有 ${dirty:-?} 份),
     所以「升級會把多餘的收回去」這一條**這一輪沒有被驗證**。
     這不是產品壞了,是這段測試沒有製造出它要測的前提。"
else
  ok "重現出舊版的狀態(${dirty} 份被啟用)"
  # 真正的動作:跑一次 enable-user,就像升級時安裝程式做的那樣。
  "${INSTALL_DIR}/rime_ime_setup.exe" enable-user > "${WORK}/reconverge.log" 2>&1 || true
  cat "${WORK}/reconverge.log"
  "${INSTALL_DIR}/rime_ime_setup.exe" user-profiles > "${WORK}/profiles-clean.log" 2>&1 || true
  clean="$(tr -d '\r' < "${WORK}/profiles-clean.log" | sed -n 's/^ENABLED_COUNT=//p' | head -1)"
  if [ "${clean}" = "1" ]; then
    ok "升級清理成立:${dirty} 份 → 1 份"
  else
    cat "${WORK}/profiles-clean.log"
    note_fail "跑完 enable-user 之後還有 ${clean:-?} 份被啟用。
     升級的使用者會看到「新版說只有一格,但清單裡還是 ${clean:-?} 格」——
     對他而言等於這個問題根本沒修。"
  fi

  # 反向測試:上面那條斷言會不會在該紅的時候紅?
  # 再弄髒一次,直接問 check --user —— 它必須**失敗**。
  "${INSTALL_DIR}/rime_ime_setup.exe" enable-user-legacy-all >/dev/null 2>&1 || true
  if "${INSTALL_DIR}/rime_ime_setup.exe" check --user --no-enum >/dev/null 2>&1; then
    note_fail "三份全開的狀態下 check --user 竟然通過 —— 那道關卡擋不住這個回歸,
     它報出來的綠燈不算數。(舊的斷言寫的是「至少一份」,正是這個形狀。)"
  else
    ok "反向測試通過:三份全開時 check --user 會紅"
  fi
  # 收乾淨,後面幾節要的是正常狀態。
  "${INSTALL_DIR}/rime_ime_setup.exe" enable-user >/dev/null 2>&1 || true
fi

# ══════════════════════════════════════════════════════════════════
#  5. 資料目錄:安裝完之後解析出來的是哪三個
# ══════════════════════════════════════════════════════════════════
#
# 不啟動引擎,所以不必等好幾分鐘的詞庫編譯。
# 要斷言的是:shared 指到 Program Files,而 user **不在** Program Files 底下。
log "5. 資料目錄解析"
"${INSTALL_DIR}/rime_service.exe" --print-dirs > "${WORK}/dirs.txt" 2>&1 \
  || { cat "${WORK}/dirs.txt"; die "rime_service.exe --print-dirs 失敗"; }
tr -d '\r' < "${WORK}/dirs.txt" | tee "${WORK}/dirs.clean"
d_shared="$(awk -F= '$1=="shared"{ sub(/^shared=/,""); print; exit }' "${WORK}/dirs.clean")"
d_user="$(awk   -F= '$1=="user"  { sub(/^user=/,"");   print; exit }' "${WORK}/dirs.clean")"

case "${d_shared}" in
  "${INSTALL_DIR_W}"*) ok "shared 指到安裝目錄: ${d_shared}" ;;
  *) note_fail "shared 是「${d_shared}」,預期在 ${INSTALL_DIR_W} 底下" ;;
esac
case "${d_user}" in
  "${INSTALL_DIR_W}"*)
    note_fail "使用者資料目錄落在 Program Files 底下(${d_user})
     一般權限的進程寫不進去,而 librime 寫不進去時**不會報錯** ——
     只是一個學過的詞都留不住,而且完全沒有錯誤訊息。" ;;
  *\\AppData\\*) ok "user 在 AppData 底下: ${d_user}" ;;
  *) note_fail "user 是「${d_user}」,預期在 %APPDATA% 底下" ;;
esac

# ── 5x. 同一個診斷動詞,經過 cmd 再跑一次 ────────────────────────────
#
# ⚠ 這一格是 service/main.cc 的 AttachParentConsoleForDiagnosticVerbs()
#   在整棵樹上僅有的自動守門。
#
#   rime_service.exe 現在是 **GUI 子系統**(CMakeLists.txt 的
#   WIN32_EXECUTABLE TRUE,為的是讓「開始」功能表的捷徑不再閃黑框)。
#   GUI 進程啟動時系統**不會**幫它補標準控制代碼,所以 --print-dirs /
#   --print-choice 這兩個「文件叫人用手跑」的診斷動詞,必須自己
#   AttachConsole(ATTACH_PARENT_PROCESS) 才有輸出。
#
#   ⚠ 上面 §5 那一格**證不了**這件事:`> dirs.txt 2>&1` 是 shell 幫它把
#     handle 填好的,AttachConsole 那一段寫不寫都一樣綠。
#
# ── ⚠ 這一格證不到什麼,要說清楚 ────────────────────────────────
#
#   只要我們在這一端把輸出接起來(檔案或管線都一樣),cmd 就會把那個
#   handle 傳給子行程,於是 GetStdHandle 不是 NULL、AttachConsole 那一段
#   根本不會執行。真正的「裸主控台」要有人**看著螢幕**才驗得到 ——
#   那一條在真機清單上,不在這裡。
#
#   這一格證得到的是另外兩件、而且都是這次改動真的可能弄壞的事:
#     1. 改成 GUI 子系統之後,這支 exe 從 cmd **還起得來**(進入點沒斷、
#        CRT 起得來)。/ENTRY:mainCRTStartup 若哪天被拿掉,連結會先紅;
#        但 CRT 的 app type 相關的啟動期崩潰只會在這裡現形。
#     2. 輸出內容與 §5 **一字不差** —— 也就是子系統換了之後,這個動詞的
#        行為沒有跟著漂掉。
log "  5x. 同一個動詞經過 cmd 再跑一次(GUI 子系統之後進入點還活著)"
set +e
cmd //c "\"${INSTALL_DIR_W}\\rime_service.exe\" --print-dirs" \
  > "${WORK}/dirs-cmd.raw" 2>&1
rc_cmd=$?
set -e
tr -d '\r' < "${WORK}/dirs-cmd.raw" > "${WORK}/dirs-cmd.clean"
if [ "${rc_cmd}" -ne 0 ]; then
  sed 's/^/     /' "${WORK}/dirs-cmd.clean"
  note_fail "cmd //c rime_service.exe --print-dirs 以 ${rc_cmd} 結束。
     改成 GUI 子系統之後它還起得來嗎?(windows/CMakeLists.txt 的
     /ENTRY:mainCRTStartup 是保住 main 進入點的那一行。)"
elif diff -u "${WORK}/dirs.clean" "${WORK}/dirs-cmd.clean" > "${WORK}/dirs-cmd.diff" 2>&1; then
  ok "經過 cmd 跑出來的三個目錄與 §5 一字不差"
else
  sed 's/^/     /' "${WORK}/dirs-cmd.diff"
  note_fail "同一個 --print-dirs,經過 cmd 跑出來的內容與 §5 不同 ——
     子系統換了之後這個診斷動詞的行為跟著漂掉了。"
fi

# ── 5b. 輸入模式 → 方案 / 簡繁(docs/settings-model.md §4)────────
#
# ⚠ 這一段斷言的是**裝好的那份二進位**,不是單元測試裡的那一份。
#   單元測試證明 ChooseSchema 這個函式是對的;這裡證明使用者真的裝到
#   機器上的那支 rime_service.exe 也會給同一個答案 ——
#   兩者之間隔著 CMake 的來源清單、連結器,以及「有沒有真的編進去」。
#
# 斷言的是**實際會送給引擎的那一組 option**,不是一個中間表示:
# 決定簡繁的是那幾個 option,而不是我們心裡想的那個 enum。
#
# --print-choice 不啟動引擎、不碰管道、不需要詞庫,所以在這個 job 裡跑得動。
log "5b. 輸入模式 → 方案 / 簡繁"

choice_out() { "${INSTALL_DIR}/rime_service.exe" --print-choice "$1" 2>&1 | tr -d '\r'; }
field_of() { echo "$1" | awk -F= -v k="$2" '$1==k { sub("^" k "=", ""); print; exit }'; }

# ⚠ --print-choice 會讀 ${USER_DIR}/${RS_WIN_SETTINGS_FILE},而設定裡釘的
#   方案優先於輸入模式。runner 上那個檔案不存在,所以讀到的就是「沒釘」——
#   但如果哪天有人在這支腳本前面寫了設定,這段斷言會安靜地變成在測別的東西。
if [ -f "${USER_DIR}/${RS_WIN_SETTINGS_FILE}" ]; then
  note_fail "runner 上竟然已經有 ${RS_WIN_SETTINGS_FILE} —— 下面的斷言測到的
     會是那份設定的覆寫,而不是輸入模式的推導。"
fi

check_choice() {  # langid 期望方案 期望簡繁 期望開啟的字形option
  local lang="$1" want_schema="$2" want_variant="$3" want_opt="$4"
  local out; out="$(choice_out "${lang}")"
  local got_schema got_variant
  got_schema="$(field_of "${out}" schema)"
  got_variant="$(field_of "${out}" variant)"
  if [ "${got_schema}" != "${want_schema}" ] || \
     [ "${got_variant}" != "${want_variant}" ]; then
    note_fail "${lang} 給的是「${got_schema} / ${got_variant}」,
     預期「${want_schema} / ${want_variant}」。
     這正是使用者回報過的那個缺陷:選了簡體輸入法、打出來是繁體字。"
    return
  fi
  if [ -n "${want_opt}" ] && \
     ! echo "${out}" | grep -q "^option=${want_opt}=true$"; then
    note_fail "${lang} 沒有把 ${want_opt} 設成 true。實際送出去的是:
$(echo "${out}" | grep '^option=' | sed 's/^/       /')"
    return
  fi
  # radio group 的互斥:同組**只能有一個** true。兩個都真的話
  # t2s 之後會再串一次 t2tw,輸出變成沒有人要的東西。
  local on
  on="$(echo "${out}" | grep -cE '^option=zh_(hant|hans|hant_hk|hant_tw)=true$' || true)"
  if [ "${on}" != "1" ]; then
    note_fail "${lang} 的字形開關同時有 ${on} 個是 true(應該正好 1 個)。"
    return
  fi
  ok "${lang} → ${got_schema} / ${got_variant} / ${want_opt}"
}

check_choice 0x0804 luna_pinyin_tw simplified  zh_hans
check_choice 0x0404 luna_pinyin_tw traditional zh_hant_tw
check_choice 0x0C04 luna_pinyin_tw traditional zh_hant_hk

# 反向測試:認不出來的語言必須**完全不碰**簡繁(規範 §4.2:不要預設繁體)。
# 少了這一條,一個「永遠設成繁體」的實作也會讓上面兩條繁體的斷言通過。
#
# ⚠ 判準是「不碰**簡繁**」,不是「一個 option 都不送」。
#   `ascii_mode` 一定會送(schema_choice.h:157「永遠有」)—— 它是模式不是
#   三態偏好,沒有「不干預」那一格。舊版寫成 `grep -q '^option='`,
#   在 c9fc502 把 ascii_mode 加進 BuildOptionPlan 之後就變成**恆假**,
#   而這一格從那時起一次都沒有跑過(install-x64 這條車道整個不存在)。
neg="$(choice_out 0x0409)"
neg_variant_opts="$(echo "${neg}" | grep -cE '^option=zh_' || true)"
if [ "$(field_of "${neg}" variant)" = "(不干預)" ] && \
   [ "${neg_variant_opts}" = "0" ] && \
   echo "${neg}" | grep -q '^option=ascii_mode='; then
  ok "0x0409(en-US)完全不碰簡繁(0 個 zh_* option) —— 上面幾條不是恆真的"
else
  note_fail "0x0409 竟然動了簡繁,或者連 ascii_mode 都不送了(送了 ${neg_variant_opts} 個 zh_* option):
$(echo "${neg}" | sed 's/^/       /')
     規範 §4.2:認不出來的輸入模式一律不表示意見,不要預設繁體。
     ⚠ 這一條只看 zh_* 那幾個開關。ascii_mode 必須照送 —— 它不見了
     代表 BuildOptionPlan 整個沒回東西,那一樣是紅的。"
fi

# ══════════════════════════════════════════════════════════════════
#  5c. 冷啟動:空的使用者資料目錄 + 沒有人先把服務跑起來
# ══════════════════════════════════════════════════════════════════
#
# ⚠⚠ **這一節補的是這整支腳本以前最大的一個洞。**
#
#   底下的 §6 是這樣開始的:腳本**自己**把 rime_service.exe 跑起來,
#   等它就緒,然後才驗端到端。§6c 也一樣 —— 它跑 rime_tsf_host 的時候,
#   服務已經起來而且預熱完了。
#
#   也就是說:「使用者切到本輸入法 → 瘦 DLL 在 ActivateEx 裡把服務叫起來」
#   這一條路,**從來沒有被驗過一次**。而它正是每一個使用者的第一次經驗,
#   也正是 2026-08 那次回報壞掉的地方(內建 Administrator 帳號上,
#   那條路被「提權的宿主不啟動服務」擋掉,於是沒有系統匣圖示、
#   沒有設定視窗、打不出字 —— 三個症狀一個原因,零個錯誤訊息)。
#
#   這一節把它變成:**沒有人先動手,只有 ActivateEx。**
#   而且使用者資料目錄是空的 —— 那才是第一次啟動真正的樣子。
log "5c. 冷啟動 + 服務由 DLL 自動啟動"

# ⚠ 這一節非要 rime_tsf_host.exe 不可:它是唯一能逼系統走完
#   「載入 DLL → ActivateEx」的東西,而「服務由 DLL 自動啟動」就掛在那裡。
#   沒有它就不是「少驗一點」,是這一整條路完全沒有守門 —— 所以不靜靜跳過。
if [ -z "${HOST}" ]; then
  note_fail "沒有給 --host,冷啟動與「服務自動啟動」這一整節驗不到。
     那條路是每個使用者的第一次經驗(切過去 → 系統匣圖示、設定視窗、
     打得出字),而它在本輪之前從來沒有被驗過一次。"
fi
if [ -n "${HOST}" ]; then

# 這一節之後 §6 才會自己啟動服務,所以這裡結束前一定要把服務停掉 ——
# 不然 §6 那一支會被單一實例的互斥鎖擋掉,而它會安靜地以 0 結束。
coldstart_stop_service() {
  "${INSTALL_DIR}/rime_ime_setup.exe" stop-service --dir "${INSTALL_DIR_W}" \
    > "${WORK}/coldstart-stop.log" 2>&1 || true
  for _ in $(seq 1 30); do
    [ "$(count_service)" -eq 0 ] && break
    sleep 1
  done
}

# 進程數。用 tasklist 而不是 kill -0:自動啟動的那一支不是這個 shell 的子行程。
count_service() {
  tasklist 2>/dev/null | grep -c -i 'rime_service\.exe' || true
}

# ── 前置:確定現在真的是「冷」的 ────────────────────────────────
if [ "$(count_service)" -ne 0 ]; then
  note_fail "5c 開始前就已經有 rime_service.exe 在跑 —— 這一節要驗的是
     『沒有人先動手』,有人先動手就驗不到了。"
  coldstart_stop_service
fi
# 使用者資料目錄清空。第一次安裝時它本來就不存在,而重跑這支腳本時會留著。
rm -rf "${USER_DIR}"
if [ -d "${USER_DIR}" ]; then
  note_fail "刪不掉 ${USER_DIR} —— 冷啟動這一節驗不到(目錄裡已經有部署好的產物)"
else
  ok "使用者資料目錄是空的:${USER_DIR}"
fi

# ── 這台機器的提權形狀 ──────────────────────────────────────────
#
# ⚠ runner 預設就是系統管理員,所以「提權的宿主不啟動服務」這條規則在
#   CI 上一直是**生效**的 —— 這正是那個缺陷沒有被 CI 抓到的原因之一。
#   現在把判定印出來並據此決定要斷言什麼,而**不可以**因為形狀不對就
#   安靜地跳過:那是這個專案抓過很多次的「測試是綠的,因為它沒在測」。
"${INSTALL_DIR}/rime_ime_setup.exe" doctor --no-engine --no-scan \
  > "${WORK}/doctor-coldstart.log" 2>&1 || true
ELEV="$(tr -d '\r' < "${WORK}/doctor-coldstart.log" \
        | sed -n 's/^.*\[INFO\] 提權判定: \([a-z-]*\).*$/\1/p' | head -1)"
if [ -z "${ELEV}" ]; then
  note_fail "doctor 沒有印出「提權判定:」那一行 —— 第 4 節那一格沒有在看,
     而它正是使用者回報時唯一能分辨『刻意不啟動』與『壞掉』的東西。"
  ELEV="(沒印出來)"
fi
log "  runner 的提權判定 = ${ELEV}"

case "${ELEV}" in
  normal|whole-session-elevated)
    # 這兩種都必須自動啟動。normal 是多數使用者;whole-session-elevated
    # 是內建 Administrator / 關掉 UAC 的那一種,也就是這次回報的那一種。
    EXPECT_AUTOSTART=1 ;;
  split-token-elevated|service-account|unknown)
    EXPECT_AUTOSTART=0 ;;
  *)
    EXPECT_AUTOSTART=0 ;;
esac

# ── ActivateEx:只切過去,不打字 ────────────────────────────────
#
# 刻意不送任何按鍵。以前服務是靠「第一顆按鍵走到 EnsureReady」才起來的,
# 而這一節要驗的是**切過去就該起來**(系統匣圖示與設定視窗都在服務裡,
# 使用者不該為了看到 UI 而先打一個字)。
set +e
"${HOST}" --langid "${ACTIVE_LANGID}" --require-activate \
          --trace "$(w "${WORK}/coldstart-trace.log")" --wait-ms 5000 \
          > "${WORK}/coldstart-activate.log" 2>&1
rc=$?
set -e
tr -d '\r' < "${WORK}/coldstart-activate.log" | sed 's/^/    /'
[ "${rc}" -eq 0 ] || note_fail "冷啟動的 ActivateEx 那一趟以 ${rc} 結束"

# ── 服務有沒有自己起來 ──────────────────────────────────────────
SAW=0
for _ in $(seq 1 30); do
  if [ "$(count_service)" -gt 0 ]; then SAW=1; break; fi
  sleep 1
done

if [ "${EXPECT_AUTOSTART}" -eq 1 ]; then
  if [ "${SAW}" -eq 1 ]; then
    ok "**服務由瘦 DLL 自動啟動了**(提權判定 ${ELEV})—— 這一格從本輪之前
     一直是紙上的:以前每一次都是測試腳本自己先把服務跑起來。"
  else
    note_fail "切到本輸入法之後,服務**沒有自己起來**(提權判定 ${ELEV})。
     使用者看到的會是:沒有系統匣圖示、沒有設定視窗、打不出字 ——
     三個症狀一個原因,而且沒有任何錯誤訊息。
     瘦 DLL 的除錯記錄裡會有一行說明它為什麼沒啟動:
$(tr -d '\r' < "${WORK}/coldstart-trace.log" 2>/dev/null | grep -a '服務' | sed 's/^/       /')"
  fi
else
  # 這台機器的形狀不允許自動啟動。那**不是**跳過 —— 我們仍然斷言兩件事:
  #   (1) 它確實沒有啟動(規則真的生效,不是碰巧)
  #   (2) 拒絕的理由有被寫下來(不然使用者看到的就只是「壞掉」)
  if [ "${SAW}" -eq 0 ]; then
    ok "提權判定 ${ELEV} → 刻意不自動啟動,而且真的沒有啟動"
  else
    note_fail "提權判定 ${ELEV} 說不該自動啟動,但服務還是起來了 ——
     那條保護沒有生效。"
  fi
  if tr -d '\r' < "${WORK}/coldstart-trace.log" 2>/dev/null \
       | grep -aq '不啟動服務(刻意)'; then
    ok "除錯記錄裡寫明了拒絕的理由"
  else
    note_fail "拒絕啟動而**沒有留下理由** —— 一個刻意的拒絕不該長得跟壞掉一樣。"
  fi
  printf '\033[1;33m  ⚠ 這台 runner 的提權形狀是 %s,所以「服務自動啟動」這一條\033[0m\n' "${ELEV}" >&2
  printf '\033[1;33m    在這裡驗不到。使用者回報的那一種是 whole-session-elevated。\033[0m\n' >&2
  note_fail "「服務由 DLL 自動啟動」在這台 runner 上驗不到(形狀是 ${ELEV})。
     這條路徑是每個使用者的第一次經驗,不可以沒有守門 ——
     請改用一台形狀是 normal 或 whole-session-elevated 的 runner。"
fi

# ── 冷啟動的第一次打字 ──────────────────────────────────────────
#
# 服務起來之後要先把詞庫編譯完(首次安裝是一到數分鐘)。使用者在那段時間
# 打字打不出中文是正常的,但**打完之後必須成功** —— 那才是「第一次使用」
# 真正的驗收標準。
if [ "${SAW}" -eq 1 ]; then
  log "  等服務就緒(冷啟動要編譯詞庫,可能數分鐘)"
  READY_COLD=0
  for i in $(seq 1 900); do
    if "${PROBE}" --connect-only --attempts 1 > "${WORK}/coldstart-probe.log" 2>&1; then
      READY_COLD=1
      break
    fi
    sleep 1
    [ $((i % 60)) -eq 0 ] && log "    ...已等 ${i}s"
  done
  if [ "${READY_COLD}" -eq 1 ]; then
    ok "冷啟動的服務在自己編譯完詞庫之後接得起連線"
    set +e
    "${HOST}" --langid "${ACTIVE_LANGID}" --require-activate --require-eaten \
              --keys nihao1 --expect 你好 \
              --trace "$(w "${WORK}/coldstart-type-trace.log")" --wait-ms 5000 \
              > "${WORK}/coldstart-type.log" 2>&1
    rc=$?
    set -e
    tr -d '\r' < "${WORK}/coldstart-type.log" | sed 's/^/    /'
    if [ "${rc}" -eq 0 ]; then
      ok "**冷啟動之後第一次打字就打得出「你好」**(空的使用者目錄、
     沒有人先幫它部署 —— 這正是新使用者的第一次經驗)"
    else
      note_fail "冷啟動之後第一次打字失敗(結束碼 ${rc})"
    fi
  else
    note_fail "冷啟動的服務在 900 秒內沒有接起連線 —— 使用者的第一次使用
     會是『切過去了,然後什麼都不會發生』。"
  fi
fi

# 把場地還原:§6 要自己啟動一支服務。
coldstart_stop_service
if [ "$(count_service)" -ne 0 ]; then
  note_fail "5c 結束時還有 rime_service.exe 在跑 —— §6 那一支會被單一實例
     的互斥鎖擋掉,然後安靜地以 0 結束,而 §6 會變成在驗這一支。"
fi

# ══════════════════════════════════════════════════════════════════
#  5d. 第三種「冷」:服務被結束掉之後,由瘦 DLL 重新拉起
# ══════════════════════════════════════════════════════════════════
#
# ⚠ 「冷」有三種,而在這一節之前 CI 只驗了第一種:
#
#     1. 首次安裝          → §5c(空的使用者目錄 + 首次部署)
#     2. 升級之後          → §13(安裝程式停掉舊服務、起一支新的)
#     3. **使用者重新開機** → 就是這一節
#
#   第三種在使用者機器上**每天都會發生**:他開機、登入、切到這個輸入法,
#   而服務進程並不會自己隨開機啟動 —— 它是被瘦 DLL 在 ActivateEx 時拉起來的。
#   也就是說,每一天的第一次打字走的都是這條路,而在這之前沒有人在守它。
#
#   與 §5c 的差別:使用者目錄**已經部署好了**(詞庫編過了),所以這一節
#   驗的不是「等得夠不夠久」,而是「服務重新起來之後,建 session 有沒有
#   在預算之內」—— 那正是 2026-08-09 一路追下來的那個東西。
log "5d. 第三種冷:服務被結束之後由瘦 DLL 重新拉起(= 使用者每天開機)"
[ "$(count_service)" -eq 0 ] \
  && ok "現在沒有服務在跑(與使用者剛登入時一樣)" \
  || note_fail "5d 開始前還有服務在跑 —— 這一節驗不到「由 DLL 重新拉起」"

# ⚠ 分成兩步,而且這正是使用者的動線:先切到這個輸入法(服務被拉起來),
#   過一會兒才開始打字。
#
#   合成一步會量到別的東西:同一支宿主一邊啟動服務、一邊馬上送按鍵時,
#   服務進程剛建好的視窗(設定視窗 / 系統匣)會把**前景**拿走,而 TSF
#   只把按鍵交給擁有前景的執行緒 —— 於是一顆鍵都不會到達文字服務,
#   而報表看起來像「打字失敗」。實測過(CI run 31314468397)。
log "  5d-1. 切到這個輸入法(只 activate,不打字)—— 服務應該被拉起來"
set +e
"${HOST}" --langid "${ACTIVE_LANGID}" --require-activate \
          --trace "$(w "${WORK}/relaunch-activate-trace.log")" --wait-ms 5000 \
          > "${WORK}/relaunch-activate.log" 2>&1
rc=$?
set -e
[ "${rc}" -eq 0 ] || note_fail "5d 的 activate 那一趟以 ${rc} 結束"
RELAUNCHED=0
for _ in $(seq 1 30); do
  if [ "$(count_service)" -gt 0 ]; then RELAUNCHED=1; break; fi
  sleep 1
done
[ "${RELAUNCHED}" -eq 1 ] \
  && ok "瘦 DLL 把服務重新拉起來了" \
  || note_fail "服務**沒有**被重新拉起來 —— 使用者每天開機切過來都會是這樣"

# 等它就緒。詞庫已經編好了,所以這裡量的是「重啟之後多久能服務」。
log "  5d-2. 等它就緒(詞庫已經編好,所以這一段應該很短)"
READY_RE=0
for i in $(seq 1 180); do
  if "${PROBE}" --connect-only --attempts 1 > "${WORK}/relaunch-probe.log" 2>&1; then
    READY_RE=1
    break
  fi
  sleep 1
done
[ "${READY_RE}" -eq 1 ] \
  && ok "重新拉起來的服務在 ${i} 秒內接得起連線" \
  || note_fail "重新拉起來的服務 180 秒內接不起連線 —— 使用者每天開機之後
     切過來都打不出中文,而詞庫早就編好了。"

log "  5d-3. 現在打字(= 使用者每天開機之後的第一串字)"
set +e
"${HOST}" --langid "${ACTIVE_LANGID}" --require-activate --require-eaten \
          --keys nihao1 --expect 你好 \
          --trace "$(w "${WORK}/relaunch-trace.log")" --wait-ms 5000 \
          > "${WORK}/relaunch-type.log" 2>&1
rc=$?
set -e
tr -d '\r' < "${WORK}/relaunch-type.log" | sed 's/^/    /'
if [ "${rc}" -eq 0 ]; then
  ok "**服務被結束之後,瘦 DLL 自己把它拉起來,而且第一次打字就打得出「你好」**
     —— 這是使用者每天開機之後的第一次打字。"
else
  note_fail "服務被結束之後重新拉起,第一次打字失敗(結束碼 ${rc})。
     使用者每天開機切過來打的第一串字就會是英文,而且沒有任何錯誤訊息。
     ⚠ 這一格與 §5c 的差別是:詞庫**已經編好了**,所以慢的不會是部署。
       去看 ${DIAG_SVC_LOG} 裡的 SESSION_NEW_MS 與預熱耗時。"
fi

# 場地還原:§6 要自己啟動一支服務。
coldstart_stop_service
if [ "$(count_service)" -ne 0 ]; then
  note_fail "5d 結束時還有 rime_service.exe 在跑 —— §6 那一支會被單一實例擋掉"
fi
fi  # -n "${HOST}"


# ══════════════════════════════════════════════════════════════════
#  6. 用**安裝好的**東西真的打出「你好」
# ══════════════════════════════════════════════════════════════════
#
# 刻意不給 --shared / --user:要驗的正是「安裝出來的那一份自己就夠了」。
# 走的路是 rime_probe → 真的具名管道 → 安裝好的 rime_service
#           → rime_shell → librime → 安裝好的詞庫。
# 也就是 DLL 會走的每一段,除了 TSF 本身。
log "6. 端到端:用安裝好的服務與資料打出「你好」"

# ⚠ 相片在 §2 裝完的當下就照了(見上面),**不是在這裡**。
#
#   原本這一行就在這裡,而那讓 §5c / §5d(冷啟動、瘦 DLL 自己拉起服務)
#   寫進 Program Files 的東西全部被算進基準線 —— 於是它們永遠比對得過。
#
#   實測(2026-08-12,run 31526574022 / install-x64):把 service/main.cc
#   的 RedirectStdIoIfDetached 從 %LOCALAPPDATA% 改成 %ProgramFiles%,
#   服務真的把 diagnostics/service.log 寫進了安裝目錄(${INSTALL_DIR}),
#   ⚠ 這一行原本把安裝路徑連同產品名一起寫成字面值,而 scripts/
#     verify_product_ids.sh §3 與 windows/verify_product_names.sh §5
#     連註解一起掃 —— 那是對的:改名時漏掉的正是註解裡那一份,
#     而它會在下一個人讀的時候變成錯的說明。識別碼一律向
#     product.env 那條線要(這裡直接用已經讀好的 ${INSTALL_DIR})。
#   而這一條照樣印
#
#       ✓ 安裝目錄跑完之後一個檔案都沒變(使用者資料沒有寫進 Program Files)
#
#   抓到它的是**別的**斷言(§6d 的「找不到 service.log」與 §8 的
#   「安裝目錄還留著 1 個我們的檔案」)。這一條自己是瞎的,而且瞎在
#   它最該看見的那條路上 —— 冷啟動那一支才是使用者機器上真正跑的服務。

READY="${WORK}/ready.txt"
SVC_LOG="${WORK}/service.log"
: > "${SVC_LOG}"
SVC_PID=""

# ── 起 / 停這一段的服務 ──────────────────────────────────────────
#
# 抽成函式是因為 §6g 要**重起好幾次**:那一節的每一個案例都要自己一份
# 乾淨的使用者詞典,而詞典是服務進程開著的(見 §6g 的長註解)。
# 日誌一律附加到同一個 service.log,前面加一行標題分段 ——
# 後面 §15 掃的是整份,分成好幾個檔案會讓它漏掉。
start_probe_service() {   # $1 = 這一趟的標籤(只進日誌與錯誤訊息)
  rm -f "${READY}"
  echo "=== [服務啟動:$1] ===" >> "${SVC_LOG}"
  "${INSTALL_DIR}/rime_service.exe" \
    --no-ui --wait-deploy 1200 \
    --ready-file "$(w "${READY}")" --quit-after 900 \
    >> "${SVC_LOG}" 2>&1 &
  SVC_PID=$!
  local i
  for i in $(seq 1 1200); do
    [ -f "${READY}" ] && break
    if ! kill -0 "${SVC_PID}" 2>/dev/null; then
      echo "--- service.log ---"; tr -d '\r' < "${SVC_LOG}"
      die "服務進程提前結束了($1)"
    fi
    sleep 1
    [ $((i % 60)) -eq 0 ] && log "    ...已等 ${i}s"
  done
  [ -f "${READY}" ] \
    || { tr -d '\r' < "${SVC_LOG}"; die "服務在 1200 秒內沒有就緒($1)"; }
}

stop_probe_service() {
  "${INSTALL_DIR}/rime_ime_setup.exe" stop-service --dir "${INSTALL_DIR_W}" \
    > "${WORK}/stop-restart.log" 2>&1 || true
  if [ -n "${SVC_PID}" ]; then
    kill "${SVC_PID}" 2>/dev/null || true
    wait "${SVC_PID}" 2>/dev/null || true
  fi
  SVC_PID=""
  # ⚠ 一定要等到進程真的不見,而且不能只等自己的子行程。
  #   單一實例的互斥鎖還被握著的話,下一支會**安靜地以 0 結束** ——
  #   症狀是「ready 檔一直不出現」,而那看起來像部署很慢。
  local i
  for i in $(seq 1 30); do
    [ "$(tasklist 2>/dev/null | grep -c -i 'rime_service\.exe' || true)" -eq 0 ] \
      && break
    sleep 1
  done
  # ⚠ 一定要明著 return 0。這個 for 是函式的最後一句,而它最後一輪的
  #   `[ ... ] && break` 在「等滿 30 秒都沒停下來」時回 1 —— 配上 set -e,
  #   整支腳本會在**這裡**當場結束,而畫面上不會有任何一句話說明原因。
  #   真的停不下來由下一步的 ready 檔逾時來報,那句話說得清楚得多。
  return 0
}

cleanup() {
  if [ -n "${SVC_PID}" ] && kill -0 "${SVC_PID}" 2>/dev/null; then
    # 走正常的停止路徑(送結束事件),順便把它也驗一次。
    "${INSTALL_DIR}/rime_ime_setup.exe" stop-service --dir "${INSTALL_DIR_W}" \
      > "${WORK}/stop.log" 2>&1 || true
    kill "${SVC_PID}" 2>/dev/null || true
    wait "${SVC_PID}" 2>/dev/null || true
  fi
}
trap cleanup EXIT

log "  等待服務就緒(首次部署要編譯詞庫,可能數分鐘)"
start_probe_service "§6"
ok "服務就緒"

# ⚠ --attempts 1:「ready 檔存在」必須等於「立刻連得上」。
#
#   這一步曾經間歇性地紅在一句「連不上服務或握手失敗」上,而服務的日誌
#   乾乾淨淨。當時 probe 寫的是「重試 100 次、每次睡 100ms」,看起來很有
#   耐心 —— 實際上連線狀態機的退避會把那 100 次吃到只剩幾次(握手不合的
#   退避是 30 秒,於是只剩**一次**),所以那個迴圈既沒有真的重試,
#   也把「服務其實沒在監聽」蓋成了「大概是版本不合」。
#
#   現在兩件事都被修在正確的那一側:服務寫 ready 檔之前,管道確定已經
#   接得起連線、引擎也預熱過了。所以第一次就該成功,而失敗時 probe 會
#   說出是哪一步(開管道 / 握手 / 建 session)以及對方回了什麼。
set +e
# ⚠ `--select-text 你好` 而不是 `--select 1`。理由見 §6g 開頭那一段:
#   候選的**位置**是引擎的輸出,前面每一個上屏過的斷言都會把它推著跑。
#   這一格問的是「打不打得出你好」,那就直接問內容。
"${PROBE}" --keys nihao --select-text 你好 --schema luna_pinyin_tw --expect 你好 \
  --attempts 1 > "${WORK}/probe.log" 2>&1
rc=$?
set -e
tr -d '\r' < "${WORK}/probe.log"
if [ "${rc}" -ne 0 ]; then
  # ⚠ 先把 [pipe] 那幾行單獨挑出來。監聽迴圈非預期結束時只會留下這幾行,
  #   而它們會淹沒在幾百行 glog 的詞庫編譯警告裡 —— 那正是上一次查這個
  #   缺陷時,最該被看到卻沒被看到的東西。
  echo "--- service.log:管道監聽迴圈 ---"
  if tr -d '\r' < "${WORK}/service.log" | grep -a '^\[pipe\]'; then
    echo "  ↑ 監聽迴圈有話要說 —— 問題在服務端,不在協議。"
  else
    echo "  (沒有 [pipe] 行:監聽迴圈沒有回報任何問題)"
  fi
  echo "--- service.log:預熱 ---"
  tr -d '\r' < "${WORK}/service.log" | grep -a '預熱' \
    || echo "  (沒有預熱那一行 —— 引擎沒有預熱過,第一次建 session 會很慢)"
  echo "--- service.log(其餘,已濾掉 glog)---"
  tr -d '\r' < "${WORK}/service.log" | grep -v -E '^[WIEF][0-9]{4,8} ' || true
  note_fail "probe 以 ${rc} 結束 —— 診斷見上面 probe 的輸出(它會指出是
     開管道 / 握手 / 建 session 哪一步失敗,不要再猜)"
else
  # 錨定整行精確比對。只用 grep -q 你好 是不夠的:上屏成「你好嗎」一樣會過。
  if grep -qE '^>>> COMMIT: "你好"$' <(tr -d '\r' < "${WORK}/probe.log"); then
    ok "經由具名管道以安裝好的資料打出「你好」"
  else
    note_fail "probe 的輸出裡沒有 >>> COMMIT: \"你好\""
  fi
fi

# ══════════════════════════════════════════════════════════════════
#  6g. 簡繁真的送到引擎了嗎(使用者實機回報的那一條)
# ══════════════════════════════════════════════════════════════════
#
# 使用者回報:設定裡選了簡體,狀態列畫「简」,而打出來是繁體。
#
# ⚠ **判準只能用「逆号 / 拟好」這一類的字。** 你好 / 妳好 / 你 在簡繁兩套
#   字集裡長得一模一樣,拿它們斷言等於沒斷言 —— 上面 §6 那一格用 `你好`
#   是對的(它問的是「打不打得出字」),但拿來問簡繁就會永遠是綠的。
#
#   nihao 的那兩個字:簡體 = 逆号 / 拟好,繁體 = 逆號 / 擬好。
#
# ⚠ --variant 在**連線之前**寫設定檔。順序就是它的全部意義:服務在
#   SESSION_NEW 那一趟現場讀設定檔,所以「先寫檔再連線」= 「使用者在
#   設定裡選了簡體,然後開一個新程式」。
#
# ══ 這一節為什麼要自己重起服務、自己清詞典 ═══════════════════════
#
# ⚠ 2026-08-12 之前,這兩個案例是這樣紅的:
#
#     16:15:21.322  [新 session]  2. 妳好  3. 逆号  → COMMIT "逆号"  ✓
#     16:15:21.398  [換方案]      2. 逆号  3. 妳好  → COMMIT "妳好"  紅
#
#   兩邊**都是簡體**,所以產品那一格(648c02c)是好的。紅的原因是**測試
#   沒有隔離**:兩次相差 76 毫秒,中間唯一發生的事,是上一個斷言把「逆号」
#   上屏了 —— librime 的使用者詞典就地學習,把它從第 3 位拱到第 2 位,
#   而斷言寫的是 `--select 3`。
#
#   所以真正的形狀不是「這兩行寫錯了」,是:
#   **任何依賴候選位置的斷言,在這支腳本裡都會隨著前面的斷言漂。**
#   位置是引擎的輸出,不是我們的約定;拿它當判準,等於把「使用者詞典
#   剛剛學了什麼」偷偷寫進測試的前提裡。
#
#   兩件事都要做,少一件就只是把症狀壓下去:
#
#     1. **用內容選候選**(`--select-text 逆号`)—— 位置不再是判準。
#     2. **每個案例自己一份使用者詞典** —— 斷言之間互不影響。
#        只做第 1 件的話,詞典仍然是共用的:librime 學到夠多之後,
#        「逆号」會直接變成第 1 個候選,而那時第 1 件也保不住
#        §6c 那幾格 `nihao1`(用按鍵 '1' 選第一個)的斷言。
#
#   第 2 件的做法是「每個案例前重置」:停掉服務 → 刪掉 `*.userdb`
#   → 重起。**不是**換一個使用者目錄 —— 換目錄會連 `build/` 底下編好的
#   詞庫一起換掉,每個案例都要重編一次(以分鐘計)。`*.userdb` 是
#   librime 唯一會「學」的東西,刪它就夠,而設定檔、default.custom.yaml、
#   編譯產物全部留著。
#
# ⚠ 詞典是**服務進程開著的**(mmap),所以非得先停掉服務不可。
#   服務跑著的時候 rm 會失敗,而失敗的樣子是「刪了但檔案還在」——
#   下面那一格會當場點名。
#
# ⚠ **仍然是位置判準的:§5c / §5d / §6c / §13 那五處 `nihao1`。**
#   那是 rime_tsf_host.exe 送的按鍵 '1'(= 選第一個候選),而它經由 TSF
#   走,看不到候選清單,沒有「用內容選」這個選項。護住它們的是這一節:
#   §6g 結束時會再重置一次,所以 §6c 拿到的是一份沒學過任何詞的詞典;
#   而整支腳本上屏過的中文只有「你好」與這一節的「逆号」,前者本來就是
#   第 1 個候選,學習只會把它釘得更牢。⚠ 哪天有人加了一個會上屏別的字
#   的案例,那五處就會開始漂 —— 屆時要修的是 tsf_host 那一側。
log "6g. 簡繁:設定裡選簡體之後,新開的程式打出來要是簡體"

# 刪掉 librime 學過的詞,其餘一個檔案都不動。**呼叫前服務必須是停的。**
reset_user_dict() {   # $1 = 這一趟的標籤
  rm -rf "${USER_DIR}"/*.userdb "${USER_DIR}"/*.userdb.txt 2>/dev/null || true
  local left
  left="$( (find "${USER_DIR}" -maxdepth 1 -name '*.userdb*' 2>/dev/null || true) \
           | wc -l | tr -d ' ')"
  if [ "${left}" -ne 0 ]; then
    note_fail "$1:刪不掉使用者詞典(還剩 ${left} 個 *.userdb*)——
     多半是服務還握著它。這個案例的候選排序會被前一個案例的上屏帶著跑,
     而那正是這一節要隔離掉的東西。"
  else
    ok "$1:使用者詞典已重置(從沒學過任何詞的狀態開始)"
  fi
}

# ── 案例一:不帶 --schema(靠 langid 選方案,驗新 session 那條路)──
log "  6g-1. 重置詞典並重起服務"
stop_probe_service
reset_user_dict "案例一"
start_probe_service "6g 案例一"

set +e
"${PROBE}" --variant simplified --keys nihao --select-text 逆号 --expect 逆号 \
  --attempts 1 > "${WORK}/probe-variant-new.log" 2>&1
rc_v1=$?
set -e
tr -d '\r' < "${WORK}/probe-variant-new.log"
if [ "${rc_v1}" -eq 0 ] \
   && grep -qE '^>>> COMMIT: "逆号"$' <(tr -d '\r' < "${WORK}/probe-variant-new.log"); then
  ok "設定選簡體 → 新 session 打出簡體(逆号)"
else
  note_fail "設定選了簡體,新開的 session 打出來卻不是簡體。
     ⚠ 如果上屏的是「逆號」,那就是使用者回報的那一條:偏好送到了畫面、
     沒送到引擎(或送了又被換方案洗掉)。看 windows/common/schema_choice.cc
     的 BuildOptionPlan 與 service/engine.cc 的 SelectAndApply。"
fi

# ── 案例二:**帶** --schema,釘住「換方案會洗掉簡繁」那條路 ────────
#
# ⚠ 這一條釘的是 Engine::SelectSchema。librime 的
#   ConcreteEngine::InitializeOptions() 每一次載入方案都會把 switches 重設回
#   方案宣告的值,而 luna_pinyin_tw 的 __patch 把 switches/@2/reset 設成 3
#   —— 換一次方案,zh_hant_tw 回到真、zh_hans 回到假,使用者剛選的簡體
#   被悄悄洗掉。
#
#   修法有兩截,而且兩截都有原始碼層面的守門(windows/ 在 Ubuntu 上
#   編不起來,所以只能那樣守):
#     · Engine::SelectAndApply(選方案 → 立刻重套簡繁)
#       ← audit_single_source.sh 規則 2「只能有一個裸的 rs_select_schema」
#     · 重套時拿的是**設定檔**那一份,不是引擎手上那份過期的複本(648c02c)
#       ← audit_single_source.sh 規則 3 + common/schema_choice.cc 的
#         PickVariantPrefForSchemaSwitch(tests/test_schema_choice.cc 驗判斷)
#   這一格是那兩件事在**真的服務**上的證據。
log "  6g-2. 重置詞典並重起服務(案例二不可以看到案例一學過的東西)"
stop_probe_service
reset_user_dict "案例二"
start_probe_service "6g 案例二"

set +e
"${PROBE}" --variant simplified --schema luna_pinyin_tw \
  --keys nihao --select-text 逆号 --expect 逆号 \
  --attempts 1 > "${WORK}/probe-variant-sel.log" 2>&1
rc_v2=$?
set -e
tr -d '\r' < "${WORK}/probe-variant-sel.log"
if [ "${rc_v2}" -eq 0 ] \
   && grep -qE '^>>> COMMIT: "逆号"$' <(tr -d '\r' < "${WORK}/probe-variant-sel.log"); then
  ok "換方案之後簡繁還在(逆号)"
else
  note_fail "換一次方案就把簡繁洗掉了 —— 上屏的多半是「逆號」。
     Engine::SelectSchema 那條路少了重套簡繁那一步,或者重套時拿的是
     引擎手上那份**過期的**偏好複本。
     見 service/engine.cc 的 SelectAndApply、service/pipe_server.cc 的
     kSelectSchema,以及 windows/audit_single_source.sh 的規則 2 與規則 3。
     ⚠ 這一格現在有自己的一份使用者詞典,所以它**不會**再因為案例一
       上屏過什麼而紅 —— 真的紅就是產品的事。"
fi

# 場地還原:後面 §6c 起的每一格都用這一支服務,而其中 `nihao1` 那幾處
# 仍然是位置判準(見本節開頭)。交還一份沒學過任何詞的詞典給它們。
log "  6g-3. 重置詞典並重起服務(交還給 §6c 起的那幾節)"
stop_probe_service
reset_user_dict "交還給 §6c"
start_probe_service "§6c 起"

# ══════════════════════════════════════════════════════════════════
#  6c. **經由真的 TSF** 打一次字
# ══════════════════════════════════════════════════════════════════
#
# ⚠ 上面 §6 走的是具名管道 —— 它**繞過 TSF**。它證明的是
#   「引擎 + 資料 + IPC」是好的,完全沒有經過
#     登錄檔 → CoCreateInstance → 把 rime_tsf.dll 載入宿主進程
#           → ActivateEx → key event sink → edit session → 組字 → 上屏
#   而使用者實際回報的「切過去、一個字都打不出來、也沒有任何 UI」,
#   撞到的正是這一段。
#
# rime_tsf_host.exe 是一個假的文字編輯器,它逼系統走完那一整條。
# 這裡用的是**安裝程式裝好並註冊好的那一份 DLL**,以及上面那支已經
# 預熱完的服務 —— 也就是使用者機器上的狀態。
if [ -n "${HOST}" ]; then
  log "6c. 經由真的 TSF:ActivateEx → 按鍵 → 組字 → 上屏"
  TRACE_LOG="${WORK}/tsf-trace.log"
  rm -f "${TRACE_LOG}"
  set +e
  "${HOST}" --langid "${ACTIVE_LANGID}" --require-activate --require-eaten \
            --keys nihao1 --expect 你好 \
            --trace "$(w "${TRACE_LOG}")" --wait-ms 4000 \
            > "${WORK}/tsf-host.log" 2>&1
  rc=$?
  set -e
  tr -d '\r' < "${WORK}/tsf-host.log" | sed 's/^/    /'
  host_out="$(tr -d '\r' < "${WORK}/tsf-host.log")"
  case "${host_out}" in
    *"系統把 rime_tsf.dll 載入了這個進程"*)
      ok "系統把裝好的 rime_tsf.dll 載入了宿主進程" ;;
    *) note_fail "裝好、註冊好,但系統**沒有**把 rime_tsf.dll 載入宿主進程" ;;
  esac
  case "${host_out}" in
    *"ActivateEx 被呼叫了"*) ok "ActivateEx 被呼叫了" ;;
    *) note_fail "ActivateEx **沒有**被呼叫 —— 使用者切過去之後什麼都不會發生" ;;
  esac
  case "${host_out}" in
    *"文件裡真的是「你好」"*)
      ok "**經由真的 TSF**把「你好」寫進了文件(這一格從本輪之前一直是紙上的)" ;;
    *) note_fail "沒有經由 TSF 打出「你好」(結束碼 ${rc})。
     照除錯記錄裡的 keysym 判斷是哪一段:
       keysym=0x0 → 鍵盤佈局問不出字
       keysym!=0  → 連不上服務(記錄裡會有一行「連線失敗」)" ;;
  esac
  if [ -f "${TRACE_LOG}" ]; then
    echo "    --- 瘦 DLL 的除錯記錄 ---"
    tr -d '\r' < "${TRACE_LOG}" | sed 's/^/      /'
  else
    note_fail "瘦 DLL 完全沒有留下除錯記錄 —— 落地記錄機制本身壞了,
     而那正是使用者回報問題時唯一的線索來源。"
  fi
else
  log "6c. (沒有給 --host,跳過「經由真的 TSF」那一段)"
fi

# ══════════════════════════════════════════════════════════════════
#  6f. 組字**進行中**按 Ctrl+空白鍵(中英切換)
# ══════════════════════════════════════════════════════════════════
#
# ⚠ common/hotkey_policy.h 的檔頭寫著這顆鍵存在的理由:「中英切換發生在
#   句子中間」。也就是說**唯一該驗的情境就是這一個** —— 而在這之前:
#
#     · verify_tsf.sh 只問「TSF 收下註冊了沒」(不呼叫產品一行程式碼);
#     · verify_ime.sh 只走具名管道問服務端(繞過整個 TSF 與文件);
#     · rime_probe 的 --ascii-toggle 在每一次送熱鍵之前先 SendClear,
#       把最該測的情境主動繞開了。
#
#   實測:把 tsf/text_service.cc 的 OnPreservedKey 在比對完 GUID 之後直接
#   `return S_OK`,CI 三個 job 全綠。這一節就是那個洞。
#
# 斷言全部看**文件內容**,不看任何內部狀態:
#   1. 熱鍵之後,打到一半的那段字要在文件裡(librime 在切到英數的當下
#      把它上屏並清掉,而那份 commit 在 acquire 當下就被消費 —— 只現身
#      一次。沒接住就是永久消失);
#   2. 接著按退格要刪得掉(組字收乾淨、engine_composing_ 沒卡在 true);
#   3. 再按一次要回得到中文。
if [ -n "${HOST}" ]; then
  log "6f. 組字進行中按 Ctrl+空白鍵:那段字不可以消失"
  TRACE_TOGGLE="${WORK}/tsf-trace-toggle.log"
  rm -f "${TRACE_TOGGLE}"
  set +e
  "${HOST}" --langid "${ACTIVE_LANGID}" --require-activate \
            --toggle-mid-compose nihao --expect-toggle-doc nihao \
            --trace "$(w "${TRACE_TOGGLE}")" --wait-ms 4000 \
            > "${WORK}/tsf-toggle.log" 2>&1
  rc_toggle=$?
  set -e
  tr -d '\r' < "${WORK}/tsf-toggle.log" | sed 's/^/    /'
  toggle_out="$(tr -d '\r' < "${WORK}/tsf-toggle.log")"
  case "${toggle_out}" in
    *"PRESERVEDKEY_DELIVERED=1"*)
      ok "Ctrl+空白鍵按下去真的走到 OnPreservedKey 了" ;;
    *) note_fail "Ctrl+空白鍵按下去**沒有到達 OnPreservedKey** —— 註冊在那裡,
     而那顆鍵不接到任何東西。使用者按下去不會有任何事發生。" ;;
  esac
  case "${toggle_out}" in
    *"組字中切中英:打到一半的「nihao」被寫進文件了"*)
      ok "組字中切中英:打到一半的字被寫進文件了(沒有消失)" ;;
    *) note_fail "組字進行中按 Ctrl+空白鍵,打到一半的字**沒有進到文件**(結束碼 ${rc_toggle})。
     librime 在切到英數的當下把手上那一段上屏並清掉,而那份 commit 在
     rs_snapshot_acquire 的當下就被消費 —— 只現身一次。瘦 DLL 的
     OnPreservedKey 把快照丟掉的話,使用者打到一半的字就永久消失。" ;;
  esac
  case "${toggle_out}" in
    *"切完之後退格刪得掉字"*)
      ok "切完之後退格刪得掉字(組字收乾淨了)" ;;
    *) note_fail "切完中英之後退格刪不掉字 —— engine_composing_ 卡在 true,
     那顆鍵掉進黑洞。使用者的說法會是「可以打字,不能刪除」。" ;;
  esac
  case "${toggle_out}" in
    *"再按一次切回中文了"*)
      ok "再按一次切回中文了(中英模式是行程層級的,沒有留在英數)" ;;
    *) note_fail "再按一次沒有切回中文。⚠ 中英是行程層級的模式:留在英數的話,
     後面每一節都會以「打不出中文」的樣子紅,而真正的原因在這裡。" ;;
  esac
  if [ "${rc_toggle}" -ne 0 ]; then
    note_fail "6f 的宿主以 ${rc_toggle} 結束 —— 見上面逐條的結論"
  fi
else
  note_fail "沒有給 --host,§6f 驗不到。Ctrl+空白鍵在組字中按下去會不會把
     使用者打到一半的字弄丟,只有這一條路問得到。"
fi

# ══════════════════════════════════════════════════════════════════
#  6d. 按鍵矩陣:每一顆會被吃掉的鍵,真的做了它宣稱的事嗎
# ══════════════════════════════════════════════════════════════════
#
# ⚠ 上面的 §6c 送的是 `nihao1` —— 七顆字母與數字,**一顆功能鍵都沒有**。
#   使用者回報的「可以打字,不能刪除」因此完全在自動化的射程之外:
#   退格鍵被宣告吃掉、引擎在沒有組字時不處理它,那顆鍵就掉進黑洞,
#   而 CI 全綠。
#
#   windows/verify_input_matrix.sh 把每一顆鍵在「組字中」與「沒有組字」
#   兩種狀態下各驗一次,而且斷言的是**文件內容**,不是「有沒有被吃掉」。
if [ -n "${HOST}" ]; then
  log "6d. 按鍵矩陣(退格、Delete、方向鍵、Esc、Enter、Tab…)"
  set +e
  "${SCRIPT_DIR}/verify_input_matrix.sh" --host "${HOST}" --langid "${ACTIVE_LANGID}" \
    --out "${WORK}/inputmatrix" 2>&1 | sed 's/^/    /'
  rc_matrix="${PIPESTATUS[0]}"
  set -e
  if [ "${rc_matrix}" -eq 0 ]; then
    ok "按鍵矩陣全過"
  else
    note_fail "按鍵矩陣有格子沒過 —— 詳見上面,以及 ${WORK}/inputmatrix/"
  fi

  # ── 6e. 建 session 有沒有超過預算(實作在上面的 assert_session_new_budget)──
  #
  # 這裡掃的是「到目前為止」:§5c 冷啟動(瘦 DLL 啟動的那一支)與 §6/§6d。
  # ⚠ §13 的升級還沒跑,所以腳本**最後**還會再掃一次 —— 那一次才涵蓋得到
  #   升級之後那幾支,而 2026-08-09 最慢的三次全在那裡。
  if [ -f "${DIAG_SVC_LOG}" ]; then
    ok "瘦 DLL 啟動的那一支服務有留下記錄:${DIAG_SVC_LOG}"
    echo "    --- 它的預熱 ---"
    (grep -a '預熱' "${DIAG_SVC_LOG}" || true) | tail -8 | sed 's/^/      /'
  else
    note_fail "找不到 ${DIAG_SVC_LOG} —— 瘦 DLL 啟動的那一支服務又變回
     「印出來的東西掉進黑洞」了,而那正是使用者機器上跑的那一支。
     下一次冷啟動再出事,我們一樣查不到原因。"
  fi
  assert_session_new_budget "6e"

  # ── 大量輸入之後,服務要**回得到可服務的狀態** ────────────────
  #
  # ⚠ 這不是「睡一下讓它過」。矩陣剛剛讓 18 個宿主進程接連連上、打字、
  #   上屏、離線,而服務的引擎是**單一一條執行緒**(service.log 的
  #   「rs_init OK,啟動引擎執行緒」),所有請求都排在那一條上。
  #   而「結束一個 session」也排在同一條 —— 那一步要收掉 librime 的 Engine、
  #   把使用者詞典寫回去。所以某個宿主剛離開的那一瞬間,下一個 SESSION_NEW
  #   會排在後面等。
  #
  #   實測到的、乾淨的重現(瘦 DLL 的除錯記錄,CI run #81):
  #       08:00:08.901 rime_probe.exe     | 連線就緒 session=14
  #       08:00:09.240 rime_ime_setup.exe | 連線失敗 階段=建立 session 原因=逾時
  #   前一支拿到 session 之後結束,它的 EndSession 還在排隊,0.34 秒後
  #   下一支就要不到 session 了。**服務完全正常,只是還沒輪到。**
  #
  #   所以這裡斷言的是**恢復**:在有限的時間內,服務必須重新接得起
  #   一個新的 session。回不來就是真的壞了,紅在這裡。
  #   (rime_probe --connect-only 會走完 連線 → 握手 → 建 session,
  #    所以它問的正是這件事,不是只有「管道開得起來」。)
  #
  # ⚠ 這一步**自己也會製造**上面那個排隊:它拿到 session 之後就結束,
  #   而它的 EndSession 會排在下一位客人前面。所以真正的修法不在這裡,
  #   在 doctor —— 它現在會試三次才判定(見 setup/doctor.cc 的 SectionPipe:
  #   把暫時的排隊報成故障,會把使用者送去重裝,而重裝沒有用)。
  #   這一步留著是因為它問的是另一個問題:「服務回不回得來」。
  log "  等服務從剛才那一輪輸入裡回過神(引擎是單執行緒的)"
  SETTLED=0
  for i in $(seq 1 60); do
    if "${PROBE}" --connect-only --attempts 1 \
         > "${WORK}/settle-probe.log" 2>&1; then
      SETTLED=1
      break
    fi
    sleep 1
  done
  if [ "${SETTLED}" -eq 1 ]; then
    ok "大量輸入之後,服務在 ${i} 秒內又接得起新的 session"
  else
    tr -d '\r' < "${WORK}/settle-probe.log" | sed 's/^/    /'
    note_fail "18 個宿主打完字之後,服務**60 秒內都建不出新的 session**。
     使用者的樣子會是:在好幾個程式裡打過字之後,下一個程式切過去
     打不出東西。引擎執行緒卡住了,或有東西沒有被收掉。"
  fi
else
  note_fail "沒有給 --host,按鍵矩陣驗不到。退格鍵那一類缺陷只有這一條路
     抓得到(單元測試驗的是政策,不是政策有沒有接到 TSF 上)。"
fi

# ══════════════════════════════════════════════════════════════════
#  6b. doctor:服務跑著的時候必須全綠,停掉之後必須指出是服務那一格
# ══════════════════════════════════════════════════════════════════
#
# 這是這一輪要交給使用者的那支工具的**正面**斷言。它跑在這裡是刻意的:
# 上面那一段剛好讓服務處於「起來了、詞庫也部署完了」的狀態 ——
# 也就是使用者機器上正常時的樣子。
log "6b. doctor(裝好、服務跑著)"
set +e
"${INSTALL_DIR}/rime_ime_setup.exe" doctor --no-engine \
  > "${WORK}/doctor-after.log" 2>&1
rc_doc=$?
set -e
doctor_after="$(tr -d '\r' < "${WORK}/doctor-after.log")"
printf '%s\n' "${doctor_after}" | sed 's/^/    /'
if [ "${rc_doc}" -eq 0 ]; then
  ok "裝好而且服務跑著的時候,doctor 以 0 結束"
else
  note_fail "裝好之後 doctor 仍然以 ${rc_doc} 結束 —— 見上面的 [FAIL] 行"
fi
# 逐格點名。只看結束碼的話,「九格都沒在看」也會以 0 結束。
for want in \
  "[PASS] rime_tsf.dll" \
  "[PASS] rime_service.exe 在跑" \
  "[PASS] 連得上、握手過了、session 建得起來" \
  "[PASS] 全機註冊完整"
do
  case "${doctor_after}" in
    *"${want}"*) ok "doctor 有這一格:${want}" ;;
    *) note_fail "doctor 沒有印出「${want}」——那一格沒有在看" ;;
  esac
done

# ── 除錯記錄檔的預設路徑(這一段 CI 別的地方都走不到)────────────
#
# ⚠ 瘦 DLL 的記錄檔路徑有兩條分支:環境變數 RIME_TSF_TRACE,與預設的
#   `%LOCALAPPDATA%\<資料夾名>\diagnostics\tsf.log`。**CI 走的一律是
#   前者** —— rime_tsf_host 自己會把環境變數設成一個它指定的路徑,
#   為的是把那一輪的記錄撈出來。於是預設那條分支從來沒有被求值過。
#
#   而預設那條分支正是改名時最危險的一段:它原本自己抄了一份資料夾名
#   (見 tsf/trace.cc 與 winshared/winutil.h)。抄的那一份漏改的話,
#   記錄會寫進一個叫舊名字的資料夾,而使用者看到的是 doctor 說
#   「記錄檔不存在 = 這台機器從來沒有載入過 rime_tsf.dll」——
#   一句**完全誤導**的診斷,指向註冊那一段。
#
#   doctor 第 8 節會把算出來的路徑印出來。這裡把那一行接住。
trace_line="$(printf '%s\n' "${doctor_after}" | sed -n 's/^.*記錄檔: //p' | head -1)"
if [ -z "${trace_line}" ]; then
  note_fail "doctor 第 8 節沒有印出記錄檔路徑 —— 那一格沒有在看
     (若它印的是「除錯記錄是關掉的」,代表這個 shell 裡 RIME_TSF_TRACE
      被設過了,那樣這條斷言就驗不到預設分支。)"
else
  # 兩件事分開問,因為它們壞掉的原因不一樣:
  #   結尾  → 資料夾名對不對(tsf/trace.cc 有沒有收斂到 winshared 那一份)
  #   開頭  → 寫在哪一個側寫檔目錄底下(不可以掉到安裝目錄或 %APPDATA%)
  want_tail="\\${RS_WIN_DATA_FOLDER}\\diagnostics\\tsf.log"
  case "${trace_line}" in
    *"${want_tail}") ok "除錯記錄檔在 ${trace_line}" ;;
    *) note_fail "doctor 算出來的記錄檔是「${trace_line}」,結尾應該是「${want_tail}」
     資料夾名在 tsf/trace.cc 那一側漏改了(它應該問 RimeUserDataFolderName())。" ;;
  esac
  # $LOCALAPPDATA 在 Git Bash 底下本來就是 Windows 形式,直接比前綴 ——
  # 繞一趟 cygpath 只會多出大小寫與結尾反斜線兩種與本題無關的失敗方式。
  if [ -n "${LOCALAPPDATA:-}" ]; then
    case "${trace_line}" in
      "${LOCALAPPDATA}\\"*) ok "而且它在 %LOCALAPPDATA% 底下" ;;
      *) note_fail "記錄檔不在 %LOCALAPPDATA%(${LOCALAPPDATA})底下:${trace_line}" ;;
    esac
  fi
fi

# ── 反向:把服務停掉,doctor 必須指出來 ──────────────────────────
#
# 沒有這一條的話,「服務在跑」那一格可能是恆真的(例如判斷寫反、
# 或它其實什麼都沒查),而那正是它最該說話的時候。
"${INSTALL_DIR}/rime_ime_setup.exe" stop-service --dir "${INSTALL_DIR_W}" \
  > "${WORK}/stop-for-doctor.log" 2>&1 || true
sleep 2
#
# ⚠ 這一次**不加** --no-engine:第 7 格(引擎層)在這裡才驗得到,而且
#   只有在這裡驗得到 —— 它要呼叫 rime_console.exe 直接驅動 librime,
#   而那需要一份**已經部署完**的使用者目錄(上面 §6 已經做完了),
#   還需要**服務不在跑**(服務持有使用者詞庫的 LevelDB,兩支同時開會打架)。
#   剛好這兩個條件在這一刻同時成立。
#
#   沒有這一條的話,doctor 最有價值的那一格(「引擎層通不通」——
#   分層診斷的第一刀)會是整支工具裡唯一沒有人驗過的部分。
set +e
"${INSTALL_DIR}/rime_ime_setup.exe" doctor --no-scan \
  > "${WORK}/doctor-nosvc.log" 2>&1
rc_doc=$?
set -e
doctor_nosvc="$(tr -d '\r' < "${WORK}/doctor-nosvc.log")"
if [ "${rc_doc}" -eq 0 ]; then
  printf '%s\n' "${doctor_nosvc}" | sed 's/^/    /'
  note_fail "服務已經停掉了,doctor 竟然還以 0 結束 —— 服務那一格是恆真的"
elif printf '%s\n' "${doctor_nosvc}" \
       | grep -q '^  \[FAIL\] rime_service.exe 沒有在跑'; then
  ok "服務停掉之後,doctor 指出「rime_service.exe 沒有在跑」"
else
  printf '%s\n' "${doctor_nosvc}" | sed 's/^/    /'
  note_fail "服務停掉之後 doctor 紅了,但沒有指出是服務那一格"
fi

# 引擎層那一格。斷言的是它真的跑了 rime_console 並拿到「你好」——
# 而不是安靜地跳過(「這個安裝裡沒有 rime_console.exe」也是 [INFO],
# 印出來長得跟通過很像)。
#
# ⚠ 接受**兩種**結果,而且只接受這兩種:
#
#   [PASS] 引擎層打得出「你好」        —— 最好的情形
#   [WARN] 引擎層起得來,但這一次沒有打完 —— rs_init 過了,只是沒等到部署回報
#
# 第二種不是我們放水,是一個真實而且無法在這裡消除的時序:
# rime_console 會等最多 600 秒的部署通知,而 librime 在**已經部署過**的
# 目錄上不一定會再發一次 —— 而這裡的使用者目錄剛好在 §6 就部署完了。
# doctor 的預算是 180 秒(它是給使用者跑的,不能等十分鐘)。
#
# 關鍵是這兩種以外的一律紅,包括:
#   · 「連 rs_init 都沒有完成」   —— 引擎層真的壞了
#   · 「起得來,但打不出你好」     —— 方案或詞庫那一層壞了
#   · 「沒有 rime_console.exe」    —— 分層診斷的第一刀切不下去
#     (那一格會安靜地變成 [INFO],而 [INFO] 印出來跟通過很像)
case "${doctor_nosvc}" in
  *"[PASS] 引擎層打得出「你好」"*)
    ok "doctor 的引擎層那一格真的跑了 rime_console 並拿到「你好」" ;;
  *"[WARN] 引擎層起得來,但這一次沒有打完"*)
    ok "doctor 的引擎層那一格跑了,rs_init 過了(沒等到部署回報,見上面的說明)" ;;
  *"沒有 rime_console.exe"*)
    note_fail "安裝包裡沒有 rime_console.exe —— 分層診斷的第一刀切不下去。
     那一格會安靜地變成 [INFO],而 [INFO] 印出來跟通過很像。" ;;
  *)
    printf '%s\n' "${doctor_nosvc}" | sed -n '/7. 引擎層/,/^$/p' | sed 's/^/    /'
    note_fail "doctor 的引擎層那一格既不是 PASS 也不是那個已知的 WARN —— 見上" ;;
esac

# 後面的「安裝目錄一個位元都沒變」需要服務是停的 —— 剛好已經停了。
# 但為了不讓兩段互相依賴,下面那一步仍然會自己再停一次(冪等)。

# ── 使用者詞典去了哪裡 ────────────────────────────────────────────
[ -d "${USER_DIR}" ] && ok "使用者資料目錄已建立: ${USER_DIR}" \
                     || note_fail "沒有建立 ${USER_DIR}"
[ -f "${USER_DIR}/default.custom.yaml" ] \
  && ok "範本 default.custom.yaml 已補進使用者目錄" \
  || note_fail "使用者目錄裡沒有 default.custom.yaml —— schema_list 會退回上游那一份,
     而它列了我們沒有詞庫的方案(部署噴錯,使用者看到的是「有些方案沒有候選」)"
# `|| true` 不是裝飾:使用者目錄不存在時 find 以 1 結束,配上 pipefail 會讓
# 整支腳本在這一行**當場死掉** —— 而那正好是我最需要看到後面那幾項診斷的時候
# (前一步剛剛才報「沒有建立使用者目錄」)。
n_user="$( (find "${USER_DIR}" -type f 2>/dev/null || true) | wc -l | tr -d ' ')"
# ⚠ 這一行原本是無條件的 ok —— n_user=0 也會印一個 ✓。
#   它長得像斷言、排在斷言中間、被算進「189 個 ✓」,而它什麼都沒問。
#   (靜態掃出來的,2026-08-12;不需要 CI 就看得出來。)
[ "${n_user}" -gt 0 ] && ok "使用者目錄裡有 ${n_user} 個檔案" \
  || note_fail "使用者目錄 ${USER_DIR} 底下一個檔案都沒有 ——
     部署沒有跑到,或它把東西寫到別的地方去了。"

# ── 安裝目錄一個位元都不該變 ──────────────────────────────────────
#
# 這一條是「使用者資料不可以寫進 Program Files」的直接證據。
# 在 runner 上我們是系統管理員,所以就算程式真的往那裡寫也不會失敗 ——
# 換句話說,權限本身擋不出這個 bug,只有這道比對擋得住。
"${INSTALL_DIR}/rime_ime_setup.exe" stop-service --dir "${INSTALL_DIR_W}" \
  > "${WORK}/stop.log" 2>&1 || true
tr -d '\r' < "${WORK}/stop.log"
sleep 2
snapshot > "${WORK}/after.txt"
if diff -u "${WORK}/before.txt" "${WORK}/after.txt" > "${WORK}/installdir.diff"; then
  ok "安裝目錄跑完之後一個檔案都沒變(使用者資料沒有寫進 Program Files)"
else
  cat "${WORK}/installdir.diff"
  note_fail "安裝目錄的內容變了 —— 有東西寫進了 Program Files。
     一般使用者沒有那個權限,所以在使用者的機器上它會安靜地失敗:
     輸入法照常運作,只是學過的詞一個都留不住。"
fi

# 服務已經停了,解除安裝不需要 trap 再做一次。
trap - EXIT

# ══════════════════════════════════════════════════════════════════
#  12. 設定的入口:它們在嗎、指對了嗎、按下去真的會開嗎
# ══════════════════════════════════════════════════════════════════
#
# ⚠ 使用者的原話:「設置你要做成圖形界面的。你這樣對新手不友好。」
#   而在那之前,他問「UI 在哪裡」,得到的回答是一行命令列指令。
#   那不是答案,那是開發者的繞法。
#
# 現在有四個入口,而這一節的工作是讓「它在」變成一件**斷言得到**的事:
#
#   1. 「開始」功能表的捷徑  ← 最不會失敗,而且新手找得到。本節驗。
#   2. 系統匣圖示            ← Windows 11 預設收進「^」溢位區。本節說明。
#   3. 語言列上的按鈕        ← §6c 的 TSF 宿主驗得到它被建出來。
#   4. rime_service --settings ← 捷徑指的就是它。本節真的執行一次。
#
# ⚠ 捷徑存在但參數打錯,症狀是**「點了沒反應」**——
#   而這個專案抓過四次那種鍵,共同點都是「畫面完全正常、自動化全過」。
#   所以這裡不只驗檔案在不在,還驗它指到哪、帶什麼參數。
log "12. 設定的入口"

# ── 12a. 「開始」功能表的兩個捷徑 ─────────────────────────────────
#
# {group} = DefaultGroupName = [Setup] 的 AppName = 產品的中文名。
# 全機安裝,所以在 ProgramData 底下的共用「開始」功能表。
# {group} = DefaultGroupName = [Setup] 的 AppName = 產品的中文名。
# 全機安裝,所以在 ProgramData 底下的共用「開始」功能表。
# ⚠ 名字從 product.env 推導,不寫死 —— 改名時這裡要跟著動,
#   而寫死的那一份會安靜地去找一個不存在的資料夾,然後每一條
#   「這個東西應該不在」的檢查都會通過。
START_MENU="$(cygpath -u "${ProgramData:-C:\\ProgramData}")/Microsoft/Windows/Start Menu/Programs/${RS_PRODUCT_NAME_ZH}"
echo "  程式集資料夾:${START_MENU}"
[ -d "${START_MENU}" ] || note_fail "找不到程式集資料夾 —— 「開始」功能表裡什麼都沒有,
     而那是新手唯一找得到的入口。"
LNK_TSV="${WORK}/shortcuts.tsv"
dump_start_menu_shortcuts "${LNK_TSV}"
if [ ! -s "${LNK_TSV}" ]; then
  note_fail "列不出任何「開始」功能表捷徑 —— 這一節什麼都沒驗到。
     (不是產品的問題,是這段測試自己壞了;但**不可以**因此當成通過。)"
else
  echo "  「開始」功能表裡與我們有關的捷徑:"
  grep -i "${RS_PRODUCT_NAME}"'\|rime_' "${LNK_TSV}" | sed 's/^/    /' || true
fi

# 名字、目標、參數三者都要對。三者的來源是 .iss 的 [Icons]。
#
# ⚠ **目標與參數直接讀 .lnk 的位元組,不問 WScript.Shell。**
#   前一版用 PowerShell 的 CreateShortcut().TargetPath,而它在 runner 上
#   對每一個捷徑都回傳**空字串**(CI run #87)—— 檔案明明在、名字也對。
#   沒有查清楚為什麼,但那不重要:一個回傳空字串的探針,會把
#   「捷徑指到錯的地方」與「探針壞了」變成同一個紅燈,而那正是要避免的。
#
#   .lnk 裡的路徑與參數是 UTF-16LE 的字串。把 NUL 去掉之後,
#   `rime_service.exe` 與 `--settings` 這種純 ASCII 的字串就直接搜得到 ——
#   不需要 COM、不需要 PowerShell、不經過任何代碼頁。
#   (代價:這是子字串比對,不是欄位解析。配上「檔案本身要在對的名字下」
#    這一條之後仍然擋得住「指到錯的執行檔」與「參數打錯」這兩種真實的失敗。)
lnk_contains() {  # $1 = .lnk 路徑, $2 = 要找的 ASCII 字串
  # ⚠ `--` 是必要的:要找的字串本身以 -- 開頭(--settings),
  #   沒有它 grep 會把它當成選項、印出用法、並**回傳非零** ——
  #   於是「參數是對的」會被判成「找不到參數」。實測踩過(CI run #89)。
  tr -d '\000' < "$1" 2>/dev/null | grep -aqF -- "$2"
}

check_lnk() {
  local name="$1" want_exe="$2" want_args="$3"
  local path="${START_MENU}/${name}.lnk"
  if [ ! -f "${path}" ]; then
    note_fail "缺少捷徑「${name}」(${path})。
     ⚠ 名字裡有**半形**冒號的話,NTFS 會把它當成交替資料流 ——
     磁碟上長出來的是一個截斷的空檔案,而「開始」功能表裡那一項
     什麼都不會做。用全形冒號。(2026-08-09 實際抓到過一次。)"
    return
  fi
  echo "    ${name}(${path})"
  if lnk_contains "${path}" "${want_exe}"; then
    ok "捷徑「${name}」指到 ${want_exe}"
  else
    echo "      .lnk 裡的可讀字串:"
    tr -d '\000' < "${path}" | tr -c '[:print:]' '\n' | grep -a '\.exe\|--' \
      | head -8 | sed 's/^/        /' || true
    note_fail "捷徑「${name}」裡找不到 ${want_exe}。
     症狀會是「點了沒反應」或跑錯程式。"
  fi
  if [ -z "${want_args}" ] || lnk_contains "${path}" "${want_args}"; then
    ok "捷徑「${name}」帶著參數 ${want_args}"
  else
    note_fail "捷徑「${name}」裡找不到參數「${want_args}」。
     參數錯掉的症狀正是「點了沒反應」—— 這個專案抓過四次那種鍵。"
  fi
}
check_lnk "${RS_PRODUCT_NAME} 設定" "rime_service.exe" "--settings"
check_lnk "${RS_PRODUCT_NAME} 診斷：輸入法為什麼不能用" "rime_ime_setup.exe" "doctor --report"

# 反向測試:上面那個比對真的會在該紅的時候紅嗎?
# 沒有這一條的話,「lnk_contains 永遠回真」與「捷徑都對」在報表上分不出來。
if lnk_contains "${START_MENU}/${RS_PRODUCT_NAME} 設定.lnk" "這個字串不可能在裡面"; then
  note_fail "一個不可能存在的字串竟然比對得到 —— 上面幾條斷言不算數"
else
  ok "反向測試通過:.lnk 裡不存在的字串比對不到"
fi

# ── 12b. 捷徑真的按得下去嗎(冷:服務沒在跑)──────────────────────
#
# ⚠ 這是「三條路一條都沒有被人走過」裡的第三條(CreateProcess)。
#   捷徑指的就是這一條 —— 服務沒在跑的時候,它必須自己起來並開出視窗。
log "  12b. 服務沒在跑時,--settings 要自己把視窗開出來"
taskkill //IM rime_service.exe //F >/dev/null 2>&1 || true
sleep 2
if settings_window_present; then
  note_fail "測試開始前設定視窗就已經開著 —— 下面那條斷言測不到東西。"
fi
# ⚠ **不可以帶 --no-ui**:main.cc 在 no_ui 底下連設定視窗都不建
#   (系統匣圖示也掛在它的訊息迴圈上)。帶了的話這條斷言會永遠紅,
#   而紅的原因與產品無關 —— 那比不驗更糟,它會讓人以為入口壞了。
"${INSTALL_DIR}/rime_service.exe" --settings --quit-after 40 \
  > "${WORK}/settings-cold.log" 2>&1 &
cold_pid=$!
opened=0
for i in $(seq 1 40); do
  if settings_window_present; then opened=1; break; fi
  sleep 1
done
# ── 12b-fg. 開出來了,而且必須在**最前面** ────────────────────────
#
# ⚠ 量在這裡而不是等一下才量:視窗剛出現的那一刻,前景狀態最乾淨。
#   冷啟動這一支是**自己建視窗**的,而它是被有前景權的呼叫端啟動的,
#   所以它本來就該在最前面 —— 拿不到就是真的壞了。
if [ "${opened}" -eq 1 ]; then
  if settings_window_foreground; then
    ok "冷啟動:設定視窗就是前景視窗"
  else
    "${INSTALL_DIR}/rime_ime_setup.exe" find-window \
      --class "${SETTINGS_CLASS}" 2>&1 | sed 's/^/     /' || true
    note_fail "冷啟動的設定視窗顯示出來了,但**不是前景視窗**。
     使用者看到的是「按了設定,視窗開在別的東西後面」—— 而那與
     「按了完全沒反應」在體感上是同一件事。"
  fi
fi
if [ "${opened}" -eq 1 ]; then
  ok "冷啟動:--settings 把設定視窗開出來了(捷徑走的就是這條)"
else
  cat "${WORK}/settings-cold.log" 2>/dev/null | tail -20
  note_fail "服務沒在跑時,rime_service.exe --settings 沒有開出設定視窗。
     「開始」功能表那個捷徑按下去會**沒反應** —— 而它是新手唯一的入口。"
fi

# ── 12s. 截圖:讓「好不好看」第一次有人看得到 ────────────────────────
#
# ⚠ **這條線最大的問題不是哪一個色值寫錯,是沒有人看過畫面。**
#   windows/service/settings_window.cc 這一路的改動全部在 Linux 上建置與
#   驗證(建置機沒有 Windows、沒有 wine),而「好不好看」在那個條件下
#   驗不了 —— 每一次都只能靠讀碼說服自己,而使用者已經白裝過一次。
#
#   這一段把設定視窗的**五頁 × 深淺兩份**各拍一張,寫進 ${WORK}
#   (= dist/verify/)。檔名是 `settings-p<頁>-<light|dark>.bmp` ——
#   人打開 artifact 不必猜哪張是哪張。
#
# ── ⚠ 這一段**只拍,不判**。判在 windows/check_ui_shots.sh ─────────
#
#   理由:這支腳本的結束碼要回答的是「安裝/解除安裝對不對」。把「畫面
#   拍到了嗎」混進同一個結束碼,紅起來就分不出是哪一件事壞了 ——
#   而分不出來的紅會被當成雜訊。所以判準是 windows.yml 裡**另一個步驟**
#   (`./windows/check_ui_shots.sh dist/verify`),在 GitHub 的畫面上
#   自己佔一列。手動跑這支腳本的人,底下那一行 log 會告訴他接著跑什麼。
#
# ⚠ 它證明得了什麼、證明不了什麼,寫清楚免得有人拿它當驗收:
#     證明得了:方角 vs 圓角、徽章有沒有被切、四個文字盒有沒有削掉一條、
#               側欄指示條在不在、卡片有沒有畫出來、按鈕高度是不是一致、
#               深淺兩份各自長什麼樣。
#     證明不了:**字體**(runner 是 Windows Server,多半沒有
#               Segoe UI Variable 與 Microsoft JhengHei UI,退化路徑會生效,
#               所以圖上的字不是使用者看到的那一套 —— docs/ui-design.md
#               §12.14.3 明著寫了這一條);**真實 accent**(runner 是預設藍);
#               **125%/150%/200% 的 DPI**(runner 是 96);**hover / 按下**
#               (runner 上沒有人在動滑鼠);以及「好不好看」。
#
# ── 深色怎麼切 ────────────────────────────────────────────────────
#
# ⚠ **不動 HKCU 的 AppsUseLightTheme。** 那個值改得動(runner 上我們是
#   自己的使用者),而且產品真的讀它(service/ui_theme.cc 的
#   SystemPrefersDark()),但它是**跟系統**那一條路:要讓已經開著的視窗
#   重畫,得靠 WM_SETTINGCHANGE + "ImmersiveColorSet" 廣播真的送到、
#   而且真的被那個視窗收到。這裡不賭那件事 —— 賭輸的樣子是**兩組圖
#   長得一模一樣**,而那在 artifact 上與「深色沒做壞」分不出來。
#
#   走的是產品自己的偏好:`appearance.appearance = dark`(§12.7.4 的
#   kDark,壓過系統)。它由服務啟動時讀進去,所以每一組圖各起一次服務。
#
# ⚠ 「兩組圖是不是真的不一樣」不是靠這裡的信心撐著 ——
#   check_ui_shots.sh 逐頁 `cmp` 深淺兩張,一樣就紅。也就是說
#   「深色沒切成功」會以紅字出現,不會靜靜只出淺色。
capture_settings_ui() {
  local mode="$1"      # light / dark
  local shots=0
  local blanks=0
  # 設定檔的格式是 `鍵 = 值`(見 common/settings.cc 的 Parse)。
  # ⚠ 直接寫死深淺,**不跟系統** —— 見上面那一段。
  mkdir -p "${USER_DIR}"
  printf 'appearance.appearance = %s\n' "${mode}" > "${USER_DIR}/${RS_WIN_SETTINGS_FILE}"
  "${INSTALL_DIR}/rime_service.exe" --settings --quit-after 90 \
    > "${WORK}/settings-shot-${mode}.log" 2>&1 &
  local pid=$!
  local up=0
  local i
  for i in $(seq 1 40); do
    if settings_window_present; then up=1; break; fi
    sleep 1
  done
  if [ "${up}" -ne 1 ]; then
    log "  (截圖 ${mode}:視窗沒開出來 —— 上面 12b 會先報這件事;"
    log "   十張圖會缺 ${mode} 那五張,check_ui_shots.sh 會因此紅)"
    # ⚠ 這條早退路徑同樣要走收尾。視窗沒開出來不代表服務沒起來 ——
    #   它可能開得慢、或是開在別的地方,而它一樣會佔著前景。
    settings_section_teardown "${pid}"
    return 0
  fi
  # 五頁:0=輸入方案 1=外觀 2=文字 3=連網 4=進階(common/ui_layout.h 的
  # SettingsPage 列舉,順序 = 側欄由上而下)。
  local page rc
  for page in 0 1 2 3 4; do
    set +e
    "${INSTALL_DIR}/rime_ime_setup.exe" capture-window \
      --class "${SETTINGS_CLASS}" --page "${page}" --fail-if-blank \
      --out "$(cygpath -w "${WORK}")\\settings-p${page}-${mode}.bmp" \
      >> "${WORK}/settings-shot-${mode}.log" 2>&1
    rc=$?
    set -e
    case "${rc}" in
      0) shots=$((shots + 1)) ;;
      # 3 = 寫出來了,但整張同一個顏色(PrintWindow 對自繪按鈕全黑的
      #     那個已知風險)。檔案留著 —— 它是證據,而判它的是
      #     check_ui_shots.sh。
      3) blanks=$((blanks + 1)) ;;
    esac
  done
  # ⚠ 這裡以前是 `kill ${pid}` + `taskkill //F` —— 而截圖這一節的服務
  #   **正好把設定視窗擺在最前面**(它就是被拍的那個)。在那個狀態下
  #   硬殺擁有者,前景欄位不會交還給任何人。見 settings_section_teardown。
  settings_section_teardown "${pid}"
  log "  截圖(${mode}):${shots} / 5 頁,其中 ${blanks} 張是整片同色"
  # ⚠ 把 capture-window 自己印的那一行(用了 PrintWindow 還是螢幕擷取、
  #   是不是整張同一個顏色)留在日誌裡 —— 「抓到黑畫面」與「視窗沒開出來」
  #   在 artifact 上長得一模一樣,不印的話沒有人分得出來。
  grep -a 'capture-window' "${WORK}/settings-shot-${mode}.log" 2>/dev/null | \
    sed 's/^/    /' || true
}

log "  12s. 設定視窗五頁 × 深淺兩份截圖(只拍;判準在 check_ui_shots.sh)"
settings_backup=""
if [ -f "${USER_DIR}/${RS_WIN_SETTINGS_FILE}" ]; then
  # ⚠ 備份**不要**放進 ${WORK} —— 那整個目錄會被 upload-artifact 傳上去。
  #   使用者的設定檔在 CI 上無所謂,但「凡是寫進 WORK 的都會公開」
  #   這條規則要一直成立,不然下一次有人往那裡放的東西就不會被想過。
  settings_backup="$(mktemp)"
  cp "${USER_DIR}/${RS_WIN_SETTINGS_FILE}" "${settings_backup}" 2>/dev/null || true
fi
# ⚠ 這一行以前是 `taskkill //IM rime_service.exe //F` —— 而它殺的正是
#   §12b 那一支,而 §12b 上面剛剛才斷言完「它的設定視窗就是前景視窗」。
#   走正規的關窗路徑,前景才會被交還。見 settings_section_teardown。
settings_section_teardown "${cold_pid}"
capture_settings_ui light
capture_settings_ui dark
# 把使用者的設定放回去 —— 這支腳本後面還有「解除安裝之後要乾淨」那幾條,
# 而我們剛剛在使用者資料夾裡寫過檔。
if [ -n "${settings_backup}" ] && [ -f "${settings_backup}" ]; then
  cp "${settings_backup}" "${USER_DIR}/${RS_WIN_SETTINGS_FILE}" 2>/dev/null || true
  rm -f "${settings_backup}"
else
  rm -f "${USER_DIR}/${RS_WIN_SETTINGS_FILE}" 2>/dev/null || true
fi
shot_count="$(ls -1 "${WORK}"/settings-p*.bmp 2>/dev/null | wc -l | tr -d ' ')"
log "  截圖:${shot_count} 張寫進 $(basename "$(dirname "${WORK}")")/$(basename "${WORK}")/"
log "  ⚠ 這一節**不判**。要判就跑:./windows/check_ui_shots.sh \"${WORK}\""

# ── 12c. 服務已經在跑時(具名事件那條路)──────────────────────────
#
# ⚠ 這是三條路裡的第二條。語言列按鈕在管道還沒連上時走的就是它,
#   而它同樣從來沒有被走過。第二支 rime_service.exe 會被單一實例擋掉,
#   那時它必須**把訊息傳過去**,不是靜靜結束。
log "  12c. 服務已經在跑時,第二支 --settings 要通知它(具名事件)"
if [ "${opened}" -eq 1 ]; then
  # 先把整支服務收掉,再從乾淨狀態重來。
  #
  # ⚠ 原本這裡是用 PowerShell 的 ShowWindow 把視窗藏起來,但那條路同樣
  #   要在命令列上傳類別名,而且「藏起來」不等於「視窗不存在」——
  #   FindWindow 找得到隱藏的視窗,所以那個做法根本擋不住恆真。
  #   收掉進程才是真的回到「服務沒在跑」。
  #
  # ⚠ 但「收掉進程」不可以是硬殺。§12b 剛剛才斷言完設定視窗**就是前景
  #   視窗**,在那個狀態下 taskkill //F 掉擁有者,前景欄位不會交還 ——
  #   見 settings_section_teardown 的說明。這裡走正規的關窗路徑。
  settings_section_teardown "${cold_pid}"
  cold_pid=""
  if settings_window_present; then
    note_fail "服務收掉之後設定視窗還在 —— 12c 的斷言會恆真,這一節沒有驗到。"
  else
    # 起一支長命的服務(不帶 --settings),然後用第二支 --settings 去通知它。
    "${INSTALL_DIR}/rime_service.exe" --quit-after 60 \
      > "${WORK}/settings-host.log" 2>&1 &
    host_pid=$!
    up=0
    for i in $(seq 1 30); do
      if "${INSTALL_DIR}/rime_ime_setup.exe" doctor --no-engine --no-scan \
           >/dev/null 2>&1; then up=1; break; fi
      sleep 1
    done
    sleep 2
    if settings_window_present; then
      note_fail "第一支服務(不帶 --settings)就把視窗開出來了 ——
     那不該發生,而且會讓下面那條斷言恆真。"
    else
      set +e
      "${INSTALL_DIR}/rime_service.exe" --settings > "${WORK}/settings-warm.log" 2>&1
      rc_warm=$?
      set -e
      warm=0
      for i in $(seq 1 15); do
        if settings_window_present; then warm=1; break; fi
        sleep 1
      done
      # ── 前景:這一格才是症狀 D 第 5 問真正要守的東西 ─────────────
      #
      # ⚠ 第二支 --settings 只送一個具名事件就結束;真正呼叫
      #   SetForegroundWindow 的是已經在跑的那一支服務,而它三條放行條件
      #   一條都不符合。沒有 service/main.cc 那一段
      #   AllowSetForegroundWindow(執行中那一支的精確 pid),系統只會讓
      #   工作列按鈕閃一下 —— 視窗顯示出來了,卻停在別的視窗後面,而
      #   settings_window_present() 對那個狀態回真。
      # ⚠ 這一條的反向確認(把 main.cc 那一段拿掉,它必須紅)只有
      #   Windows CI 做得到:建置機是 Linux,連結不起來。
      if [ "${warm}" -eq 1 ]; then
        if settings_window_foreground; then
          ok "前景權轉讓成立:第二支 --settings 之後,設定視窗在最前面"
        else
          "${INSTALL_DIR}/rime_ime_setup.exe" find-window \
            --class "${SETTINGS_CLASS}" 2>&1 | sed 's/^/     /' || true
          note_fail "第二支 --settings 把視窗叫出來了,但它**不是前景視窗**。
     少了 service/main.cc 的 AllowSetForegroundWindow(執行中那一支的 pid),
     系統只會讓工作列按鈕閃一下 —— 使用者以為按了沒反應。"
        fi
      fi
      if [ "${warm}" -eq 1 ] && [ "${rc_warm}" -eq 0 ]; then
        ok "具名事件那條路成立:第二支 --settings 把視窗叫出來了(rc=${rc_warm})"
      else
        cat "${WORK}/settings-warm.log" 2>/dev/null
        note_fail "服務已經在跑時,第二支 rime_service.exe --settings 沒有把視窗叫出來
     (rc=${rc_warm})。語言列按鈕在管道還沒連上時走的就是這條 ——
     症狀是「按了設定,什麼都沒發生」。"
      fi
    fi
    kill "${host_pid}" 2>/dev/null || true
  fi
else
  note_fail "12b 沒過,所以 12c 這一節**沒有被驗證**。"
fi
# ── §12 整節的收尾 ─────────────────────────────────────────────────
#
# ⚠ 這裡是整支腳本裡**最危險的一個收尾**,而它以前只有兩行硬殺:
#     kill "${cold_pid}"; taskkill //IM rime_service.exe //F
#   §12c 剛剛才斷言完「設定視窗現在就是前景視窗」(而且它綠了 ——
#   AllowSetForegroundWindow 做的正是它該做的事)。也就是說,硬殺的
#   那一刻,被殺掉的正是**前景視窗的擁有者**。這台 runner 沒有 Explorer
#   可以接手前景、也沒有任何輸入事件,於是 SetForegroundWindow 唯一還
#   成立的那條放行條件(「目前沒有前景視窗」)從此消失 ——
#   §13 的兩支 TSF 宿主一支都搶不到,而 §13c 把它講成「版本協商壞了」。
#
#   run 31896143629 的時間軸乾淨得沒有第二種解釋:§12 之前 24 支宿主
#   全部搶到了,§12 之後 2 支全部沒搶到,而且前景是同一個 handle。
settings_section_teardown "${cold_pid}"

# ── 12d. 系統匣圖示:Windows 11 的溢位區 ──────────────────────────
#
# ⚠ 查過了,結論是**沒有辦法**,所以這裡只留下記錄,不留下一個假的斷言。
#
#   · Shell_NotifyIcon / NOTIFYICONDATA **沒有**任何「請把我釘在外面」的旗標。
#     微軟把這件事寫成政策而不是疏漏:「Only the user can promote an icon
#     from the overflow to the notification area」。
#   · HKCU\Control Panel\NotifyIconSettings\<id> 底下確實有 IsPromoted,
#     但那個 <id> 是不公開的雜湊(同一支執行檔在不同機器上不一樣),
#     而且那個鍵**要等圖示第一次出現過才會存在** —— 安裝程式寫不了它。
#   · unattend 的 PromotedIcon1..4 是 OEM 封裝映像用的,不是應用程式叫得動的。
#
#   所以產品這一側能做的只有「在說明裡講清楚要點那個 ^」,而那句話已經
#   寫進安裝完成頁(見 .iss 的 FinishedLabel)。這裡斷言的是**那句話還在**——
#   文案被改掉而沒有人發現,使用者就又找不到圖示了。
log "  12d. 系統匣的溢位區(沒有 API 可以請求晉升,只能在文案裡講)"
if grep -q '\^' "${SCRIPT_DIR}/${RS_WIN_ISS_REL}" \
   && grep -q '系統匣' "${SCRIPT_DIR}/${RS_WIN_ISS_REL}"; then
  ok "安裝完成頁有交代系統匣圖示與「^」溢位區"
else
  note_fail "安裝完成頁沒有交代系統匣圖示會被收進「^」——
     Windows 11 預設把新圖示藏起來,而我們沒有 API 可以要求晉升,
     所以那句話是使用者唯一會知道的管道。"
fi

# ══════════════════════════════════════════════════════════════════
#  7. 反向測試:故意破壞註冊,check 必須紅
# ══════════════════════════════════════════════════════════════════
#
# 「安裝之後 check 通過」要成立,得先證明 check 會在註冊被破壞時失敗。
# 這裡刪掉 InprocServer32 —— 那是最像「其實沒事」的一種壞法:
# 輸入法還在清單上,CLSID 也還在,只是 COM 載入不了 DLL。
log "7. 反向測試:刪掉 InprocServer32 之後 check 必須紅"
reg delete "${HKLM_INPROC}" //f //reg:64 >/dev/null 2>&1 \
  || die "刪不掉 ${HKLM_INPROC}(權限不足?)"
if "${INSTALL_DIR}/rime_ime_setup.exe" check > "${WORK}/check-broken.log" 2>&1; then
  cat "${WORK}/check-broken.log"
  note_fail "InprocServer32 被刪掉了,check 竟然還通過 —— 這道檢查沒有在檢查"
else
  ok "註冊被破壞時 check 以非零結束"
fi
log "  修回去"
"${INSTALL_DIR}/rime_ime_setup.exe" register > "${WORK}/rereg.log" 2>&1 \
  || { cat "${WORK}/rereg.log"; die "重新註冊失敗"; }
"${INSTALL_DIR}/rime_ime_setup.exe" check > /dev/null 2>&1 \
  || die "重新註冊之後 check 仍然紅"
ok "重新註冊之後 check 又通過了"

# ══════════════════════════════════════════════════════════════════
#  13. 升級不得要求重新啟動
# ══════════════════════════════════════════════════════════════════
#
# 決策紀錄:docs/decisions/no-restart.md。使用者的原話:
#   「不要每次安裝都重啟啊。這個是大工程,任何端都不能這樣。」
#
# 而理由不是體驗:**一個每次更新都要重開機的輸入法,使用者就不會更新它**,
# 然後安全性修正到不了他手上 —— 而這個專案的定位是經得起審計,
# 那句話建立在「使用者手上跑的是我們現在的程式碼」之上。
#
# ── 這一節驗三件事,而第一件是使用者機器上**現在的真實狀態** ──────
#
#   1. 舊的 DLL 映像 + 新的服務進程,還打不打得出字。
#      升級之後,已經開著的程式(檔案總管、瀏覽器)手上仍然是舊的 DLL,
#      而服務已經換成新的。談不攏的症狀是「有些程式能打字、有些不能」,
#      而使用者完全無法理解為什麼 —— 那比全部壞掉更難查。
#   2. 新開的進程拿到的是**新檔**(而不是等下次開機才換)。
#   3. 整個過程**沒有任何東西被排進開機佇列**、安裝程式沒有要求重新啟動。
#
# ── ⚠ 為什麼這一關不會是「永遠綠、永遠沒在測」 ────────────────────
#
# 因為它有反向測試,而且反向測試在最後面自成一節(§14):
# 用 /LEGACYINPLACE 跑一次**跳過改名挪開**的安裝,並要求第 3 項的斷言
# 真的紅。沒有那一節的話,這一關有可能只是因為 runner 上沒有人握著 DLL
# 而通過 —— 這個專案已經抓過四次那種東西。
#
# ⚠ 誠實說一件事:這裡的「新版」是**同一次建置**再裝一次。它驗到的是
#   換檔的機制與升級之後的相容性,**不是**「版本號真的變了」。
#   兩個版本的位元組不同時會不會有別的問題(例如線路協議改版),
#   這一節驗不到 —— 那要靠 protocol.h 的版本協商與 test_proto_compat.cc。
if [ -n "${HOST}" ]; then
log "13. 升級不得要求重新啟動(舊 DLL 還被握著的時候裝新版)"

# ── ⚠ 誰在前景、誰在背景,是這一節唯一難寫對的地方 ────────────────
#
#   TSF 只把按鍵交給**擁有前景**的那一條執行緒(README 的「學到的兩件事」
#   第 1 點:沒有前景時 KeyDown 回 S_OK、pfEaten=FALSE,一顆鍵都不會到達
#   文字服務,而且不報錯)。
#
#   第一版把兩階段宿主丟到背景跑,好讓腳本騰出手去裝新版 —— 結果它搶不到
#   前景(實測:`前景視窗 = ...0600A0(我們的是 ...070052)`、
#   `IsThreadFocus = 0`),六顆按鍵一顆都沒到,於是第二階段量到的
#   「連不回服務」**完全是假的** —— 它根本沒機會連。
#
#   所以現在反過來:**宿主在前景**(與 §6c 同一種跑法),
#   而「等它就緒 → 裝新版 → 起新服務 → 放行」那一串在背景的子 shell 裡。
p13_stop_service() {
  "${INSTALL_DIR}/rime_ime_setup.exe" stop-service --dir "${INSTALL_DIR_W}" \
    > "${WORK}/p13-stop.log" 2>&1 || true
  for _ in $(seq 1 30); do
    [ "$(count_service)" -eq 0 ] && break
    sleep 1
  done
}

# 起一支服務並等它**真的**就緒(ready 檔是服務自己在管道接得起連線、
# 引擎也預熱過之後才寫的)。回傳 pid;失敗回空字串。
#
# ⚠ 為什麼不讓瘦 DLL 自己去啟動它(那條路 §5c 已經驗過了):
#   這一節要問的是「舊的 DLL 映像跟新的服務談不談得攏」,
#   而不是「服務要多久才部署完」。把部署延遲混進來的話,
#   一次逾時會被記成「舊 DLL 連不回去」—— 而那兩件事要修的地方完全不同。
p13_start_service() {   # $1 = ready 檔  $2 = 記錄檔
  rm -f "$1"
  "${INSTALL_DIR}/rime_service.exe" --no-ui --wait-deploy 1200 \
    --ready-file "$(w "$1")" --quit-after 1800 > "$2" 2>&1 &
  local pid=$!
  local i
  for i in $(seq 1 1200); do
    [ -f "$1" ] && { echo "${pid}"; return 0; }
    kill -0 "${pid}" 2>/dev/null || { echo ""; return 1; }
    sleep 1
  done
  echo ""
  return 1
}

# 起點必須乾淨,不然後面「多了一顆 .old-」的斷言不算數。
"${INSTALL_DIR}/rime_ime_setup.exe" sweep-stale-dlls --dir "${INSTALL_DIR_W}" \
  > "${WORK}/p13-sweep0.log" 2>&1 || true
tr -d '\r' < "${WORK}/p13-sweep0.log" | sed 's/^/    /'
if [ "$(stale_count)" -ne 0 ]; then
  stale_list | sed 's/^/    /'
  note_fail "13 開始前安裝目錄裡就有 rime_tsf.dll.old-*,而且掃不掉 ——
     後面「升級之後正好多一顆」的斷言會失去意義。"
fi
[ -f "${DLL_U}" ] || die "13 開始前找不到 ${DLL_U}"
ID_BEFORE="$(file_id "${DLL_W}")"
if [ -n "${ID_BEFORE}" ]; then
  log "  升級前 rime_tsf.dll 的檔案 id = ${ID_BEFORE}"
else
  printf '\033[1;33m  ⚠ fsutil 問不到檔案 id —— 「換成另一顆檔案了」那一格改用\033[0m\n' >&2
  printf '\033[1;33m    「.old- 出現了」與「沒有排進開機佇列」兩條間接證據。\033[0m\n' >&2
fi

# ── 前置:自己起一支**升級前**的服務,並等它就緒 ────────────────
log "  起一支升級前的服務(等它真的就緒)"
p13_stop_service
# ⚠ `|| true` 不是裝飾:服務起不來時這個函式以非零結束,而它的結果拿去做
#   變數指派 —— 配上 set -e,整支腳本會在這一行當場死掉,而訊息完全不會
#   提到服務。空字串就是「起不來」,下面那個 if 會好好地說出來。
SVC_OLD_PID="$(p13_start_service "${WORK}/p13-ready-svc-old" "${WORK}/p13-svc-old.log" || true)"
if [ -z "${SVC_OLD_PID}" ]; then
  tr -d '\r' < "${WORK}/p13-svc-old.log" | tail -20 | sed 's/^/    /'
  note_fail "升級前的服務起不來 —— 這一節後面每一條都驗不到"
else
  ok "升級前的服務已就緒(pid ${SVC_OLD_PID})"

  RDY="${WORK}/p13-ready"
  GO="${WORK}/p13-go"
  rm -f "${RDY}" "${GO}" "${WORK}/p13-upgrade.rc"

  # ── 背景:等宿主就緒 → 裝新版 → 起新服務並等它就緒 → 放行 ──────
  (
    for _ in $(seq 1 600); do
      [ -f "${RDY}" ] && break
      sleep 1
    done
    if [ ! -f "${RDY}" ]; then
      echo "no-ready" > "${WORK}/p13-upgrade.rc"
    else
      set +e
      "${SETUP}" //VERYSILENT //SUPPRESSMSGBOXES //NORESTART \
                 "//LOG=$(w "${WORK}/p13-upgrade.log")"
      echo "$?" > "${WORK}/p13-upgrade.rc"
      set -e
      # 安裝程式在複製檔案之前把舊服務停掉了。起一支**新的**,
      # 並且等它真的就緒 —— 放行之後宿主才不會撞上「還在部署」。
      #
      # ⚠ `|| true`:起不來也一定要往下走到那個 `: > "${GO}"`。
      #   少了它,子 shell 會被 set -e 帶走,而前景那支宿主會傻等到逾時 ——
      #   然後回報「連不回服務」,把一個「服務根本沒起來」講成別的故事。
      p13_start_service "${WORK}/p13-ready-svc-new" "${WORK}/p13-svc-new.log" \
        > "${WORK}/p13-svc-new.pid" 2>/dev/null || true
    fi
    : > "${GO}"
  ) &
  UPGRADER_PID=$!

  # ── 13a/13c:兩階段宿主,**在前景跑** ────────────────────────
  log "  13a. 前景開一個真的 TSF 宿主:載入舊版、打出「你好」,然後等升級"
  # ── ⚠ 起宿主之前先問一次桌面 ──────────────────────────────────
  #
  #   §13 的每一格都要經過 TSF,而 TSF 只把按鍵交給**有執行緒焦點**的
  #   那一份文字服務。焦點的前提是前景 —— 所以「現在誰佔著前景」是這
  #   一整節的前置條件,不是背景資訊。
  #
  #   run 31896143629 就是在這個前提不成立的狀態下跑完了整節:
  #   兩支宿主一顆按鍵都沒送出去,而報表印的是「舊 DLL 連不回新服務」。
  #   把這一行記在**宿主啟動之前**,「環境被前一節弄髒了」與「產品壞了」
  #   在報表上就分得出來 —— 現在這兩件事是同一句紅字,而它指的三個
  #   方向全是好的東西。
  P13_DESKTOP_DIRTY=0
  if foreground_not_ours; then
    ok "13a 之前的桌面狀態:前景不是我們的視窗(這一節的前提成立)"
  else
    P13_DESKTOP_DIRTY=1
    note_fail "13a 開始前,**前景仍然是我們自己的設定視窗** ——
     §12 的收尾沒有把桌面還原(見上面 §12 那幾行 [桌面])。
     TSF 只把按鍵交給有執行緒焦點的那一份文字服務,而焦點要先有前景。
     ⚠ 底下 §13 每一格量到的「打不出字」都**不能拿來判斷產品好壞**。"
  fi
  foreground_note "13a 起宿主之前"
  set +e
  "${HOST}" --langid "${ACTIVE_LANGID}" --require-activate --require-eaten \
            --keys nihao1 --expect 你好 \
            --phase2-ready-file "$(w "${RDY}")" \
            --phase2-go-file "$(w "${GO}")" \
            --phase2-timeout-ms 900000 --phase2-relink-ms 300000 \
            --trace "$(w "${WORK}/p13-oldhost-trace.log")" --wait-ms 5000 \
            > "${WORK}/p13-oldhost.log" 2>&1
  OLD_RC=$?
  set -e
  wait "${UPGRADER_PID}" 2>/dev/null || true
  tr -d '\r' < "${WORK}/p13-oldhost.log" | sed 's/^/    /'

  UPRC="$(cat "${WORK}/p13-upgrade.rc" 2>/dev/null || echo missing)"
  UPLOG="$(tr -d '\r' < "${WORK}/p13-upgrade.log" 2>/dev/null || true)"

  # ── 13b. 升級本身:機制有沒有跑、有沒有留下重啟的理由 ────────
  log "  13b. 升級的結果(檔案、檔案 id、開機佇列)"
  case "${UPRC}" in
    0) ok "升級以 0 結束(而且是在有進程握著舊 DLL 的狀態下)" ;;
    no-ready) note_fail "宿主沒有走到「打完字、等升級」那一步,升級根本沒跑" ;;
    missing)  note_fail "背景那一段沒有留下結束碼 —— 升級沒跑到" ;;
    *)        printf '%s' "${UPLOG}" | tail -40 | sed 's/^/    /'
              note_fail "升級以 ${UPRC} 結束" ;;
  esac

  # 機制真的跑了嗎。**不要只看結果** —— 結果可能因為別的原因剛好是對的
  # (例如 runner 上根本沒有人握著檔案),而那時這一關就沒在測東西。
  if printf '%s' "${UPLOG}" | grep -q '已改名挪開'; then
    ok "安裝程式**把舊的 DLL 改名挪開**了(而不是原地覆蓋)"
    printf '%s' "${UPLOG}" | grep '已改名挪開' | head -2 | sed 's/^/    /'
  else
    note_fail "安裝記錄裡沒有「已改名挪開」—— 不重啟就生效的機制沒有跑到。
     可能是 PrepareToInstall 沒被呼叫,或改名失敗退回了 restartreplace。"
  fi
  if printf '%s' "${UPLOG}" | grep -q '改名挪開.*失敗'; then
    printf '%s' "${UPLOG}" | grep '改名挪開' | sed 's/^/    /'
    note_fail "改名挪開失敗,退回了 restartreplace —— 這一次升級會要求重新啟動。"
  fi

  # ── 檔案層面:新開的進程會拿到哪一顆 ──────────────────────────
  [ -f "${DLL_U}" ] || note_fail "升級之後 ${DLL_U} 不見了"
  n_stale="$(stale_count)"
  if [ "${n_stale}" -eq 1 ]; then
    ok "舊檔被挪到 $(stale_list | sed 's|.*/||')(升級當下仍被宿主握著,無害)"
  else
    stale_list | sed 's/^/    /'
    note_fail "預期正好 1 個 rime_tsf.dll.old-*,實際 ${n_stale} 個"
  fi

  ID_AFTER="$(file_id "${DLL_W}")"
  if [ -n "${ID_BEFORE}" ] && [ -n "${ID_AFTER}" ]; then
    if [ "${ID_AFTER}" != "${ID_BEFORE}" ]; then
      ok "**rime_tsf.dll 現在是另一顆檔案了**(id ${ID_BEFORE} -> ${ID_AFTER})
     —— 也就是說,現在新開的進程載入的是新裝的那一份,不必等重新開機。"
    else
      note_fail "升級之後 rime_tsf.dll 的檔案 id 沒變(還是 ${ID_BEFORE})。
     那代表磁碟上躺的仍然是舊檔 —— **新開的程式也會拿到舊版**,
     一直到使用者重新開機為止。這正是這一輪要修掉的東西。"
    fi
    stale_one="$(stale_list | head -1)"
    if [ -n "${stale_one}" ]; then
      id_stale="$(file_id "$(w "${stale_one}")")"
      [ "${id_stale}" = "${ID_BEFORE}" ] \
        && ok "被挪開的那一顆正是舊 host 手上那一顆(id ${id_stale})" \
        || note_fail "被挪開的檔案 id=${id_stale},而升級前是 ${ID_BEFORE} —— 對不上"
    fi
  fi

  # ── ⚠ 這一條是整節的核心斷言 ────────────────────────────────
  queued13="$(pending_in_install_dir)"
  if [ -n "${queued13}" ]; then
    echo "  --- PendingFileRenameOperations(落在安裝目錄底下的)---"
    printf '%s\n' "${queued13}" | sed 's/^/    /'
    note_fail "升級把安裝目錄底下的東西**排進了開機佇列**(上面那幾筆)。
     那代表磁碟上那幾個還是舊的、新開的程式也拿到舊的,而且安裝程式
     會在最後一頁問使用者要不要重新啟動 —— 這一整輪要修的就是這件事。
     ⚠ 這裡問的是**整個安裝目錄**,不是只有 rime_tsf.dll:
       rime_service.exe 或 data\\shared 底下任何一份被排進去,
       症狀一樣是「裝完要重開機,而且沒重開之前用的是舊的」。"
  else
    ok "**升級沒有把任何東西排進開機佇列**(整個 ${INSTALL_DIR_W} 都問過了,
     不是只問 rime_tsf.dll;我們自己排的 .old- 清理不算)"
  fi

  # ── 13c. 舊的 DLL 映像 + 新的服務 ────────────────────────────
  #
  # 使用者機器上**現在**的狀態(他實測回報「我沒重啟也能用」的那一台)。
  # 走不通的話,他看到的是「有些程式能打字、有些不能」。
  log "  13c. 舊的 DLL 映像 + 新的服務"
  if [ "${OLD_RC}" -eq 0 ]; then
    ok "**舊的 DLL 映像在升級之後照樣打得出「你好」**(重新連上了新的服務)
     —— 這是使用者升級之後那些沒關掉的程式(檔案總管、瀏覽器)的處境。"
  else
    # ── 三種紅字,不是一種 ────────────────────────────────────
    #
    # ⚠ 這裡以前只有一句「舊的 DLL 在升級之後不能用了」,而它在
    #   run 31896143629 上是**錯的**:那一趟舊 DLL 全程連著新服務
    #   (p13-svc-new.log:連線 #1 存活=300829ms 握手=1),
    #   只是宿主拿不到前景,一顆按鍵都沒有離開宿主進程。
    #   一句話蓋住三種完全不同的原因,而它指的方向會把人送去改
    #   管道名 / 版本協商 / rime_shell ABI —— 那三個都是好的。
    #
    # 現在照**證據**分派,順序是「最不需要前景的先問」:
    #   1. 桌面在這一節開始前就髒了(§13a 已經紅過)→ 環境,不是產品
    #   2. 宿主自己說它拿不到焦點(PHASE2_PROBE_SKIPPED=1)→ 同上
    #   3. 管道連不回去(PHASE2_PIPE_REACHABLE=0)→ **這才是產品**
    #   4. 以上都不是 → 真的打不出字
    P13_SKIPPED="$(tr -d '\r' < "${WORK}/p13-oldhost.log" \
                   | sed -n 's/^ *PHASE2_PROBE_SKIPPED=//p' | head -1)"
    P13_PIPE="$(tr -d '\r' < "${WORK}/p13-oldhost.log" \
                | sed -n 's/^ *PHASE2_PIPE_REACHABLE=//p' | head -1)"
    if [ "${P13_PIPE}" = "0" ]; then
      note_fail "**管道那一側真的連不回去**(宿主印的 PHASE2_PIPE_REACHABLE=0)。
     這一格不經過 TSF,所以它與前景無關 —— 它是真的。
     去查:管道名(winshared/winutil.cc 的 RimePipeName)、
     握手的版本協商(common/protocol.h、tsf/ipc_client.cc 的 EnsureReady)、
     rime_shell 的 ABI(ipc_client.cc 的 Handshake)。"
    elif [ "${P13_DESKTOP_DIRTY}" = "1" ] || [ "${P13_SKIPPED}" = "1" ]; then
      note_fail "**這一格沒有量到相容性** —— 宿主拿不到 TSF 執行緒焦點
     (PHASE2_PROBE_SKIPPED=${P13_SKIPPED:-?}),按鍵一顆都沒有離開宿主進程。
     而管道是連得上的(PHASE2_PIPE_REACHABLE=${P13_PIPE:-?}),
     也就是說**沒有任何證據說產品壞了,也沒有任何證據說它是好的**。
     ⚠ 不要從這一格去查管道名、版本協商或 rime_shell ABI。
     要查的是**誰佔著前景**:宿主印了它的 pid / exe / 類別 / IsWindow,
     上面 §13a 的 [桌面] 那幾行也記了同一件事。
     ⚠ 這一節的前提壞了,所以它必須是紅的 —— 一個驗不到東西的段落
       綠起來,比它紅起來危險得多。"
    else
      note_fail "舊的 DLL 在升級之後不能用了(宿主結束碼 ${OLD_RC})。
     宿主拿得到焦點、管道也連得上,所以這一格是**真的量到了壞的**。
     症狀會是「有些程式能打字、有些不能」,而使用者無法理解為什麼。"
    fi
  fi
  (tr -d '\r' < "${WORK}/p13-oldhost.log" | grep -a 'PHASE2_' || true) \
    | sed 's/^/    /'

  # ── 13d. 新開的進程必須拿到新版 ──────────────────────────────
  log "  13d. 升級之後新開一個宿主(它應該載入新裝的那一份)"
  # ⚠ 先等服務接得起新的 session。引擎是單執行緒的,上一個宿主剛離開時
  #   它的 EndSession 還排在佇列上 —— 那時 SESSION_NEW 會逾時,
  #   而那與「新的 DLL 有問題」在結果上長得一模一樣(見 §6d 的同款說明)。
  SETTLED13=0
  for i in $(seq 1 120); do
    if "${PROBE}" --connect-only --attempts 1 \
         > "${WORK}/p13-settle.log" 2>&1; then
      SETTLED13=1
      break
    fi
    sleep 1
  done
  [ "${SETTLED13}" -eq 1 ] \
    && ok "升級後的服務接得起新的 session(等了 ${i} 秒)" \
    || note_fail "升級後的服務 120 秒內建不出新的 session —— 下面那一格會跟著紅,
     但真正的原因在這裡。"
  set +e
  "${HOST}" --langid "${ACTIVE_LANGID}" --require-activate --require-eaten \
            --keys nihao1 --expect 你好 \
            --trace "$(w "${WORK}/p13-newhost-trace.log")" --wait-ms 5000 \
            > "${WORK}/p13-newhost.log" 2>&1
  rc=$?
  set -e
  tr -d '\r' < "${WORK}/p13-newhost.log" | sed 's/^/    /'
  [ "${rc}" -eq 0 ] \
    && ok "升級之後新開的宿主打得出「你好」" \
    || note_fail "升級之後新開的宿主以 ${rc} 結束"
  LOADED_NEW="$(tr -d '\r' < "${WORK}/p13-newhost.log" \
                | sed -n 's/^ *LOADED_TSF_DLL=//p' | head -1)"
  # ⚠ 大小寫不敏感地比。Windows 的路徑不分大小寫,而 %ProgramW6432% 與
  #   GetModuleFileName 回來的字串來自不同的地方 —— 拿它們逐字比對的話,
  #   一次大小寫差異就會變成一個看起來很嚴重、其實什麼事都沒有的紅字。
  if [ "$(printf '%s' "${LOADED_NEW}" | tr 'A-Z' 'a-z')" \
     = "$(printf '%s' "${DLL_W}" | tr 'A-Z' 'a-z')" ]; then
    ok "它載入的是 ${LOADED_NEW} —— 那個路徑上現在躺的是新檔(見上面的檔案 id)"
  else
    note_fail "新宿主載入的是「${LOADED_NEW}」,預期「${DLL_W}」"
  fi

  # ── 13e. 舊檔的清理策略真的會清 ──────────────────────────────
  #
  # 兩個宿主都結束了,沒有人再握著那顆舊 DLL —— 這正是清理該生效的時機。
  # (真實世界裡對應的是「使用者下次登入,服務啟動時掃一遍」。)
  #
  # ⚠ 這一條同時擋兩種相反的錯:
  #   · 清不掉 → 舊檔會一次升級留一顆,幾年之後累積成幾十顆
  #   · 清了不該清的 → 那就是把正在用的 DLL 刪掉
  #   所以掃完之後**兩件事都要驗**:.old- 沒了,而 rime_tsf.dll 還在。
  log "  13e. 沒有人握著之後,舊檔必須清得掉"
  "${INSTALL_DIR}/rime_ime_setup.exe" sweep-stale-dlls --dir "${INSTALL_DIR_W}" \
    > "${WORK}/p13-sweep.log" 2>&1 || true
  tr -d '\r' < "${WORK}/p13-sweep.log" | sed 's/^/    /'
  if [ "$(stale_count)" -eq 0 ]; then
    ok "舊檔已經清乾淨(沒有人握著的時候它真的刪得掉)"
  else
    stale_list | sed 's/^/    /'
    note_fail "沒有任何進程握著舊 DLL 了,sweep-stale-dlls 卻還是沒清掉。
     那代表舊檔會一次升級留一顆,幾年之後累積成幾十顆。"
  fi
  [ -f "${DLL_U}" ] \
    && ok "而且**還在用的那一顆沒有被掃掉**" \
    || note_fail "掃描把 ${DLL_U} 也刪了 —— 那是正在用的那一顆!"

  # 場地還原:§8 要解除安裝,身上不該還掛著我們起的服務。
  p13_stop_service
fi
fi  # -n "${HOST}"

# ══════════════════════════════════════════════════════════════════
#  8. 靜默解除安裝
# ══════════════════════════════════════════════════════════════════
#
# 走的是登錄檔裡那一筆 UninstallString —— 也就是使用者從「新增或移除程式」
# 按下去會跑的同一支程式。另外寫一條路徑的話,驗到的是那一條。
log "8. 靜默解除安裝"
[ -n "${UNINST}" ] || die "沒有 UninstallString,無法解除安裝"
UNINST_U="$(cygpath -u "${UNINST}")"
[ -f "${UNINST_U}" ] || die "UninstallString 指到不存在的檔案: ${UNINST}"
set +e
"${UNINST_U}" //VERYSILENT //SUPPRESSMSGBOXES //NORESTART
rc=$?
set -e
# Inno 的解除安裝程式會把自己複製到暫存目錄再跑,原本那支立刻返回;
# 所以不能只看結束碼,要等登錄檔那一筆真的消失。
for i in $(seq 1 120); do
  reg_key_exists "${ARP}" || break
  sleep 1
done
[ "${rc}" -eq 0 ] || note_fail "解除安裝程式以 ${rc} 結束"

reg_key_exists "${ARP}" \
  && note_fail "解除安裝之後「新增或移除程式」裡那一筆還在" \
  || ok "新增或移除程式:那一筆已消失"
reg_key_exists "${HKLM_CLSID}" \
  && note_fail "解除安裝之後 ${HKLM_CLSID} 還在 —— 登錄檔留下殘骸" \
  || ok "CLSID 已清乾淨"
reg_key_exists "${HKLM_CTF}" \
  && note_fail "解除安裝之後 ${HKLM_CTF} 還在 —— TSF 的註冊留下殘骸" \
  || ok "CTF\\TIP 已清乾淨"

# 每一個語言都要消失,不是只消失一個。
while IFS= read -r line; do
  [ -z "${line}" ] && continue
  lang="${line%%=*}"; guid="${line#*=}"
  key="${HKLM_CTF}\\LanguageProfile\\$(profile_langkey "${lang}")\\${guid}"
  if reg_key_exists "${key}"; then
    note_fail "${key} 還在 —— ${lang} 的語言設定檔沒有被清掉"
  else
    ok "${lang} 的語言設定檔已清除"
  fi
done <<< "${PROFILES}"

# ── ⚠ 沒有東西被鎖住時,不可以留下「開機時刪除」的佇列 ────────────
#
# 這一條回答的是「重啟提示會不會在不必要的時候跳出來」。
#
# 走到這裡時,沒有任何進程握著 rime_tsf.dll(§6c 那支宿主早就結束了),
# 所以 Inno 應該是**直接刪掉**它,而不是排進開機佇列 —— 也就不會問使用者
# 要不要重新啟動。這一條把「應該」變成斷言。
#
# 它紅掉的意思是:每一個使用者、即使剛開機什麼都沒開,解除安裝之後
# 都會被問一次要不要重開機。一個沒有必要的重啟提示會讓人覺得這軟體很髒。
if pending_has_our_dll; then
  echo "  --- PendingFileRenameOperations ---"
  pending_renames | sed 's/^/    /'
  note_fail "沒有任何程式握著 rime_tsf.dll,解除安裝卻仍然把它排進「開機時刪除」。
     那代表**每一次**解除安裝都會跳出重新啟動的提示,即使完全沒有必要。"
else
  ok "沒有東西被鎖住時,解除安裝不留下「開機時刪除」的佇列(不會問要不要重啟)"
fi

# ── 反向測試:解除安裝之後 check 必須紅 ──────────────────────────
if "${TOOL}" check > "${WORK}/check-uninstalled.log" 2>&1; then
  cat "${WORK}/check-uninstalled.log"
  note_fail "解除安裝之後 check 竟然還通過"
else
  ok "解除安裝之後 check 以非零結束"
fi
if "${TOOL}" doctor --no-engine --no-scan > "${WORK}/doctor-uninstalled.log" 2>&1; then
  tr -d '\r' < "${WORK}/doctor-uninstalled.log"
  note_fail "解除安裝之後 doctor 竟然還以 0 結束"
else
  ok "解除安裝之後 doctor 以非零結束"
fi

# ── ⚠ 使用者詞典必須還在 ─────────────────────────────────────────
#
# 那是使用者的資料:學過的詞、自訂短語、設定。解除安裝一律留下。
# 這一條斷言的價值在於它擋的是**不可逆**的損失 —— 其他每一項失敗都可以
# 重裝一次補救,只有這一項不行。
log "9. 使用者詞典必須被保留"
if [ -d "${USER_DIR}" ]; then
  n_after="$(find "${USER_DIR}" -type f 2>/dev/null | wc -l | tr -d ' ')"
  if [ "${n_after}" -gt 0 ]; then
    ok "${USER_DIR} 還在,${n_after} 個檔案"
  else
    note_fail "${USER_DIR} 還在但是空的 —— 使用者的詞典被清掉了"
  fi
else
  note_fail "解除安裝把 ${USER_DIR} 刪掉了。
     那是使用者的資料(學過的詞、自訂短語),不是我們的檔案。
     這是唯一一項重裝也補救不回來的失敗。"
fi

# ── 檔案有沒有清乾淨 ──────────────────────────────────────────────
#
# ⚠ 要斷言的是「**我們的**檔案都不在了」,不是「目錄裡一個檔案都沒有」。
#
# Inno 的解除安裝程式沒有辦法在執行中刪掉自己,它用 MoveFileEx 排到下次開機
# 才刪 —— 所以靜默解除安裝之後 unins000.exe / unins000.dat 還在是**正常的**,
# 不是我們沒清乾淨。(實測:第一次跑就只剩這一個檔案,而其餘全部清光。)
# 把它判成失敗的話,這道斷言會在產品完全正確時紅,而那看起來像產品壞了。
#
# 先給它一點時間:解除安裝程式返回之後,刪檔還在收尾。
for i in $(seq 1 30); do
  [ -d "${INSTALL_DIR}" ] || break
  remaining="$(find "${INSTALL_DIR}" -type f ! -name 'unins*' 2>/dev/null | wc -l | tr -d ' ')"
  [ "${remaining}" -eq 0 ] && break
  sleep 1
done

if [ ! -d "${INSTALL_DIR}" ]; then
  ok "安裝目錄已移除"
else
  ours="$( (find "${INSTALL_DIR}" -type f ! -name 'unins*' 2>/dev/null || true) | wc -l | tr -d ' ')"
  if [ "${ours}" -eq 0 ]; then
    left_all="$( (find "${INSTALL_DIR}" -type f 2>/dev/null || true) | wc -l | tr -d ' ')"
    ok "我們的檔案都清掉了(還剩 ${left_all} 個 Inno 自己的 unins*,下次開機由系統刪除)"
  else
    note_fail "安裝目錄還留著 ${ours} 個**我們的**檔案:$( (find "${INSTALL_DIR}" -type f ! -name 'unins*' | head -5 | tr '\n' ' ') )"
  fi
fi

# ══════════════════════════════════════════════════════════════════
#  10. 「連我的資料一起刪」那條路
# ══════════════════════════════════════════════════════════════════
#
# ⚠ 這一節與第 9 節是**一對**,兩條都要驗。
#
#   第 9 節:不帶旗標的靜默解除安裝 → 使用者的詞典必須還在
#   第 10 節:帶 /PURGEUSERDATA 的  → 必須整個消失
#
# 只驗第 9 節的話,「刪除」這個功能等於沒有被驗過;只驗第 10 節的話,
# 更糟 —— 那會讓「靜默解除安裝順手毀掉使用者資料」變成綠燈。
# 這個專案抓過太多次「測試是綠的,因為它沒在測」。
log "10. 「連我的資料一起刪」"

# ── 10a. 反向:不帶確認參數時,那支工具必須什麼都不做 ─────────────
#
# 這一條擋的是「參數被忽略」。忽略了的話,上面每一條「預設保留」的斷言
# 都還是會綠(因為安裝程式那一側沒有叫它),而真正的地雷埋在工具裡。
n_before="$( (find "${USER_DIR}" -type f 2>/dev/null || true) | wc -l | tr -d ' ')"
if [ "${n_before}" -eq 0 ]; then
  note_fail "第 10 節開始時使用者目錄是空的 —— 後面的斷言不算數"
fi
set +e
"${TOOL}" purge-user-data > "${WORK}/purge-noflag.log" 2>&1
rc=$?
set -e
tr -d '\r' < "${WORK}/purge-noflag.log" | sed 's/^/    /'
if [ "${rc}" -eq 0 ]; then
  note_fail "purge-user-data 沒有帶確認參數竟然以 0 結束 —— 那個參數沒有作用"
else
  ok "purge-user-data 不帶確認參數時以 ${rc} 結束,什麼都不做"
fi
n_now="$( (find "${USER_DIR}" -type f 2>/dev/null || true) | wc -l | tr -d ' ')"
[ "${n_now}" -eq "${n_before}" ] \
  && ok "資料一個檔案都沒少(${n_now} 個)" \
  || note_fail "不帶確認參數卻少了檔案:${n_before} -> ${n_now}"

# ── 10b. 路徑是向產品要的,不是腳本自己拼的 ──────────────────────
#
# 安裝程式靠 `user-data-path` 拿路徑(見 .iss 的 QueryUserDataDir)。
# 那一條若壞掉,刪除會去刪一個不存在的目錄然後回報成功 ——
# 使用者以為清乾淨了,其實原封不動。
claimed="$("${TOOL}" user-data-path 2>/dev/null | tr -d '\r' | head -1)"
claimed_u="$(cygpath -u "${claimed}" 2>/dev/null || true)"
if [ -n "${claimed}" ] && [ "${claimed_u}" = "${USER_DIR}" ]; then
  ok "user-data-path 說的是 ${claimed}"
else
  note_fail "user-data-path 說「${claimed}」,而實際的使用者目錄是 ${USER_DIR}
     安裝程式就是靠這一行決定要刪哪裡的。"
fi

# ── 10c. 重裝一次,然後帶旗標靜默解除安裝 ────────────────────────
#
# 重裝很快(詞庫已經編好了,這一步不啟動服務)。要重裝是因為
# 解除安裝程式本身已經在第 8 節被移除了。
log "  重新安裝一次(不啟動服務)"
set +e
"${SETUP}" //VERYSILENT //SUPPRESSMSGBOXES //NORESTART \
           "//LOG=$(w "${WORK}/install2.log")"
rc=$?
set -e
[ "${rc}" -eq 0 ] || { tail -40 "${WORK}/install2.log"; die "第二次安裝以 ${rc} 結束"; }
ok "第二次安裝完成"

UNINST2="$(reg_value "${ARP}" UninstallString | tr -d '"')"
[ -n "${UNINST2}" ] || die "第二次安裝之後 ARP 裡沒有 UninstallString"
UNINST2_U="$(cygpath -u "${UNINST2}")"

n_before="$( (find "${USER_DIR}" -type f 2>/dev/null || true) | wc -l | tr -d ' ')"
[ "${n_before}" -gt 0 ] \
  && ok "重裝之後使用者的詞典還在(${n_before} 個檔案)—— 重裝不會弄丟資料" \
  || note_fail "重裝之後使用者目錄空了"

log "  帶 /PURGEUSERDATA 靜默解除安裝"
set +e
"${UNINST2_U}" //VERYSILENT //SUPPRESSMSGBOXES //NORESTART //PURGEUSERDATA
rc=$?
set -e
for i in $(seq 1 120); do
  reg_key_exists "${ARP}" || break
  sleep 1
done
[ "${rc}" -eq 0 ] || note_fail "帶旗標的解除安裝以 ${rc} 結束"

# 給刪檔一點時間收尾。
for i in $(seq 1 30); do
  [ -d "${USER_DIR}" ] || break
  sleep 1
done

if [ ! -d "${USER_DIR}" ]; then
  ok "**使用者資料已完全刪除** —— /PURGEUSERDATA 真的做了它宣稱的事"
else
  left="$( (find "${USER_DIR}" -type f 2>/dev/null || true) | wc -l | tr -d ' ')"
  if [ "${left}" -eq 0 ]; then
    ok "使用者資料已刪除(目錄殼還在,裡面 0 個檔案)"
  else
    (find "${USER_DIR}" -type f | head -10 | sed 's/^/      /') || true
    note_fail "帶了 /PURGEUSERDATA,使用者目錄裡卻還有 ${left} 個檔案。
     使用者按了「連我的資料一起刪」,而它其實沒刪乾淨 ——
     那比不提供這個選項更糟。"
  fi
fi

# 產品本身也必須清乾淨(這一次沒有第 8 節那些斷言,至少確認 ARP 沒了)。
reg_key_exists "${ARP}" \
  && note_fail "帶旗標解除安裝之後,「新增或移除程式」裡那一筆還在" \
  || ok "新增或移除程式:那一筆已消失"

# ══════════════════════════════════════════════════════════════════
#  11. 有程式握著 DLL 時解除安裝會發生什麼(使用者撞到的那一個)
# ══════════════════════════════════════════════════════════════════
#
# ⚠ 這一節是**用來寫文案的**,不是用來讓 CI 變綠的。
#
# 使用者解除安裝時看到「必須重新啟動」。我們要在文案裡回答三件事:
# 為什麼、不重啟會怎樣、可不可以晚點再重啟。**那三件事都不可以用猜的。**
#
# 所以這裡把他的處境重現出來:讓一個進程握著 rime_tsf.dll(那正是
# 檔案總管與瀏覽器在做的事),然後解除安裝,再量三件事:
#   1. 檔案還在不在
#   2. 有沒有被排進「開機時刪除」的佇列(→ 為什麼登出不夠)
#   3. **不重開機就重裝的話,安裝程式擋不擋**(→ 「不重啟會怎樣」的具體後果)
#
# 量到的結果直接寫進 .iss 的 UninstalledAndNeedsRestart。
if [ -n "${HOST}" ]; then
  log "11. 有程式握著 DLL 時解除安裝(重現使用者的處境)"
  set +e
  "${SETUP}" //VERYSILENT //SUPPRESSMSGBOXES //NORESTART \
             "//LOG=$(w "${WORK}/install3.log")"
  rc=$?
  set -e
  [ "${rc}" -eq 0 ] || die "第三次安裝以 ${rc} 結束"

  UNINST3="$(reg_value "${ARP}" UninstallString | tr -d '"')"
  UNINST3_U="$(cygpath -u "${UNINST3}")"

  log "  讓一個進程握住 ${INSTALL_DIR_W}\\rime_tsf.dll"
  "${HOST}" --hold-dll "${INSTALL_DIR_W}\\rime_tsf.dll" --hold-ms 90000 \
    > "${WORK}/hold.log" 2>&1 &
  HOLD_PID=$!
  hold_cleanup() { kill "${HOLD_PID}" 2>/dev/null || true; }
  trap hold_cleanup EXIT
  for i in $(seq 1 20); do
    grep -q '已載入' "${WORK}/hold.log" 2>/dev/null && break
    sleep 1
  done
  if grep -q '已載入' "${WORK}/hold.log" 2>/dev/null; then
    ok "DLL 已被另一個進程握住"
  else
    tr -d '\r' < "${WORK}/hold.log" | sed 's/^/    /'
    note_fail "沒有握住 DLL —— 這一節量不到任何東西"
  fi

  log "  在這個狀態下靜默解除安裝"
  set +e
  "${UNINST3_U}" //VERYSILENT //SUPPRESSMSGBOXES //NORESTART
  set -e
  for i in $(seq 1 120); do
    reg_key_exists "${ARP}" || break
    sleep 1
  done

  echo "  --- 量到的事實(文案就照這個寫)---"
  if [ -f "${INSTALL_DIR}/rime_tsf.dll" ]; then
    echo "    · rime_tsf.dll **還在磁碟上**(被握著,刪不掉)"
  else
    echo "    · rime_tsf.dll 已經不在了"
  fi
  if pending_has_our_dll; then
    echo "    · 已被排進「開機時刪除」的佇列(PendingFileRenameOperations)"
    echo "      → 那份佇列只有 Session Manager 在**開機**時處理;"
    echo "        登出再登入不會碰它。所以「重新啟動」不是誇大。"
    ok "重現成功:被握著的 DLL 進了開機刪除佇列"
  else
    echo "    · **沒有**進開機刪除佇列"
    pending_renames | sed 's/^/      /'
    note_fail "檔案被握著,卻沒有排進開機刪除佇列 —— 那它永遠不會被刪掉,
     而使用者卻被要求重新啟動。這兩件事必須一致。"
  fi

  # ── ⚠ 不重開機就重裝,現在**必須**成功 ────────────────────────
  #
  # 這一格從「量測」升級成「斷言」,而升級的理由是文案改了。
  #
  # 舊的作法讓 Inno 把 rime_tsf.dll 排進開機佇列,於是重裝會被
  # PreviousInstallNotCompleted 擋下來(實測結束碼 8)——所以舊文案寫的是
  # 「在重新啟動之前您裝不回來」。
  #
  # 現在解除安裝會先把那顆 DLL **改名挪開**,佇列裡再也沒有那個固定路徑,
  # 於是重裝沒有東西可以擋。新文案據此寫了「要重新安裝也不必先重開機」。
  #
  # ⚠ 文案宣稱做得到、實際做不到,是這個專案吃過虧的那一種。
  #   所以這裡不再只是印出結束碼 —— 它得真的是 0。
  log "  不重開機直接重裝(文案宣稱做得到,這裡驗它)"
  set +e
  "${SETUP}" //VERYSILENT //SUPPRESSMSGBOXES //NORESTART \
             "//LOG=$(w "${WORK}/install4.log")"
  rc4=$?
  set -e
  echo "    · 重裝的結束碼 = ${rc4}"
  if tr -d '\r' < "${WORK}/install4.log" 2>/dev/null \
       | grep -aqi 'previous\|restart'; then
    echo "    · 安裝記錄裡提到 previous / restart:"
    tr -d '\r' < "${WORK}/install4.log" | grep -ai 'previous\|restart' \
      | head -5 | sed 's/^/      /'
  else
    echo "    · 安裝記錄裡沒有提到 previous / restart"
  fi
  if [ "${rc4}" -eq 0 ] && [ -f "${INSTALL_DIR}/rime_tsf.dll" ]; then
    ok "**解除安裝之後不重開機就裝得回來**(結束碼 0)——
     UninstalledAndNeedsRestart 那句「要重新安裝也不必先重開機」是真的。"
  else
    note_fail "解除安裝之後不重開機就重裝失敗(結束碼 ${rc4})。
     解除安裝的對話框現在寫著「要重新安裝也不必先重開機」——
     文案宣稱做得到而實際做不到,是這個專案吃過虧的那一種。
     要嘛修好(讓解除安裝不要在佇列裡留下那個固定路徑),
     要嘛把那句話改回去。**不可以留著一句不成立的話。**"
  fi

  hold_cleanup
  trap - EXIT
else
  log "11. (沒有給 --host,跳過「有程式握著 DLL 時解除安裝」那一節)"
fi

# ══════════════════════════════════════════════════════════════════
#  14. ⚠ 反向測試:把「改名挪開」拿掉,§13 的核心斷言必須紅
# ══════════════════════════════════════════════════════════════════
#
# ⚠⚠ **這一節是 §13 唯一的擔保,而且它比 §13 本身更重要。**
#
# 沒有它的話,§13 那句「升級沒有把任何東西排進開機佇列」有可能只是因為
# runner 上根本沒有人握著 DLL 而通過 —— 也就是一個永遠綠、永遠沒在測的
# 關卡。這個專案已經抓過四次那種東西(單元測試框架、離線稽核、
# 安裝包資料檢查、發布關卡的升級測試),所以每一道新關卡都要先證明它會紅。
#
# 做法:用 /LEGACYINPLACE 跑一次安裝。那個旗標讓 PrepareToInstall
# **跳過改名挪開**,退回 Inno 原本的行為(原地覆蓋、換不掉就排隊等開機)。
# 也就是說,它重現的正是這一輪修掉的那個缺陷。
#
# 要求:在有人握著 DLL 的情況下,佇列裡必須真的出現一筆目的地是
# rime_tsf.dll 的項目。出現了 = §13 那條斷言有牙齒。
#
# ⚠ 這一節放在**整支腳本的最後面**是刻意的:它會在 PendingFileRenameOperations
#   裡留下一筆真的項目,而那會讓前面每一條看佇列的斷言(§8、§13)失去意義。
#   放在最後就不必去動登錄檔清理它 —— 清理登錄檔本身是比留著更糟的風險。
if [ -n "${HOST}" ]; then
  log "14. 反向測試:拿掉「改名挪開」之後,§13 的斷言必須紅"

  # 先回到一個乾淨的已安裝狀態(§11 之後的狀態是不確定的)。
  set +e
  "${SETUP}" //VERYSILENT //SUPPRESSMSGBOXES //NORESTART \
             "//LOG=$(w "${WORK}/p14-install.log")"
  rc=$?
  set -e
  [ "${rc}" -eq 0 ] || note_fail "§14 的前置安裝以 ${rc} 結束"
  "${INSTALL_DIR}/rime_ime_setup.exe" sweep-stale-dlls --dir "${INSTALL_DIR_W}" \
    >/dev/null 2>&1 || true

  # 前提:現在佇列裡**不可以**已經有一筆指著那個固定路徑的項目 ——
  # 有的話,後面量到的就不知道是誰留下的,這個反向測試等於沒做。
  if pending_targets_live_dll; then
    echo "  --- PendingFileRenameOperations ---"
    pending_entries | sed 's/^/    /'
    note_fail "§14 開始前佇列裡就已經有一筆指著 rime_tsf.dll ——
     這個反向測試分不出結果是誰造成的,所以它證明不了任何事。"
  elif [ ! -f "${DLL_U}" ]; then
    note_fail "§14 開始前找不到 ${DLL_U},反向測試跑不了"
  else
    log "  讓一個進程握住 ${DLL_W}"
    "${HOST}" --hold-dll "${DLL_W}" --hold-ms 180000 \
      > "${WORK}/p14-hold.log" 2>&1 &
    P14_PID=$!
    p14_cleanup() { kill "${P14_PID}" 2>/dev/null || true; }
    trap p14_cleanup EXIT
    HELD=0
    for _ in $(seq 1 30); do
      grep -q '已載入' "${WORK}/p14-hold.log" 2>/dev/null && { HELD=1; break; }
      sleep 1
    done
    if [ "${HELD}" -ne 1 ]; then
      tr -d '\r' < "${WORK}/p14-hold.log" | sed 's/^/    /'
      note_fail "沒有握住 DLL —— 反向測試量不到任何東西"
    else
      ok "DLL 已被另一個進程握住(重現使用者的處境)"
      log "  帶 /LEGACYINPLACE 安裝(刻意跳過改名挪開)"
      set +e
      "${SETUP}" //VERYSILENT //SUPPRESSMSGBOXES //NORESTART //LEGACYINPLACE \
                 "//LOG=$(w "${WORK}/p14-legacy.log")"
      rc=$?
      set -e
      LEGLOG="$(tr -d '\r' < "${WORK}/p14-legacy.log" 2>/dev/null || true)"
      [ "${rc}" -eq 0 ] || note_fail "帶 /LEGACYINPLACE 的安裝以 ${rc} 結束"

      # 旗標真的生效了嗎。沒生效的話,下面那一條會因為「機制照樣跑了」
      # 而不紅,而我們會誤以為反向測試通過。
      if printf '%s' "${LEGLOG}" | grep -q 'LEGACYINPLACE'; then
        ok "/LEGACYINPLACE 生效了(安裝記錄裡有那一行)"
      else
        note_fail "/LEGACYINPLACE 沒有生效 —— 安裝記錄裡沒有那一行。
     這個反向測試等於跑了一次正常安裝,證明不了 §13 會紅。"
      fi
      if printf '%s' "${LEGLOG}" | grep -q '已改名挪開'; then
        note_fail "帶了 /LEGACYINPLACE,卻還是改名挪開了 —— 旗標沒有作用。"
      fi

      # ── 核心:§13 那條斷言在這裡必須紅 ──────────────────────
      if pending_targets_live_dll; then
        echo "  --- PendingFileRenameOperations(這正是我們要消滅的東西)---"
        pending_entries | grep -i 'rime_tsf' | sed 's/^/    /'
        ok "**反向測試通過**:退回原地覆蓋之後,rime_tsf.dll 真的被排進了
     開機佇列 —— 也就是說 §13 那條「沒有排進開機佇列」是有牙齒的,
     它不是因為 runner 上沒人握著 DLL 才通過的。"
      else
        echo "  --- PendingFileRenameOperations ---"
        pending_entries | sed 's/^/    /'
        note_fail "⚠ **反向測試沒有紅。**
     有一個進程握著 rime_tsf.dll,而且刻意跳過了改名挪開,
     Inno 卻仍然沒有把它排進開機佇列。兩種可能,兩種都要處理:
       (a) 這台 runner 上「握著 DLL」沒有真的成立 ——
           那麼 §13 那條斷言也是空的,它一直沒在測東西;
       (b) Inno 自己就會把換不掉的檔案挪開 ——
           那麼改名挪開是多餘的複雜度,應該拿掉。
     在查清楚是哪一種之前,不可以宣稱「升級不重啟」有被驗證。"
      fi
    fi
    p14_cleanup
    trap - EXIT
  fi
else
  log "14. (沒有給 --host,跳過反向測試 —— 這也表示 §13 沒有擔保)"
fi

# ══════════════════════════════════════════════════════════════════
#  15. 最後再掃一次「建 session 有沒有超過預算」
# ══════════════════════════════════════════════════════════════════
#
# ⚠ 這一次才涵蓋得到 §13(升級)自己起的那幾支服務。
#   §6e 那一次跑在 §13 之前,看不到它們 —— 而 2026-08-09 那一輪最慢的
#   三次(250 / 297 / 328)**全部**在那裡,於是 §6e 報「最久 109 ms、
#   0 次超過」,而隔壁的 §13 正因為那個 328 紅著。
#   一道只掃部分記錄的關卡,與一道只量成功案例的關卡是同一種錯。
log "15. 全部跑完之後,再掃一次所有服務記錄"
assert_session_new_budget "15"

echo
[ "${fail}" -eq 0 ] || die "安裝程式驗證失敗,見上。"
log "安裝程式驗證全部通過 ✓"
