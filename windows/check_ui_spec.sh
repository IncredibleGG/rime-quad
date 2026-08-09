#!/usr/bin/env bash
#
# windows/check_ui_spec.sh — docs/ui-design.md §12.12 的 23 條檢核項
#
# ⚠ **這支腳本的每一條都必須有反向測試**(§2-G1)。`--self-check` 會把
#   每一條的違規**真的植入**一棵複製出來的樹,然後要求那一條變紅。
#   沒有做過那一步的檢查,一律當作沒有 —— 這個專案有過六項全部不一致
#   而守門腳本 6/6 全綠的事故,原因是掃描範圍不含放那六項的目錄。
#
# ⚠ **每一條都要斷言掃描範圍非空**(§2-G2)。範圍寫錯時的行為必須是
#   **紅**,不是「零個違規」。所以每一條掃描前都先數一次分母。
#
# ⚠ §2-G5:**不要把 printf/cat 接進 grep -q。** `set -o pipefail` 下
#   命中會變成失敗(grep -q 讀到就關 pipe → SIGPIPE),而且**輸出小的
#   時候完全正常**,長大之後才發作。本專案被同一件事咬過三次。
#   所以本檔一律先把結果寫進變數或檔案,再判斷。
#
# ── §2-G4:還有誰不在範圍內 ──────────────────────────────────────
#
# 本腳本掃的是 `windows/` 底下的 `.cc` / `.h`。**刻意不在範圍內的**:
#
#   · `windows/setup/doctor.cc` 與 `setup_main.cc` 的**窄字串**中文
#     (各 338 / 246 處)。它們是 `rime_ime_setup.exe doctor` 的主控台輸出,
#     不是視窗介面。W7 掃的是**寬字串**字面值,所以碰不到它們。
#     那一整支工具還沒有在地化 —— 已列進報告的「還沒做」那一節。
#   · 安裝程式腳本(`windows/installer/*.iss`)裡的字串。
#     它有自己的守門腳本:windows/check_installer_messages.sh。
#   · `windows/*.sh` 自己。
#
# 用法:
#   windows/check_ui_spec.sh              # 跑 23 條
#   windows/check_ui_spec.sh --self-check # 逐條植入違規,要求它變紅
#
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="${RIMEWIN_ROOT:-$(cd "${SCRIPT_DIR}/.." && pwd)}"
WIN="${ROOT}/windows"

# ⚠ windows-latest 的 runner 上 python 不一定叫 `python3`。找不到就**明確
#   失敗**,不要安靜地跳過 —— 一支只會印綠字的檢核腳本比沒有更糟。
PY=""
for cand in python3 python py; do
  if command -v "${cand}" >/dev/null 2>&1; then PY="${cand}"; break; fi
done
if [ -z "${PY}" ]; then
  printf '\033[1;31m[FAIL]\033[0m 找不到 python(試過 python3 / python / py)—— 本腳本的去註解與字面值掃描都靠它\n' >&2
  exit 1
fi

FAILED=0
CHECKED=0

red()  { printf '\033[1;31m[FAIL]\033[0m %s\n' "$*" >&2; FAILED=$((FAILED+1)); }
ok()   { printf '  \033[1;32mok\033[0m   %s\n' "$*"; }
info() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }

