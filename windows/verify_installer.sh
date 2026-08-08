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

SETUP=""; PROBE=""; TOOL=""
while [ $# -gt 0 ]; do
  case "$1" in
    --setup) SETUP="$2"; shift 2 ;;
    --probe) PROBE="$2"; shift 2 ;;
    --tool)  TOOL="$2";  shift 2 ;;
    *) die "未知參數: $1" ;;
  esac
done
[ -f "${SETUP}" ] || die "找不到安裝程式: ${SETUP}"
[ -f "${PROBE}" ] || die "找不到 rime_probe.exe: ${PROBE}"
[ -f "${TOOL}" ]  || die "找不到 rime_ime_setup.exe: ${TOOL}"

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
for line in ${PROFILES}; do
  lang="${line%%=*}"          # 0x0404
  guid="${line#*=}"           # {07FB3057-…}
  key="${HKLM_CTF}\\LanguageProfile\\$(profile_langkey "${lang}")\\${guid}"
  if reg_key_exists "${key}"; then
    ok "${key}"
  else
    note_fail "缺少 ${key}
     使用 ${lang} 這個語言的人在自己的語言底下找不到這個輸入法"
  fi
done

n_cat="$(reg query "${HKLM_CTF_CAT}" //reg:64 2>/dev/null | tr -d '\r' | grep -c "^HKEY_" || true)"
# reg query 會把被查的鍵自己也列出來,所以子鍵數要減一。
n_cat="$((n_cat > 0 ? n_cat - 1 : 0))"
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

set +e
"${PROBE}" --keys nihao --select 1 --schema luna_pinyin_tw --expect 你好 \
  > "${WORK}/probe.log" 2>&1
rc=$?
set -e
tr -d '\r' < "${WORK}/probe.log"
if [ "${rc}" -ne 0 ]; then
  echo "--- service.log ---"
  tr -d '\r' < "${WORK}/service.log" | grep -v -E '^[WIEF][0-9]{4,8} ' || true
  note_fail "probe 以 ${rc} 結束"
else
  # 錨定整行精確比對。只用 grep -q 你好 是不夠的:上屏成「你好嗎」一樣會過。
  if grep -qE '^>>> COMMIT: "你好"$' <(tr -d '\r' < "${WORK}/probe.log"); then
    ok "經由具名管道以安裝好的資料打出「你好」"
  else
    note_fail "probe 的輸出裡沒有 >>> COMMIT: \"你好\""
  fi
fi

# ── 使用者詞典去了哪裡 ────────────────────────────────────────────
[ -d "${USER_DIR}" ] && ok "使用者資料目錄已建立: ${USER_DIR}" \
                     || note_fail "沒有建立 ${USER_DIR}"
[ -f "${USER_DIR}/default.custom.yaml" ] \
  && ok "範本 default.custom.yaml 已補進使用者目錄" \
  || note_fail "使用者目錄裡沒有 default.custom.yaml —— schema_list 會退回上游那一份,
     而它列了我們沒有詞庫的方案(部署噴錯,使用者看到的是「有些方案沒有候選」)"
n_user="$(find "${USER_DIR}" -type f 2>/dev/null | wc -l | tr -d ' ')"
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
for line in ${PROFILES}; do
  lang="${line%%=*}"; guid="${line#*=}"
  key="${HKLM_CTF}\\LanguageProfile\\$(profile_langkey "${lang}")\\${guid}"
  if reg_key_exists "${key}"; then
    note_fail "${key} 還在 —— ${lang} 的語言設定檔沒有被清掉"
  else
    ok "${lang} 的語言設定檔已清除"
  fi
done

# ── 反向測試:解除安裝之後 check 必須紅 ──────────────────────────
if "${TOOL}" check > "${WORK}/check-uninstalled.log" 2>&1; then
  cat "${WORK}/check-uninstalled.log"
  note_fail "解除安裝之後 check 竟然還通過"
else
  ok "解除安裝之後 check 以非零結束"
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

# 檔案有沒有清乾淨(允許 restartreplace 留下待重開機刪除的殘骸)。
if [ -d "${INSTALL_DIR}" ]; then
  left="$(find "${INSTALL_DIR}" -type f 2>/dev/null | wc -l | tr -d ' ')"
  [ "${left}" -eq 0 ] && ok "安裝目錄已清空" \
    || note_fail "安裝目錄還留著 ${left} 個檔案:$(find "${INSTALL_DIR}" -type f | head -5 | tr '\n' ' ')"
else
  ok "安裝目錄已移除"
fi

echo
[ "${fail}" -eq 0 ] || die "安裝程式驗證失敗,見上。"
log "安裝程式驗證全部通過 ✓"
