#!/usr/bin/env bash
#
# windows/check_installer_messages.sh — 安裝程式的每一句話都翻過了嗎
#
# ══ 為什麼需要這支 ═════════════════════════════════════════════════
#
# 使用者走完整個解除安裝流程,在**最後一個對話框**撞到一句英文:
#
#   To complete the uninstallation of <產品名>, your computer must be
#   restarted. Would you like to restart now?
#
# 而標題與按鈕是中文。原因是我們只覆寫了自己想得到的那幾句
# (34 句),Inno 內建的其餘 247 句照樣是英文 —— 它們平常不會出現,
# 所以「前面沒撞到」完全不代表沒有。
#
# **「我逐頁走了一遍」不是一個可以維護的答案。** 下一個人加一個
# [Tasks] 區段、或 Inno 升版多了幾句新訊息,英文就又漏出來了,
# 而且一樣要等使用者回報才知道。
#
# 所以這支腳本把它變成機器檢查的事:拿 ISCC 自己的 Default.isl,
# 逐一比對我們的 [Messages] 有沒有覆蓋到,並且比對**佔位符有沒有對上**。
#
# ══ 兩件事 ═════════════════════════════════════════════════════════
#
# 1. **完整性**:Default.isl 的每一個訊息 ID 都要被我們覆蓋,
#    否則就必須列在下面的 SKIP 清單裡**並附上理由**。
#    (也反過來查:我們覆蓋了一個 Default.isl 裡不存在的 ID ——
#     那是打錯字,ISCC 不會報錯,它會安靜地忽略,而那一句永遠是英文。)
#
# 2. **佔位符**:每一句的 %1..%9、%n、[name] 這些必須與原文一模一樣。
#    Inno 自己的翻譯守則第一條就是這個,而它壞掉的長相是
#    「對話框裡少了一段、或多出一個 %2」—— 使用者會覺得軟體壞了。
#
# ══ 參考檔從哪裡來(2026-08-13 改) ══════════════════════════════════
#
# 覆核時實跑:
#
#     $ bash windows/check_installer_messages.sh              EXIT=1
#     $ REFERENCE_ISL=/tmp/Default.isl bash …                 EXIT=0
#
# 也就是說「這一關是綠的」靠的是一個**不在版控裡、8/9 隨手放在 /tmp 的
# 檔案** —— 換一台機器、或那台機器清一次 /tmp 就不成立,而且沒有人會知道。
# 那是一種比紅燈更糟的狀態:看起來有守門,實際上守門的條件寫在某個人的
# 記憶裡。
#
# 所以參考檔收進版控了(windows/third_party/inno/Default.isl,provenance 見
# 同目錄的 README.md),而且**釘 sha256** —— 它是這一關的**對照答案**,
# 刪掉裡面一句就等於讓那一句永遠不算「漏翻」。
#
# 優先序,由高到低:
#   1. REFERENCE_ISL=<路徑>    明確指定,不動聲色地相信呼叫者(不驗 sha)
#   2. ISCC 旁邊的那一份        **CI 走這條**。runner 真的拿去編譯的版本,
#                               這一次才算數。
#   3. 版控裡的那一份           沒有 ISCC 的機器(開發機)走這條,會明白說出
#                               「用的是替身」,並驗 sha256。
#
# 用法:
#   windows/check_installer_messages.sh                 (照上面的優先序自己找)
#   REFERENCE_ISL=<路徑> windows/check_installer_messages.sh
set -euo pipefail