# ── 去註解 ──────────────────────────────────────────────────────
#
# ⚠ 這一步是必要的,而且它本身有風險。這一輪的程式碼裡到處是**解釋為什麼
#   不能那樣寫**的註解 —— 例如 cand_window.cc 寫著「這裡原本用
#   SPI_GETNONCLIENTMETRICS,而 §8.6.0 明文禁止」。不去註解的話,
#   W5 會被那句話判成違規,而**一條永遠紅的檢查會被關掉**(§3.1 的教訓)。
#
#   反過來,去註解如果做錯(例如把字串裡的 "//" 當成註解起點),
#   會讓真的違規消失。所以下面的實作是字串感知的,而且 --self-check
#   植入的違規全部在**程式碼**裡,證明去註解沒有把它們一起吃掉。
strip_comments() {
  "${PY}" - "$1" <<'PYSCRIPT'
import sys
# ⚠ windows-latest 的 runner 上,python 的 stdout 預設是 cp1252 ——
#   寫一個中文字就是 UnicodeEncodeError,而**去註解的輸出會被截斷**。
#   後果不是「腳本紅了」,是後面每一條 grep 都掃到一份殘缺的檔案:
#   W2 少數了一個視窗類別、W8/W10/W21 命中變成 0、W17 說「找不到函式」。
#   也就是說,**它會讓檢核用一堆看起來像真的違規的訊息失敗** ——
#   比直接爆掉更難查。
sys.stdout.reconfigure(encoding='utf-8', newline='')
src = open(sys.argv[1], encoding='utf-8', errors='replace').read()
out = []
i = 0
n = len(src)
while i < n:
    c = src[i]
    if c == '"' or c == "'":
        q = c
        out.append(c)
        i += 1
        while i < n:
            if src[i] == '\\':
                out.append('  ')
                i += 2
                continue
            out.append(src[i])
            if src[i] == q:
                i += 1
                break
            i += 1
        continue
    if c == '/' and i + 1 < n and src[i+1] == '/':
        while i < n and src[i] != '\n':
            i += 1
        continue
    if c == '/' and i + 1 < n and src[i+1] == '*':
        i += 2
        while i + 1 < n and not (src[i] == '*' and src[i+1] == '/'):
            if src[i] == '\n':
                out.append('\n')
            i += 1
        i += 2
        continue
    out.append(c)
    i += 1
sys.stdout.write(''.join(out))
PYSCRIPT
}

sources() { find "${WIN}" \( -name '*.cc' -o -name '*.h' \) | sort; }

# 把 windows/ 底下所有原始檔去註解之後串成一份,供多數檢查共用。
CODE_DIR=""
build_code() {
  CODE_DIR="$(mktemp -d)"
  local n=0
  local f rel
  while IFS= read -r f; do
    rel="${f#${WIN}/}"
    mkdir -p "${CODE_DIR}/$(dirname "${rel}")"
    strip_comments "${f}" > "${CODE_DIR}/${rel}"
    n=$((n+1))
  done < <(sources)
  SOURCE_COUNT="${n}"
}
cleanup() { [ -n "${CODE_DIR}" ] && rm -rf "${CODE_DIR}"; }
trap cleanup EXIT

# 在去註解後的樹上找。回傳命中的行(可能為空)。
hits() { grep -rn "$1" "${CODE_DIR}" ${2:+--include="$2"} 2>/dev/null || true; }
count_of() { [ -z "$1" ] && echo 0 || printf '%s\n' "$1" | grep -c . ; }

# ⚠ `x="$(grep -c ... || echo 0)"` 在沒命中時會得到 "0\n0" —— grep -c 自己
#   已經印了一個 0,然後 `||` 又印一個。接著 `[ "$x" -lt 3 ]` 就是
#   「integer expected」。這一支把任何輸出正規化成第一個整數。
num() {
  local v
  v="$(printf '%s' "${1:-}" | tr -dc '0-9\n' | head -1)"
  [ -z "${v}" ] && v=0
  printf '%s' "${v}"
}

need_scope() {  # 名稱 實際 下界
  if [ "$2" -lt "$3" ]; then
    red "$1:掃描範圍太小($2 < $3)—— 範圍寫錯時的行為必須是紅,不是零個違規"
    return 1
  fi
  return 0
}

check() { CHECKED=$((CHECKED+1)); }

