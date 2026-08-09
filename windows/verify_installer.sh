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
#   · 切過去之後 ActivateEx 有沒有被呼叫、組字視窗會不會出現
#   · 候選窗長什麼樣、位置對不對、高 DPI 與多螢幕
#   · 使用者的語言列上看不看得到它(還取決於使用者的語言清單,
#     而 runner 上沒有辦法製造那個情境)
#   · 「每一顆鍵是不是都真的做了它宣稱的事」
#
# 用法(Git Bash,需要系統管理員權限):
#   windows/verify_installer.sh --setup <RimeQuad-Setup-x64.exe> \
#                               --probe <rime_probe.exe> \
#                               --tool  <rime_ime_setup.exe>
#
set -euo pipefail

log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[error]\033[0m %s\n' "$*" >&2; exit 1; }

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
    *) die "未知參數: $1" ;;
  esac
done
[ -f "${SETUP}" ] || die "找不到安裝程式: ${SETUP}"
[ -f "${PROBE}" ] || die "找不到 rime_probe.exe: ${PROBE}"
[ -f "${TOOL}" ]  || die "找不到 rime_ime_setup.exe: ${TOOL}"
[ -z "${HOST}" ] || [ -f "${HOST}" ] || die "找不到 rime_tsf_host.exe: ${HOST}"

command -v cygpath >/dev/null 2>&1 || die "必須在 Git Bash / MSYS2 下執行"
w() { cygpath -w "$1"; }

# 安裝目錄不寫死 C:\。Inno 的 {autopf} 在 64 位元安裝模式下展開成
# %ProgramW6432%(而不是 (x86) 那一份),所以就照那個算 ——
# runner 的系統碟哪天不是 C: 的話,寫死的版本會以「檔案不存在」的形式失敗,
# 而那看起來像安裝程式壞了。
PF_W="${ProgramW6432:-${PROGRAMFILES:-C:\\Program Files}}"
INSTALL_DIR_W="${PF_W}\\RimeQuad"
INSTALL_DIR="$(cygpath -u "${PF_W}")/RimeQuad"
USER_DIR="${APPDATA:-}"
[ -n "${USER_DIR}" ] || die "APPDATA 是空的"
USER_DIR="$(cygpath -u "${USER_DIR}")/RimeQuad"
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
# 一行一個:PROFILE=0x0404={07FB3057-…}
PROFILES="$(grep '^PROFILE=' "${WORK}/paths.clean" | sed 's/^PROFILE=//')"

# 交叉比對:這幾個值一旦發布出去就不能改。
EXPECT_CLSID='{E94B9FC2-6730-45AD-A462-B7D02995D95B}'
# 語言 → profile GUID。**這張表就是「輸入法出現在哪些語言底下」。**
# 使用者回報過:只註冊 0x0404 的話,系統語言是簡體中文的人在自己的語言
# 底下找不到這個輸入法(它掛在「繁体中文(中国台湾)」那一欄)。
EXPECT_PROFILES="$(printf '%s\n' \
  '0x0404={07FB3057-4192-4868-AB6E-E4EE5597C0FE}' \
  '0x0804={57BE9E4D-3F4E-4B4F-959B-E85E6095F2CA}' \
  '0x0C04={23BBABB2-5C8A-4751-85F1-B360C70A5637}')"

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
  tr -d '\r' < "${WORK}/install.log" | grep -a 'RimeQuad:' | sed 's/^/    /' || true

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
  if tr -d '\r' < "${WORK}/install.log" | grep -aq 'RimeQuad: enable-user'; then
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
  guid="${line#*=}"           # {07FB3057-…}
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
  && ok "能力類別 ${n_cat} 類(GUID_TFCAT_TIP_KEYBOARD + 5 個能力類別)" \
  || note_fail "Category\\Category 底下 ${n_cat} 個,預期 ${CATEGORY_COUNT} 個
     少一個的症狀是「在市集 App / Edge / 提權視窗裡沒有這個輸入法」"

# ── 新增或移除程式 ────────────────────────────────────────────────
#
# Inno 的 ARP 鍵名是 <AppId>_is1。AppId 改了的話舊版會永遠留在
# 「新增或移除程式」裡而且解除安裝不掉,所以這裡也寫死一份做交叉比對。
ARP='HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{7A033CF7-CB91-408E-A653-EF639F4173DB}_is1'
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

if "${INSTALL_DIR}/rime_ime_setup.exe" check --user > "${WORK}/check-user.log" 2>&1; then
  ok "目前使用者已被啟用(HKCU 底下有設定檔)"