log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[error]\033[0m %s\n' "$*" >&2; exit 1; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# 產品識別:值的唯一來源是 scripts/lib/product.env(.iss 的檔名也是從
# PRODUCT_ID_ROOT 推出來的,所以下一次改名時這一行不必動)。
# shellcheck disable=SC1091
. "${SCRIPT_DIR}/product_win.sh"

# ISS_OVERRIDE 只給下面的反向測試用:它要拿一份動過手腳的複本跑同一段邏輯。
ISS="${ISS_OVERRIDE:-${SCRIPT_DIR}/${RS_WIN_ISS_REL}}"
[ -f "${ISS}" ] || die "找不到 ${ISS}"

SELF_CHECK=0
[ "${1:-}" = "--self-check" ] && SELF_CHECK=1

# ── 找 Default.isl ────────────────────────────────────────────────
#
# 版控裡那一份的 sha256(**去掉 CR 之後**才算)。
# ⚠ 為什麼不是檔案原樣的 sha:那一份是 CRLF 的,而 git 的換行正規化只要
#   在哪一台機器上被打開,原樣的 sha 就會變 —— 那時紅的是這一關,而
#   安裝程式的翻譯一個字都沒動。要擋的是「有人動了對照答案」,不是換行。
REF_ISL_VENDORED="${SCRIPT_DIR}/third_party/inno/Default.isl"
REF_ISL_SHA256=14267c5b21dfeb33e1bc65455f7b59d8443132fea13f351e251a316b95b0178e

# ⚠ 印的是「來源<TAB>路徑」,不是只有路徑。
#   在函式裡設全域變數再由 `$(...)` 取路徑是行不通的 —— 命令替換跑在
#   **子殼**裡,那個變數回不到這裡,而 case 會靜靜地一個分支都不match
#   (實測第一版就是這樣:sha256 那一關整段沒跑,輸出看起來卻很正常)。
find_isl() {
  if [ -n "${REFERENCE_ISL:-}" ]; then
    printf 'REFERENCE_ISL\t%s\n' "${REFERENCE_ISL}"; return
  fi
  local p
  for p in \
    "/c/Program Files (x86)/Inno Setup 6/Default.isl" \
    "/c/Program Files/Inno Setup 6/Default.isl" \
    "/c/ProgramData/chocolatey/lib/InnoSetup/tools/Default.isl"
  do
    [ -f "${p}" ] && { printf 'ISCC\t%s\n' "${p}"; return; }
  done
  # ISCC 在 PATH 上時,Default.isl 就在它旁邊。
  if command -v ISCC.exe >/dev/null 2>&1; then
    local d
    d="$(dirname "$(command -v ISCC.exe)")"
    [ -f "${d}/Default.isl" ] && { printf 'ISCC\t%s\n' "${d}/Default.isl"; return; }
  fi
  # 沒有 ISCC 的機器(開發機)。版控裡有一份,見 third_party/inno/README.md。
  if [ -f "${REF_ISL_VENDORED}" ]; then
    printf 'vendored\t%s\n' "${REF_ISL_VENDORED}"; return
  fi
  printf '\t\n'
}

IFS=$'\t' read -r ISL_SOURCE ISL <<< "$(find_isl)"
# ⚠ 找不到就**明確地失敗**,不要安靜地通過。
#   一支「找不到參考檔就說沒問題」的檢查,正是這個專案抓過最多次的
#   那種失敗模式:測試是綠的,因為它沒在測。
#   ——— 而現在版控裡就有一份,走到這裡等於那一份被刪了。
[ -n "${ISL}" ] || die "找不到 Inno 的 Default.isl。
  版控裡本來有一份:${REF_ISL_VENDORED}(見同目錄 README.md)—— 它不見了嗎?
  CI 上用的是 ISCC 旁邊那一份;要指定別的請設 REFERENCE_ISL=<路徑>。
  **不要把這一步改成找不到就跳過** —— 那等於把一個沒有人在看的東西寫成綠燈。"

case "${ISL_SOURCE}" in
  ISCC)
    log "參考:${ISL}(ISCC 旁邊的那一份 —— 這一次算數)" ;;
  REFERENCE_ISL)
    log "參考:${ISL}(REFERENCE_ISL 指定)"
    echo "     ⚠ 這是呼叫者指定的檔案,不驗 sha256。CI 上的那一次才算數。" >&2 ;;
  vendored)
    # 版控裡那一份是**對照答案**:少掉一句,那一句就永遠不算漏翻。
    got="$(tr -d '\r' < "${ISL}" | sha256sum | cut -d' ' -f1)"
    [ "${got}" = "${REF_ISL_SHA256}" ] || die "版控裡的參考檔被動過了:
  ${ISL}
  期望(去掉 CR 後)sha256 = ${REF_ISL_SHA256}
  實際                    = ${got}
  它是這一關的對照答案,刪掉裡面一句就等於讓那一句永遠不算漏翻。
  真的要換版本的話,連同 REF_ISL_SHA256 與 third_party/inno/README.md 一起改。"
    log "參考:${ISL}(版控裡的替身,sha256 對得上)"
    echo "     這台機器沒有 ISCC,用的是版控裡那一份 Inno 6.5.0 的原文。" >&2
    echo "     **CI 上對著 runner 自己的 Default.isl 跑的那一次才算數。**" >&2 ;;
esac