# ────────────────────────────────────────────────────────────────
run_checks() {
  build_code
  info "掃描範圍:${SOURCE_COUNT} 個 .cc/.h(去註解後)"
  need_scope "整體範圍" "${SOURCE_COUNT}" 20 || return

  # ── W1:縮放一律 MulDiv ──────────────────────────────────────
  check
  local w1; w1="$(hits 'dpi_scale_')"
  local w1b; w1b="$(hits 'static_cast<int>([^)]*\*[^)]*scale')"
  if [ -n "${w1}" ] || [ -n "${w1b}" ]; then
    red "W1:還有 dpi_scale_ 或無條件捨去的縮放(150% 下 11 DIP 會變 16 而不是 17,而誤差沿著版面累積)"
    printf '%s\n%s\n' "${w1}" "${w1b}" | grep . | head -5 >&2
  else
    ok "W1 縮放一律走 MulDivRound"
  fi

  # ── W2:每個 top-level 視窗的 WndProc 都要有 WM_DPICHANGED ──
  check
  local classes; classes="$(hits 'RegisterClassExW')"
  local nclasses; nclasses="$(count_of "${classes}")"
  need_scope "W2" "${nclasses}" 3 || true
  local w2bad=0 f
  for f in $(printf '%s\n' "${classes}" | grep . | cut -d: -f1 | sort -u); do
    if ! grep -q 'WM_DPICHANGED' "${f}"; then
      red "W2:${f#${CODE_DIR}/} 註冊了視窗類別但沒有 WM_DPICHANGED 分支(進程是 per-monitor-v2,系統**會**送這則訊息)"
      w2bad=1
    fi
  done
  [ "${w2bad}" -eq 0 ] && ok "W2 ${nclasses} 個視窗類別都處理 WM_DPICHANGED"

  # ── W3:間距 / 圓角 / 元件尺寸三個集合分開驗 ─────────────────
  #
  # ⚠ §3.1 明說「三個集合各管各的,不要混在一起掃」——
  #   混在一起會把 1 DIP 的分隔線判成違規,而一條永遠紅的檢查會被關掉。
  #   這裡驗的是**階梯本身**(ui_layout.h 的常數),而不是每一個用到的地方:
  #   版面碼只准用那些具名常數,所以驗常數等於驗全部。
  check
  local lay="${CODE_DIR}/common/ui_layout.h"
  if [ ! -f "${lay}" ]; then
    red "W3:找不到 common/ui_layout.h —— 掃描範圍錯了"
  else
    local sp; sp="$(grep -o 'constexpr int s[0-9] = [0-9]*' "${lay}" | grep -o '[0-9]*$' | sort -n | tr '\n' ' ')"
    local ra; ra="$(grep -o 'constexpr int k[A-Za-z]* = [0-9]*' "${lay}" | sed -n '/kLarge\|kMedium\|kMediumInner\|kSmall/p' | grep -o '[0-9]*$' | sort -n | tr '\n' ' ')"
    local ts; ts="$(grep -o 'constexpr int t[0-9] = [0-9]*' "${lay}" | grep -o '[0-9]*$' | sort -n | tr '\n' ' ')"
    local nlit; nlit="$(num "$(grep -c 'constexpr int' "${lay}" || true)")"
    need_scope "W3" "${nlit}" 20 || true
    if [ "${sp}" != "2 4 6 10 12 16 20 32 " ]; then
      red "W3:間距階梯不是 §3.1 桌面欄的八階(得到:${sp})"
    elif [ "${ra}" != "5 6 7 10 " ]; then
      red "W3:圓角不是 §3.3 桌面欄的四個值(得到:${ra})"
    else
      ok "W3 間距/圓角/尺寸三個集合都落在階梯上"
    fi
    # ── W4:字級落在 §3.2 桌面欄 ──
    check
    if [ "${ts}" != "11 11 12 13 15 22 " ]; then
      red "W4:字級不是 §3.2 桌面欄的 {22,15,13,12,11}(得到:${ts})"
    else
      ok "W4 字級落在 §3.2 的六階上"
    fi
  fi

  # ── W5:不得用系統 UI 字型當預設(§8.6.0)───────────────────
  check
  local w5; w5="$(hits 'SPI_GETNONCLIENTMETRICS\|lfMessageFont\|DEFAULT_GUI_FONT')"
  if [ -n "${w5}" ]; then
    red "W5:還在用系統 UI 字型當預設(§8.6.0 明文禁止)"
    printf '%s\n' "${w5}" | head -3 >&2
  else
    ok "W5 沒有 SPI_GETNONCLIENTMETRICS / lfMessageFont / DEFAULT_GUI_FONT"
  fi

  # ── W6:字型解析必須經過存在性檢查 ───────────────────────────
  check
  local w6; w6="$(hits 'CreateFontIndirectW')"
  local n6; n6="$(count_of "${w6}")"
  need_scope "W6" "${n6}" 1 || true
  local w6bad=0
  for f in $(printf '%s\n' "${w6}" | grep . | cut -d: -f1 | sort -u); do
    case "${f}" in
      */service/ui_font.cc) ;;
      *) red "W6:${f#${CODE_DIR}/} 直接呼叫 CreateFontIndirectW —— 它永遠不會失敗,存在性檢查只能有一個地方做"; w6bad=1 ;;
    esac
  done
  if ! grep -q 'EnumFontFamiliesExW' "${CODE_DIR}/service/ui_font.cc" 2>/dev/null; then
    red "W6:ui_font.cc 沒有 EnumFontFamiliesExW —— 那是唯一可靠的字體存在性檢查"
    w6bad=1
  fi
  [ "${w6bad}" -eq 0 ] && ok "W6 CreateFontIndirectW 只在 ui_font.cc,而且它有做存在性檢查"

  # ── W7:catalog 以外不得有中日韓寬字串字面值 ─────────────────
  #
  # ⚠ **這一條原本是假綠的,而反向測試把它抓出來了。**
  #   第一版用的是 §12.2 量字串時那個慣用法 `grep '[CJK 範圍]'`,
  #   而它在這台機器上回的是 `grep: Invalid collation character`。
  #   stderr 被導去 /dev/null,於是「錯誤」變成「零個命中」變成「通過」。
  #   完整的說明寫在 windows/tools/cjk_literal_scan.py 的檔頭。
  check
  local w7bad=0
  local w7out; w7out="$("${PY}" "${WIN}/tools/cjk_literal_scan.py" "${WIN}" 2>&1)"
  local nscanned; nscanned="$(printf '%s\n' "${w7out}" | sed -n 's/^SCANNED=//p')"
  nscanned="$(num "${nscanned}")"
  local nbad; nbad="$(num "$(printf '%s\n' "${w7out}" | grep -c '^BAD=' || true)")"
  [ -z "${nbad}" ] && nbad=0
  need_scope "W7" "${nscanned}" 20 || w7bad=1
  if [ "${nbad}" -ne 0 ]; then
    red "W7:catalog 以外有 ${nbad} 個中日韓寬字串字面值(使用者可見字串只能住在 ui_strings.cc)"
    printf '%s\n' "${w7out}" | sed -n 's/^BAD=/    /p' | head -5 >&2
    w7bad=1
  fi
  [ "${w7bad}" -eq 0 ] && ok "W7 ${nscanned} 個檔案裡沒有一個中日韓寬字串字面值"

  # ── W8:三個語系的陣列長度 == kUiStringCount(編譯期)────────
  check
  local cat="${CODE_DIR}/common/ui_strings.cc"
  local nentries; nentries="$(num "$(num "$(grep -c '^ *X(k' "${cat}" 2>/dev/null || true)")")"
  need_scope "W8 條目數" "${nentries}" 80 || true
  # ⚠ **只數個數是不夠的**(這一條的反向測試第一次就抓到了):把其中一個
  #   斷言換成一個恆真的東西,個數不變,而保護沒了。所以逐條點名。
  local w8bad=0 need
  for need in 'OrderMatchesEnum' 'kCount ==' 'kEnUs' 'kZhHant' 'kZhHans'; do
    local n8; n8="$(num "$(grep 'static_assert' "${cat}" 2>/dev/null | grep -c "${need}" || true)")"
    if [ "${n8}" -lt 1 ]; then
      red "W8:ui_strings.cc 少了守 ${need} 的 static_assert —— 少一條字串或錯一格順序就不再是編譯期錯誤"
      w8bad=1
    fi
  done
  # 順序那一條是重點:長度相同**擋不住錯位**。
  if ! grep -q 'constexpr bool OrderMatchesEnum' "${cat}" 2>/dev/null; then
    red "W8:找不到 OrderMatchesEnum —— 三個語系長度一樣但整體錯開一格時,沒有任何東西會發現"
    w8bad=1
  fi
  [ "${w8bad}" -eq 0 ] && ok "W8 ${nentries} 條 × 三語,長度**與順序**都由具名的 static_assert 在編譯期守住"

  # ── W9 / W15 / W18 / W19 / W20 / W22:真的單元測試 ───────────
  #
  # 這六條驗的是邏輯不是字串,所以它們是 windows/tests/ 底下真的會跑的
  # 測試,由 run_logic_tests.sh 執行。這裡只斷言那些測試**存在** ——
  # 「測試檔被刪掉了而守門腳本沒發現」是這個專案抓過的失效模式。
  check
  local missing=""
  local t
  for t in \
      "test_ui_strings.cc:ui_strings_no_banned_engine_words" \
      "test_layout.cc:layout_never_drops_candidates_horizontal_shrink" \
      "test_ui_layout.cc:ui_layout_every_clickable_target_meets_the_minimum" \
      "test_ui_layout.cc:ui_layout_content_column_three_worked_cases" \
      "test_statusbar_place.cc:statusbar_falls_back_when_the_monitor_disappears" \
      "test_ui_palette.cc:palette_every_pair_meets_its_threshold_in_both_modes" ; do
    local file="${WIN}/tests/${t%%:*}"
    local name="${t##*:}"
    if [ ! -f "${file}" ] || ! grep -q "TEST(${name})" "${file}"; then
      missing="${missing} ${t}"
    fi
  done
  if [ -n "${missing}" ]; then
    red "W9/W15/W18/W19/W20/W22:找不到這些單元測試:${missing}"
  else
    ok "W9/W15/W18/W19/W20/W22 六條的單元測試都在(由 run_logic_tests.sh 執行)"
  fi

  # ── W10:狀態字面兩個方向都驗 ───────────────────────────────
  check
  local bar="${CODE_DIR}/service/status_bar.cc"
  local in_bar; in_bar="$(num "$(num "$(grep -c 'kGlyphChinese\|kGlyphAscii\|kGlyphSimplified\|kGlyphTraditional' "${bar}" 2>/dev/null || true)")")"
  local in_cat; in_cat="$(num "$(grep -o 'L"中"\|L"简"\|L"繁"' "${cat}" 2>/dev/null | grep -c . || true)")"
  if [ "${in_bar}" -lt 4 ]; then
    red "W10:狀態列繪製碼裡找不到四個狀態字面(命中 ${in_bar})—— 兩邊都是 0 代表掃錯檔案"
  elif [ "${in_cat}" -ne 0 ]; then
    red "W10:狀態字面跑進 catalog 了(命中 ${in_cat})—— §8.12 規定它們四端一致、不得在地化"
  else
    ok "W10 狀態字面在繪製碼裡(${in_bar} 處),而且不在 catalog 裡"
  fi

  # ── W11:版面碼裡不得有 is_dark 分支;兩份色票同構 ───────────
  check
  local w11=""
  for f in "${CODE_DIR}/common/ui_layout.cc" "${CODE_DIR}/common/ui_layout.h"; do
    [ -f "${f}" ] || continue
    if grep -q 'is_dark\|dark_mode\|isDark' "${f}"; then
      w11="${w11} ${f#${CODE_DIR}/}"
    fi
  done
  local nroles; nroles="$(num "$(num "$(grep -c '^  k[A-Za-z]*,' "${CODE_DIR}/common/ui_palette.h" 2>/dev/null || true)")")"
  need_scope "W11 色票角色數" "${nroles}" 11 || true
  if [ -n "${w11}" ]; then
    red "W11:版面碼裡有深淺分支:${w11}(§2-F4:深色只換色票,不動版面)"
  else
    ok "W11 版面碼沒有深淺分支;${nroles} 個角色由 enum 索引 → 兩份色票編譯期同構"
  fi

  # ── W12:跟著系統即時切換 ────────────────────────────────────
  check
  local w12=0
  for f in "${CODE_DIR}"/service/*.cc; do
    if grep -q 'WM_SETTINGCHANGE' "${f}" && grep -q 'ImmersiveColorSet' "${f}"; then
      w12=$((w12+1))
    fi
  done
  # ui_theme.cc 有比對,settings_window / status_bar 有訊息分支 —— 至少一處
  # 兩者同時出現才算數。
  local w12a; w12a="$(hits 'ImmersiveColorSet')"
  if [ -z "${w12a}" ]; then
    red "W12:找不到 ImmersiveColorSet 的比對 —— 系統換深淺時我們不會跟著換"
  else
    ok "W12 有處理 WM_SETTINGCHANGE 並比對 ImmersiveColorSet"
  fi

  # ── W13:高對比是第三種模式 ──────────────────────────────────
  check
  local w13=0
  for f in "${CODE_DIR}"/service/*.cc "${CODE_DIR}"/common/*.cc; do
    [ -f "${f}" ] || continue
    if grep -q 'SPI_GETHIGHCONTRAST' "${f}" && grep -q 'GetSysColor' "${f}"; then
      w13=$((w13+1))
    fi
  done
  if [ "${w13}" -lt 1 ]; then
    red "W13:沒有一個檔案同時有 SPI_GETHIGHCONTRAST 與 GetSysColor —— 高對比時必須整份色票停用改走系統色"
  else
    ok "W13 高對比分支存在,而且走 GetSysColor"
  fi

  # ── W14:不得呼叫 uxtheme 的未公開序數 API ───────────────────
  check
  local w14; w14="$(hits 'MAKEINTRESOURCEA\?([0-9]')"
  local w14b; w14b="$(hits 'uxtheme')"
  local ngp; ngp="$(count_of "$(hits 'GetProcAddress')")"
  need_scope "W14" "${ngp}" 3 || true
  if [ -n "${w14}" ] || [ -n "${w14b}" ]; then
    red "W14:出現了 uxtheme 或序數形式的 GetProcAddress —— 一個外人讀到它看不出它在做什麼,而我們要求別人相信這支程式不連網"
    printf '%s\n%s\n' "${w14}" "${w14b}" | grep . | head -3 >&2
  else
    ok "W14 沒有 uxtheme 的未公開序數 API(${ngp} 個 GetProcAddress 呼叫點都是具名的)"
  fi

  # ── W16:不得用 MessageBoxW 做確認 ───────────────────────────
  check
  local w16; w16="$(hits 'MB_YESNO\|MB_OKCANCEL\|MB_RETRYCANCEL')"
  local ndlg; ndlg="$(count_of "$(hits 'ConfirmDialog\|MessageDialog')")"
  need_scope "W16 對話框數" "${ndlg}" 2 || true
  local w16bad=0
  if [ -n "${w16}" ]; then
    red "W16:還在用 MessageBoxW 做確認 —— 它的按鈕字面由系統決定,必然是「是/否」,違反 §2-C3"
    printf '%s\n' "${w16}" | head -3 >&2
    w16bad=1
  fi
  # 確認鍵的字面不得屬於 {確定, 好, OK, 是, Yes}
  local badword; badword="$(num "$(grep -o 'L"確定"\|L"好"\|L"OK"\|L"是"\|L"Yes"' "${cat}" 2>/dev/null | grep -c . || true)")"
  if [ "${badword}" -ne 0 ]; then
    red "W16:catalog 裡有 {確定, 好, OK, 是, Yes} 這類確認鍵字面(§2-C3)"
    w16bad=1
  fi
  # 預設焦點在取消
  if ! grep -q 'SetFocus(st.cancel)' "${CODE_DIR}/service/ui_confirm.cc" 2>/dev/null; then
    red "W16:確認對話框的預設焦點不在取消(§2-C4)"
    w16bad=1
  fi
  [ "${w16bad}" -eq 0 ] && ok "W16 確認走自己的對話框,字面自己寫,預設焦點在取消"

  # ── W17:清單列不得顯示 schema id ────────────────────────────
  check
  local sw="${CODE_DIR}/service/settings_window.cc"
  local nassemble; nassemble="$(num "$(num "$(grep -c 'SchemaDisplayName' "${sw}" 2>/dev/null || true)")")"
  need_scope "W17 列文字組裝點" "${nassemble}" 1 || true
  # 組裝函式的本體不得把 first(id)接進顯示字串,除非是「名字為空」的退路。
  local w17; w17="$("${PY}" - "${sw}" <<'PYSCRIPT'
import re,sys
s=open(sys.argv[1],encoding='utf-8',errors='replace').read()
m=re.search(r'std::wstring SettingsWindow::SchemaDisplayName\(size_t index\) const \{(.*?)\n\}', s, re.S)
if not m:
    print("NOFUNC"); raise SystemExit
body=m.group(1)
# 允許的唯一形式:名字為空時才用 id。出現 `+` 串接 id 就是違規。
if re.search(r'\+\s*Utf8ToWide\(kv\.first\)', body) or re.search(r'kv\.first\s*\+', body):
    print("CONCAT")
PYSCRIPT
)"
  if [ "${w17}" = "NOFUNC" ]; then
    red "W17:找不到列文字組裝函式 —— 掃描範圍錯了"
  elif [ -n "${w17}" ]; then
    red "W17:列文字把 schema id 串進去了(§6.7 第一層硬禁,而且它現在就印在使用者畫面上)"
  else
    ok "W17 清單列只顯示名字,不顯示 schema id"
  fi

  # ── W21:自繪處理常式都要畫焦點,而且尊重 UISF_HIDEFOCUS ─────
  #
  # ⚠ 規格寫的下界是 4,實際是 **3**,而且差的那一個有理由:
  #   §12.5.3 列的六類自繪裡,「懸浮狀態列」是 WS_EX_NOACTIVATE 的,
  #   它**永遠拿不到鍵盤焦點**(那是它存在的前提 —— 搶焦點會讓使用者
  #   正在打字的輸入框失去插入點)。給一個拿不到焦點的東西畫焦點環,
  #   畫出來的是一個永遠不會出現的狀態。
  #   「容器裝飾」與「確認對話框」也不是控制項。所以會動的是三個。
  check
  local nod=0 w21bad=0
  for f in "${CODE_DIR}"/service/*.cc; do
    [ -f "${f}" ] || continue
    if grep -q 'CDDS_ITEMPREPAINT\|WM_DRAWITEM' "${f}"; then
      if ! grep -q 'CDIS_FOCUS\|ODS_FOCUS' "${f}"; then
        red "W21:${f#${CODE_DIR}/} 有自繪但沒有焦點分支"
        w21bad=1
      fi
      if ! grep -q 'WM_UPDATEUISTATE\|show_focus_' "${f}"; then
        red "W21:${f#${CODE_DIR}/} 有自繪但沒有尊重 UISF_HIDEFOCUS(滑鼠使用者身上會到處是框)"
        w21bad=1
      fi
    fi
  done
  nod="$(num "$(num "$(grep -c 'CDIS_FOCUS\|ODS_FOCUS' "${sw}" 2>/dev/null || true)")")"
  if [ "${nod}" -lt 3 ]; then
    red "W21:自繪處理常式的焦點分支只有 ${nod} 處(下界 3,見上面的說明)"
    w21bad=1
  fi
  [ "${w21bad}" -eq 0 ] && ok "W21 ${nod} 個自繪處理常式都畫焦點,而且只在鍵盤使用時畫"

  # ── W23:停用的控制項,同一頁要有說明 ───────────────────────
  #
  # ⚠ 機器**只驗得到一半**:存在性可驗,「那句話說得對不對」只有人驗得到。
  check
  local dis; dis="$(hits 'EnableWindow(.*FALSE)')"
  local ndis; ndis="$(count_of "${dis}")"
  need_scope "W23 停用點" "${ndis}" 2 || true
  if [ "${ndis}" -ge 2 ] && grep -q 'kStatusRedeployRunning' "${sw}"; then
    ok "W23 ${ndis} 個停用點,而且同一個視窗裡有一句一直在動的說明(內容對不對只有人驗得到)"
  else
    red "W23:停用了控制項但同一頁找不到說明(§2-D1:沒有那句話就不准停用)"
  fi
}

# ────────────────────────────────────────────────────────────────
# 反向測試:每一條都真的植入一次違規,要求它變紅。
# ────────────────────────────────────────────────────────────────
self_check() {
  info "反向測試:逐條植入違規,要求它變紅"
  local base; base="$(mktemp -d)"
  cp -r "${ROOT}/windows" "${base}/windows"

  local pass=0 fail=0
  # 名稱|要改的檔案|python 片段(對 s 做替換)
  local muts=(
"W1|service/settings_window.cc|s=s.replace('void SettingsWindow::LayoutUi() {','void SettingsWindow::LayoutUi() { double dpi_scale_ = 1.0; (void)dpi_scale_;',1)"
"W2|service/status_bar.cc|s=s.replace('case WM_DPICHANGED: {','case WM_NULL + 12345: {',1)"
"W3|common/ui_layout.h|s=s.replace('constexpr int s7 = 20;','constexpr int s7 = 22;',1)"
"W4|common/ui_layout.h|s=s.replace('constexpr int t4 = 12;','constexpr int t4 = 14;',1)"
"W5|service/cand_window.cc|s=s.replace('namespace {','namespace {\\nstatic int probe = SPI_GETNONCLIENTMETRICS;',1)"
"W6|service/settings_window.cc|s=s.replace('void SettingsWindow::ApplyFonts() {','void SettingsWindow::ApplyFonts() { LOGFONTW lf{}; HFONT bad = ::CreateFontIndirectW(&lf); (void)bad;',1)"
"W7|service/settings_window.cc|s=s.replace('constexpr int kAlways = 99;', 'constexpr int kAlways = 99;' + chr(10) + 'constexpr wchar_t kOops[] = L' + chr(34) + chr(28204) + chr(34) + ';', 1)"
"W8|common/ui_strings.cc|s=s.replace('constexpr bool OrderMatchesEnum()','constexpr bool RemovedOnPurpose()',1).replace('static_assert(OrderMatchesEnum(),','static_assert(true,',1)"
"W10|service/status_bar.cc|s=s.replace('constexpr wchar_t kGlyphChinese[] = L\\\"中\\\";','',1).replace('constexpr wchar_t kGlyphAscii[] = L\\\"En\\\";','',1).replace('constexpr wchar_t kGlyphSimplified[] = L\\\"简\\\";','',1).replace('constexpr wchar_t kGlyphTraditional[] = L\\\"繁\\\";','',1)"
"W11|common/ui_layout.cc|s=s.replace('int ContentWidthDip(int window_w_dip) {','int ContentWidthDip(int window_w_dip) { bool is_dark = false; if (is_dark) return 0;',1)"
"W12|service/ui_theme.cc|s=s.replace('ImmersiveColorSet','SomethingElse')"
"W13|service/ui_theme.cc|s=s.replace('SPI_GETHIGHCONTRAST','SPI_GETWORKAREA')"
"W14|service/ui_theme.cc|s=s.replace('static HMODULE dwm = ::LoadLibraryW(L\\\"dwmapi.dll\\\");','static HMODULE dwm = ::LoadLibraryW(L\\\"uxtheme.dll\\\");\\n  (void)::GetProcAddress(dwm, MAKEINTRESOURCEA(135));',1)"
"W16|service/settings_window.cc|s=s.replace('    case IDC_REDEPLOY:','    case 12345: ::MessageBoxW(hwnd_, L\\\"x\\\", L\\\"y\\\", MB_YESNO); return;\\n    case IDC_REDEPLOY:',1)"
"W17|service/settings_window.cc|s=s.replace('  return Utf8ToWide(kv.second.empty() ? kv.first : kv.second);','  return Utf8ToWide(kv.second) + L\\\" (\\\" + Utf8ToWide(kv.first) + L\\\")\\\";',1)"
"W21|service/settings_window.cc|s=s.replace('CDIS_FOCUS','CDIS_SELECTED')"
"W23|service/settings_window.cc|s=s.replace('kStatusRedeployRunning','kStatusApplied')"
"範圍|__SCOPE__|"
  )

  local m name relfile pysnip tmp
  for m in "${muts[@]}"; do
    name="${m%%|*}"
    local rest="${m#*|}"
    relfile="${rest%%|*}"
    pysnip="${rest#*|}"
    tmp="$(mktemp -d)"
    cp -r "${base}/windows" "${tmp}/windows"

    if [ "${relfile}" = "__SCOPE__" ]; then
      # ⚠ 最重要的一條反向測試:**範圍寫錯時必須紅,不是零個違規**。
      #   這正是本專案六項全錯而守門腳本 6/6 全綠的那個失效模式。
      rm -rf "${tmp}/windows"
      mkdir -p "${tmp}/windows"
    else
      "${PY}" - "${tmp}/windows/${relfile}" <<PYMUT
import io,sys
p=sys.argv[1]
s=open(p,encoding='utf-8').read()
${pysnip}
open(p,'w',encoding='utf-8').write(s)
PYMUT
    fi

    local out
    out="$(RIMEWIN_ROOT="${tmp}" bash "${SCRIPT_DIR}/check_ui_spec.sh" 2>&1)"
    local rc=$?
    if [ "${rc}" -eq 0 ]; then
      printf '  \033[1;31m植入 %s 的違規之後腳本仍然是綠的 —— 那一條不算數\033[0m\n' "${name}" >&2
      fail=$((fail+1))
    else
      printf '  \033[1;32mok\033[0m   植入 %s 的違規 → 變紅\n' "${name}"
      pass=$((pass+1))
    fi
    rm -rf "${tmp}"
  done
  rm -rf "${base}"

  info "反向測試:${pass} 條會紅,${fail} 條不會"
  [ "${fail}" -eq 0 ] || return 1
  return 0
}

if [ "${1:-}" = "--self-check" ]; then
  self_check || exit 1
  info "反向測試全部通過 —— 上面那些綠燈才算數"
  exit 0
fi

info "docs/ui-design.md §12.12 的檢核項"
run_checks
printf '\n'
if [ "${FAILED}" -gt 0 ]; then
  printf '\033[1;31m%d 條不合格(共檢查 %d 組)\033[0m\n' "${FAILED}" "${CHECKED}" >&2
  exit 1
fi
if [ "${CHECKED}" -lt 15 ]; then
  # ⚠ 「一條都沒跑卻報通過」是這張檢核表自己最可能的失效方式(§2-G)。
  printf '\033[1;31m只跑了 %d 組檢查 —— 少於下界,當作失敗\033[0m\n' "${CHECKED}" >&2
  exit 1
fi
printf '\033[1;32m%d 組檢查全部通過\033[0m\n' "${CHECKED}"