else
  cat "${WORK}/check-user.log"
  note_fail "使用者側沒有被啟用 —— 安裝程式的 enable-user 那一步沒有生效。
     症狀是「裝完了,但輸入法清單裡看不到」。"
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

# ⚠ --print-choice 會讀 %APPDATA%\RimeQuad\rimequad.settings,而設定裡釘的
#   方案優先於輸入模式。runner 上那個檔案不存在,所以讀到的就是「沒釘」——
#   但如果哪天有人在這支腳本前面寫了設定,這段斷言會安靜地變成在測別的東西。
if [ -f "${USER_DIR}/rimequad.settings" ]; then
  note_fail "runner 上竟然已經有 rimequad.settings —— 下面的斷言測到的
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
neg="$(choice_out 0x0409)"
if [ "$(field_of "${neg}" variant)" = "(不干預)" ] && \
   ! echo "${neg}" | grep -q '^option='; then
  ok "0x0409(en-US)完全不碰簡繁 —— 上面幾條不是恆真的"
else
  note_fail "0x0409 竟然動了簡繁:
$(echo "${neg}" | sed 's/^/       /')
     規範 §4.2:認不出來的輸入模式一律不表示意見,不要預設繁體。"
fi

# ══════════════════════════════════════════════════════════════════
#  6. 用**安裝好的**東西真的打出「你好」
# ══════════════════════════════════════════════════════════════════
#
# 刻意不給 --shared / --user:要驗的正是「安裝出來的那一份自己就夠了」。
# 走的路是 rime_probe → 真的具名管道 → 安裝好的 rime_service
#           → rime_shell → librime → 安裝好的詞庫。
# 也就是 DLL 會走的每一段,除了 TSF 本身。
log "6. 端到端:用安裝好的服務與資料打出「你好」"

# 先照一張安裝目錄的相片(路徑 + 大小 + 修改時間)。
# 跑完之後要一模一樣 —— 那證明使用者資料真的沒有寫進 Program Files。
snapshot() { (cd "${INSTALL_DIR}" && find . -type f -printf '%p\t%s\t%T@\n' | sort); }
snapshot > "${WORK}/before.txt"

READY="${WORK}/ready.txt"; rm -f "${READY}"
"${INSTALL_DIR}/rime_service.exe" \
  --no-ui --wait-deploy 1200 \
  --ready-file "$(w "${READY}")" --quit-after 900 \
  > "${WORK}/service.log" 2>&1 &
SVC_PID=$!
cleanup() {
  if kill -0 "${SVC_PID}" 2>/dev/null; then
    # 走正常的停止路徑(送結束事件),順便把它也驗一次。
    "${INSTALL_DIR}/rime_ime_setup.exe" stop-service --dir "${INSTALL_DIR_W}" \
      > "${WORK}/stop.log" 2>&1 || true
    kill "${SVC_PID}" 2>/dev/null || true
    wait "${SVC_PID}" 2>/dev/null || true
  fi
}
trap cleanup EXIT

log "  等待服務就緒(首次部署要編譯詞庫,可能數分鐘)"
for i in $(seq 1 1200); do
  [ -f "${READY}" ] && break
  if ! kill -0 "${SVC_PID}" 2>/dev/null; then
    echo "--- service.log ---"; tr -d '\r' < "${WORK}/service.log"
    die "服務進程提前結束了"
  fi
  sleep 1
  [ $((i % 60)) -eq 0 ] && log "    ...已等 ${i}s"
done
[ -f "${READY}" ] || { tr -d '\r' < "${WORK}/service.log"; die "服務在 1200 秒內沒有就緒"; }
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
"${PROBE}" --keys nihao --select 1 --schema luna_pinyin_tw --expect 你好 \
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
  "${HOST}" --langid 0x0404 --require-activate --require-eaten \
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
ok "使用者目錄裡有 ${n_user} 個檔案"

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
# 量到的結果直接寫進 installer/rimequad.iss 的 UninstalledAndNeedsRestart。
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

  # 不重開機就重裝,安裝程式擋不擋?
  #
  # 這一格決定文案要不要寫「重新啟動之前不要重新安裝」。Inno 有一則
  # PreviousInstallNotCompleted 的內建訊息,但它到底會不會在**解除安裝**
  # 留下的佇列上觸發,只有實測知道。
  log "  不重開機直接重裝,看安裝程式擋不擋"
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

  hold_cleanup
  trap - EXIT
else
  log "11. (沒有給 --host,跳過「有程式握著 DLL 時解除安裝」那一節)"
fi

echo
[ "${fail}" -eq 0 ] || die "安裝程式驗證失敗,見上。"
log "安裝程式驗證全部通過 ✓"