# ── 刻意不翻的 ────────────────────────────────────────────────────
#
# 每一項都要有理由,而且理由要是「這句話在我們的安裝程式裡不可能出現」,
# 不是「我懶得翻」。目前只有四項,而且都是 Inno 自己就留空的欄位。
SKIP_IDS=(
  # Inno 的 Default.isl 裡本來就是空字串,是給翻譯者放「譯者註」用的欄位。
  # 覆寫成空字串沒有意義,留給 Inno 就好。
  HelpTextNote
  AboutSetupNote
  TranslatorNote
  BeveledLabel
)
is_skipped() {
  local id="$1" s
  for s in "${SKIP_IDS[@]}"; do [ "${id}" = "${s}" ] && return 0; done
  return 1
}

tmp="$(mktemp -d)"
trap 'rm -rf "${tmp}"' EXIT

# ── 抽出兩邊的 ID ─────────────────────────────────────────────────
#
# Default.isl 的 [Messages] 從 [Messages] 到 [CustomMessages] 之間。
# 只取「行首就是識別字加等號」的行 —— 續行(以空白開頭)不算。
# ⚠ 每一行先去掉結尾的 CR。Default.isl 是 CRLF 的,而我們的 .iss 是 LF ——
#   不處理的話 `$0 == "[Messages]"` 永遠不成立,於是抽出 0 個 ID,
#   而這支腳本會**通過**(0 個漏翻的)。實測第一次就是這樣:
#   底下那道「抽出來的數量太少就死」的檢查是為此加的。
extract_ids() {  # $1 = 檔案, $2 = 起始區段
  awk -v start="[$2]" '
    { sub(/\r$/, "") }
    $0 == start { inside = 1; next }
    /^\[/ { inside = 0 }
    inside && /^[A-Za-z][A-Za-z0-9_]*=/ {
      i = index($0, "="); print substr($0, 1, i - 1)
    }
  ' "$1"
}
extract_kv() {  # 同上,但輸出 id<TAB>值
  awk -v start="[$2]" '
    { sub(/\r$/, "") }
    $0 == start { inside = 1; next }
    /^\[/ { inside = 0 }
    inside && /^[A-Za-z][A-Za-z0-9_]*=/ {
      i = index($0, "="); printf "%s\t%s\n", substr($0, 1, i - 1), substr($0, i + 1)
    }
  ' "$1"
}

extract_ids "${ISL}" Messages | LC_ALL=C sort -u > "${tmp}/ref.txt"
extract_ids "${ISS}" Messages | LC_ALL=C sort -u > "${tmp}/ours.txt"
n_ref="$(wc -l < "${tmp}/ref.txt" | tr -d ' ')"
n_ours="$(wc -l < "${tmp}/ours.txt" | tr -d ' ')"
# ⚠ 下界從 100 提到 250。參考檔**就是**這一關的對照答案:它少掉一段,
#   那一段就不再算「漏翻」,而輸出仍然是一片綠字。100 這個數字擋得住
#   「CRLF 沒處理好、抽出 0 個」,擋不住「參考檔被截掉一半」。
#   Inno 6.5.0 是 281 句;真的哪天官方少於 250,連同這個數字一起改,
#   而那時要改的人會被迫看一眼為什麼。
[ "${n_ref}" -ge 250 ] || die "只從 ${ISL} 抽出 ${n_ref} 個訊息 ID(下界 250)——
  要嘛解析壞了,要嘛這份參考檔不完整。參考檔不完整時這一關會安靜地變綠。"
log "Default.isl ${n_ref} 句;我們覆蓋 ${n_ours} 句"

fail=0

# ── 1. 我們覆蓋了不存在的 ID(打錯字)────────────────────────────
#
# ISCC 對不存在的訊息 ID **不會報錯**,它安靜地忽略 ——
# 於是那一句永遠是英文,而 .iss 裡看起來明明翻過了。
bogus="$(LC_ALL=C comm -13 "${tmp}/ref.txt" "${tmp}/ours.txt")"
if [ -n "${bogus}" ]; then
  echo "  !! 這些 ID 在 Default.isl 裡不存在(打錯字?ISCC 會安靜地忽略):" >&2
  printf '%s\n' "${bogus}" | sed 's/^/     /' >&2
  fail=1
else
  log "沒有打錯的訊息 ID ✓"
fi

# ── 2. 漏翻的 ─────────────────────────────────────────────────────
missing=""
while IFS= read -r id; do
  [ -z "${id}" ] && continue
  is_skipped "${id}" && continue
  missing="${missing}${id}"$'\n'
done < <(LC_ALL=C comm -23 "${tmp}/ref.txt" "${tmp}/ours.txt")
missing="$(printf '%s' "${missing}" | sed '/^$/d')"
if [ -n "${missing}" ]; then
  n="$(printf '%s\n' "${missing}" | wc -l | tr -d ' ')"
  echo "  !! 還有 ${n} 句沒有翻:" >&2
  printf '%s\n' "${missing}" | sed 's/^/     /' >&2
  echo "     每一句都要嘛翻掉、要嘛加進本腳本的 SKIP_IDS **並寫下理由**。" >&2
  echo "     原文可以在 ${ISL} 裡查。" >&2
  fail=1
else
  log "Default.isl 的每一句都翻過了(略過 ${#SKIP_IDS[@]} 句 Inno 自己就留空的)✓"
fi

# ── 3. 佔位符對不對 ───────────────────────────────────────────────
#
# 分成兩種,規則不一樣,而且差別是有理由的:
#
#   **帶資料的**(%1..%9、[name]、[gb]…)—— 必須**一模一樣**。
#     少一個 → 對話框裡缺了檔名 / 產品名 / 數字,使用者看到一句沒有主詞的話。
#     多一個 → 畫面上出現一個沒有意義的 "%2"。
#     比對的是多重集合(排序後比對),所以**調換順序是允許的** ——
#     那在中文裡常常是必要的。
#
#   **換行 %n** —— 只要求**不少於**原文。
#     Inno 自己的翻譯守則說不要增減 %n,那是為了讓各語言版面一致。
#     但我們有一句是刻意寫長的:解除安裝完的重新啟動提示要回答
#     「為什麼 / 不重啟會怎樣 / 可不可以晚點」三件事(見 .iss)。
#     少掉 %n 會把兩句話黏在一起(真的壞掉),多幾個只是分段更清楚。
#     所以這裡擋的是「翻譯時弄丟了換行」,不是「排版與英文不同」。
# ⚠ 兩個 `|| true` 不是裝飾。沒有佔位符的句子(大多數)會讓 grep 以 1 結束,
#   配上 `set -o pipefail` 就是**整支腳本在這裡當場死掉** ——
#   而它死在「已經印完前兩項綠字」之後,看起來像檢查跑完了。
#   實測第一次就是這樣:輸出停在「每一句都翻過了 ✓」,佔位符那一項
#   一次都沒跑到,而結束碼是 0。
# ⚠ 比對的是**去重之後的集合**,不是多重集合。
#
#   要擋的是「弄丟了 %1」與「憑空多一個 %2」—— 那兩件事會讓對話框缺一段
#   或多出一個沒有意義的符號。
#   **重複出現則是允許的**:中文句子常常需要把產品名講兩次
#   (例:「[name] 已安裝完成…請在清單裡選擇 [name]」),而英文原句
#   只寫一次。硬性要求次數一樣,等於強迫譯文照英文的句法走。
#
#   ⚠ 但**新增一種**原文沒有的佔位符仍然是錯的,即使那一種看起來無害。
#     %1..%9 很明顯:原文只給一個參數,寫 %2 就是畫出垃圾。
#     [name] 這一類看起來安全,但「安不安全」取決於那一句在哪一頁被畫出來
#     ([gb] / [mb] 只在特定頁面有值),而那不是譯者判斷得了的事。
#     所以規則一律是「種類必須一樣」——實測就擋下過一次:
#     我想在 WelcomeLabel2(原文只有 [name/ver])裡多寫一個 [name],
#     被這一條擋下來,改成用代名詞。
data_tokens() {
  printf '%s' "$1" | { grep -oE '%[0-9]|\[[a-z/]+\]' || true; } \
    | LC_ALL=C sort -u | tr '\n' ' '
}
# ⚠ 用 awk 數,不要用 `grep -c`:**grep -c 在數到 0 的時候結束碼是 1**,
#   配上 set -e 一樣會讓整支腳本安靜地死掉(而且大多數句子就是 0 個 %n)。
count_n() { printf '%s' "$1" | awk '{ n += gsub(/%n/, "") } END { print n + 0 }'; }

extract_kv "${ISL}" Messages > "${tmp}/ref.kv"
extract_kv "${ISS}" Messages > "${tmp}/ours.kv"
bad_tokens=0
while IFS=$'\t' read -r id ours_val; do
  [ -z "${id}" ] && continue
  ref_val="$(awk -F'\t' -v k="${id}" '$1 == k { sub(/^[^\t]*\t/, ""); print; exit }' "${tmp}/ref.kv")"
  # 上面第 1 項已經報過不存在的 ID,這裡不重複。
  [ -z "${ref_val}" ] && continue
  a="$(data_tokens "${ref_val}")"
  b="$(data_tokens "${ours_val}")"
  if [ "${a}" != "${b}" ]; then
    echo "  !! ${id} 用到的佔位符種類與原文不同" >&2
    echo "     原文: ${a:-(無)}" >&2
    echo "     我們: ${b:-(無)}" >&2
    bad_tokens=$((bad_tokens + 1))
    fail=1
  fi
  na="$(count_n "${ref_val}")"
  nb="$(count_n "${ours_val}")"
  if [ "${nb}" -lt "${na}" ]; then
    echo "  !! ${id} 的換行變少了(原文 ${na} 個 %n,我們 ${nb} 個)" >&2
    echo "     少掉的 %n 會把兩句話黏在一起。" >&2
    bad_tokens=$((bad_tokens + 1))
    fail=1
  fi
done < "${tmp}/ours.kv"
[ "${bad_tokens}" -eq 0 ] \
  && log "每一句的 %1 / [name] 都與原文一致,而且沒有弄丟換行 ✓"

# ── 4. 訊息值裡不可以有 Markdown ──────────────────────────────────
#
# 這些字串是**直接畫在對話框上**的。`**強調**` 在使用者眼裡就是四個星號,
# 而這份 .iss 的註解裡到處都是那種寫法 —— 寫著寫著就會滑進訊息值裡。
# (實測:第一版的 UninstalledAndNeedsRestart 與 UninstallAskPurge
#  就各帶了一組。)
#
# 兩個區段都查:[Messages] 與 [CustomMessages] 都會被畫出來。
md=0
for sec in Messages CustomMessages; do
  while IFS=$'\t' read -r id val; do
    [ -z "${id}" ] && continue
    case "${val}" in
      *'**'*)
        echo "  !! ${id} 的值裡有 Markdown 的 ** —— 對話框上會顯示成星號" >&2
        md=1; fail=1 ;;
    esac
  done < <(extract_kv "${ISS}" "${sec}")
done
[ "${md}" -eq 0 ] && log "訊息值裡沒有 Markdown ✓"

# ── 反向測試 ──────────────────────────────────────────────────────
#
# 一支只會印綠字的檢查比沒有更糟。這裡拿一份動過手腳的 .iss 跑同一段邏輯,
# 要求它真的紅。
if [ "${SELF_CHECK}" -eq 1 ]; then
  log "反向測試"
  probe="${tmp}/probe.iss"

  # (a) 刪掉一句 → 必須報漏翻
  grep -v '^ButtonYes=' "${ISS}" > "${probe}"
  if ISS_OVERRIDE="${probe}" REFERENCE_ISL="${ISL}" \
     bash "${BASH_SOURCE[0]}" >/dev/null 2>&1; then
    die "刪掉一句翻譯,檢查竟然還通過"
  fi
  log "  ✓ 少一句會紅"

  # (b) 植入一組 Markdown → 必須報 Markdown
  sed 's/^ButtonOK=.*/ButtonOK=**確定**/' "${ISS}" > "${probe}"
  if ISS_OVERRIDE="${probe}" REFERENCE_ISL="${ISL}" \
     bash "${BASH_SOURCE[0]}" >/dev/null 2>&1; then
    die "訊息值裡植入了 Markdown,檢查竟然還通過"
  fi
  log "  ✓ 訊息值裡有 ** 會紅"

  # (c) 參考檔本身就是**對照答案** → 所以它必須釘 sha256
  #
  # 把同一句從 .iss 與參考檔裡**一起**刪掉:上面 (a) 是紅的,這裡必須變綠。
  # 那正是「參考檔少一句 = 那一句永遠不算漏翻」的證據 —— 也就是為什麼
  # 版控裡那一份要驗 sha256,而不是隨便一個 /tmp 裡的檔案都可以。
  probe_isl="${tmp}/probe.isl"
  grep -v '^ButtonYes=' "${ISL}" > "${probe_isl}"
  grep -v '^ButtonYes=' "${ISS}" > "${probe}"
  if ISS_OVERRIDE="${probe}" REFERENCE_ISL="${probe_isl}" \
     bash "${BASH_SOURCE[0]}" >/dev/null 2>&1; then
    log "  ✓ 參考檔裡也刪掉同一句,就不再算漏翻(所以那一份要釘 sha256)"
  else
    die "把參考檔與 .iss 裡的同一句一起刪掉,這一關竟然還是紅的 ——
  這個反向測試沒有測到它想測的東西,參考檔的角色可能已經不是對照答案了。"
  fi
  echo "  (註:上面那三次都是用改過的複本跑的,不影響版控裡那一份)"
fi

echo
[ "${fail}" -eq 0 ] || die "安裝程式的訊息還沒有全部在地化,見上。"
log "安裝程式的訊息檢查全部通過 ✓"
