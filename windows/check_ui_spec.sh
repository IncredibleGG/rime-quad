#!/usr/bin/env bash
#
# windows/check_ui_spec.sh — docs/ui-design.md §12.12 的檢核項
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
      "test_service_state.cc:service_state_three_situations_are_three_different_states" \
      "test_service_state.cc:service_state_every_state_says_a_different_sentence" \
      "test_service_state.cc:service_state_preparing_is_not_a_failure" \
      "test_service_state.cc:service_state_reads_the_not_ready_flag_off_the_wire" \
      "test_layout.cc:layout_never_drops_candidates_horizontal_shrink" \
      "test_ui_layout.cc:ui_layout_every_clickable_target_meets_the_minimum" \
      "test_ui_layout.cc:ui_layout_content_column_three_worked_cases" \
      "test_ui_layout.cc:ui_layout_every_clickable_target_is_reachable" \
      "test_ui_layout.cc:ui_layout_scroll_range_actually_uses_the_window_height" \
      "test_ui_layout.cc:ui_layout_scrolled_placement_actually_subtracts_the_scroll" \
      "test_ui_layout.cc:ui_layout_scrolled_out_controls_stay_in_the_tab_order" \
      "test_ui_layout.cc:ui_layout_every_control_belongs_to_exactly_one_page" \
      "test_statusbar_place.cc:statusbar_falls_back_when_the_monitor_disappears" \
      "test_statusbar_place.cc:statusbar_growing_wider_stays_inside_the_work_area" \
      "test_ui_palette.cc:palette_every_pair_meets_its_threshold_in_both_modes" \
      "test_status_cells.cc:status_cells_input_mode_shows_exactly_one_label" \
      "test_status_cells.cc:status_cells_mode_and_variant_speak_the_same_way" \
      "test_ui_layout.cc:ui_layout_sidebar_list_never_covers_the_status_lines" \
      "test_hotkey_policy.cc:hotkey_does_not_swallow_anything_else" \
      "test_win32_listview.cc:win32_listview_custom_draw_actually_paints_rows" ; do
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
  # ⚠ 數的是**字面本身**,不是常數名。數常數名的話,「把四個定義整組刪掉」
  #   仍然是綠的 —— 那些名字在別處(交給 status_cells.cc 的那一段)還在,
  #   而反向測試 2026-08-10 正好抓到這件事。四個字面**各**要出現過。
  local in_bar; in_bar="$(num "$(grep -o 'L"中"\|L"En"\|L"简"\|L"繁"' "${bar}" 2>/dev/null | sort -u | grep -c . || true)")"
  local in_cat; in_cat="$(num "$(grep -o 'L"中"\|L"简"\|L"繁"' "${cat}" 2>/dev/null | grep -c . || true)")"
  if [ "${in_bar}" -ne 4 ]; then
    red "W10:狀態列繪製碼裡找不到四個狀態字面(找到 ${in_bar}/4 種)—— 0 代表掃錯檔案"
  elif [ "${in_cat}" -ne 0 ]; then
    red "W10:狀態字面跑進 catalog 了(命中 ${in_cat})—— §8.12 規定它們四端一致、不得在地化"
  else
    ok "W10 四個狀態字面都在繪製碼裡,而且不在 catalog 裡"
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
  #
  # ⚠ 上一版這一條算了一個 w12(數「同一個檔案裡 WM_SETTINGCHANGE 與
  #   ImmersiveColorSet 都出現」的檔案數),然後**從來沒有讀它** ——
  #   真正的斷言只剩「windows/ 底下某處有 ImmersiveColorSet 這個字串」。
  #   而那個字串住在 ui_theme.cc 的比對函式裡,把設定視窗的
  #   WM_SETTINGCHANGE 整段刪掉它照樣是綠的。
  #
  #   現在驗的是資料流:比對函式真的比對那個字面值,而**每一個**
  #   有 WM_SETTINGCHANGE 分支的視窗都要在那一支裡呼叫它、並且換主題。
  check
  local w12bad=0
  local w12out; w12out="$("${PY}" - "${CODE_DIR}" <<'PYSCRIPT'
import sys as _s
# ⚠ windows-latest 的 runner 上 python 的 print 吐的是 CRLF。bash 的 $(...)
#   只剥掉末尾的 \n,於是每一行都帶著 \r —— `case "${line}" in SCOPE_OK)`
#   就對不上,而症狀是「未知的回報:SCOPE_OK」這種看不懂的紅字。
#   (實際發生過:CI run 31331902667 的 W12。)
_s.stdout.reconfigure(encoding='utf-8', newline='')
import glob, os, re, sys
root = sys.argv[1]
out = []

theme = os.path.join(root, 'service', 'ui_theme.cc')
try:
    t = open(theme, encoding='utf-8', errors='replace').read()
except OSError:
    t = ''
m = re.search(r'bool Theme::IsColorSetChange\(LPARAM l\) \{(.*?)\n\}', t, re.S)
if not m:
    out.append('NO_COMPARE_FN')
elif 'L"ImmersiveColorSet"' not in m.group(1):
    out.append('NO_LITERAL')
elif 'lstrcmpiW' not in m.group(1):
    out.append('NOT_CASE_INSENSITIVE')

n = 0
for f in sorted(glob.glob(os.path.join(root, 'service', '*.cc'))):
    src = open(f, encoding='utf-8', errors='replace').read()
    for m in re.finditer(r'case WM_SETTINGCHANGE:(.{0,400}?)(?=\n *case |\n *default:)',
                         src, re.S):
        n += 1
        body = m.group(1)
        rel = os.path.relpath(f, root)
        if 'IsColorSetChange(l)' not in body:
            out.append('NO_CALL=' + rel)
        if 'RefreshTheme()' not in body:
            out.append('NO_REFRESH=' + rel)
print('SCOPE_OK')
print('HANDLERS=%d' % n)
for line in out:
    print(line)
PYSCRIPT
)"
  w12msg() { red "W12:$1"; w12bad=1; }
  case "${w12out}" in
    SCOPE_OK*) ;;
    *) w12msg "掃描程式沒跑完(沒有 SCOPE_OK)。實際輸出:
${w12out}" ;;
  esac
  local nw12; nw12="$(num "$(printf '%s\n' "${w12out}" | sed -n 's/^HANDLERS=//p')")"
  need_scope "W12 WM_SETTINGCHANGE 分支數" "${nw12}" 2 || w12bad=1
  local line12
  while IFS= read -r line12; do
    line12="${line12%$'\r'}"   # 雙保險:助手萬一又吐 CRLF
    case "${line12}" in
      ''|SCOPE_OK|HANDLERS=*) continue ;;
      NO_COMPARE_FN) w12msg "找不到 Theme::IsColorSetChange —— 掃描範圍錯了" ;;
      NO_LITERAL)
        w12msg "IsColorSetChange 沒有比對 L\"ImmersiveColorSet\" —— 系統換深淺時
     我們收得到 WM_SETTINGCHANGE 卻認不出是哪一種,於是不會跟著換" ;;
      NOT_CASE_INSENSITIVE)
        w12msg "IsColorSetChange 沒有用 lstrcmpiW —— 那個字串的大小寫不保證" ;;
      NO_CALL=*)
        w12msg "${line12#NO_CALL=} 的 WM_SETTINGCHANGE 分支沒有呼叫 IsColorSetChange(l) ——
     這個視窗不會跟著系統換深淺(而整檔 grep 看不出來:那個字串在 ui_theme.cc)" ;;
      NO_REFRESH=*)
        w12msg "${line12#NO_REFRESH=} 認出了色彩變更卻沒有 RefreshTheme() —— 認出來了不做事" ;;
      *) w12msg "未知的回報:${line12}" ;;
    esac
  done <<< "${w12out}"
  [ "${w12bad}" -eq 0 ] && ok "W12 ${nw12} 個 WM_SETTINGCHANGE 分支都呼叫 IsColorSetChange(l) 並換主題,而比對函式真的比對 L\"ImmersiveColorSet\""

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

  # ── W24:設定視窗的版面只能住在 common/ui_layout.cc ────────────
  #
  # ⚠ 這一條是「守門者自己在該紅的時候安靜地不跑」的解藥。
  #   舊版:每一頁的版面在 service/settings_window.cc::LayoutUi() 裡算,
  #   而那個檔案在 Ubuntu 上編不起來 —— 於是 ui_layout.cc 的
  #   ClickableTargetsDip **手工造了一份假骨架**當代表,還把
  #   window_h_dip `(void)` 掉。結果:外觀頁的深淺色三態排在 y=574,
  #   可視高度 506,那三顆在畫面上不存在,而 W18 一路全綠。
  #
  #   兩個方向都驗:
  #     (a) settings_window.cc 不得再自己排版(不得有 Stack / st.Push)。
  #     (b) kControls 的 id 集合 == ui_layout.cc 版面裡的 id 集合。
  #         多一顆(建了沒地方擺)、少一顆(擺了沒建)都紅。
  check
  local w24bad=0
  local w24a; w24a="$(grep -n 'Stack [a-z]\|st\.Push(\|st\.PushDivider(\|PushDivider()' "${sw}" 2>/dev/null || true)"
  if [ -n "${w24a}" ]; then
    red "W24:settings_window.cc 又開始自己排版了 —— 那裡算出來的矩形單元測試看不到(外觀頁那三顆單選鈕就是這樣消失的)"
    printf '%s\n' "${w24a}" | head -3 >&2
    w24bad=1
  fi
  local w24out; w24out="$("${PY}" - "${sw}" "${CODE_DIR}/common/ui_layout.cc" <<'PYSCRIPT'
import re, sys
sw = open(sys.argv[1], encoding='utf-8', errors='replace').read()
lay = open(sys.argv[2], encoding='utf-8', errors='replace').read()

m = re.search(r'const ControlDef kControls\[\] = \{(.*?)\n\};', sw, re.S)
if not m:
    print('NOTABLE'); raise SystemExit
table = set(re.findall(r'\{(IDC_[A-Z0-9_]+),', m.group(1)))

f = re.search(r'PageLayout LayoutSettingsPageDip\(.*?\n\}\n', lay, re.S)
if not f:
    print('NOLAYOUT'); raise SystemExit
laid = set(re.findall(r'(IDC_[A-Z0-9_]+)', f.group(0)))

# 底部固定列不屬於任何一頁:它不捲動,由呼叫端擺。
chrome = {'IDC_STATUS', 'IDC_CLOSE'}
print('TABLE=%d' % len(table))
print('LAID=%d' % len(laid))

# ⚠ 只比集合擋不住「同一顆被塞進兩頁」——集合一樣,而畫面上會有一顆
#   切了頁還留著的控制項。逐頁拆開再兩兩比。
parts = re.split(r'case (kPage[A-Za-z]+):', f.group(0))
pages = {}
for i in range(1, len(parts) - 1, 2):
    pages[parts[i]] = set(re.findall(r'(IDC_[A-Z0-9_]+)', parts[i + 1]))
print('PAGES=%d' % len(pages))
names = sorted(pages)
for a in range(len(names)):
    for b in range(a + 1, len(names)):
        for i in sorted(pages[names[a]] & pages[names[b]]):
            print('ON_TWO_PAGES=%s(%s,%s)' % (i, names[a], names[b]))
for i in sorted((table - chrome) - laid):
    print('BUILT_BUT_NOT_LAID=%s' % i)
for i in sorted(laid - (table - chrome)):
    print('LAID_BUT_NOT_BUILT=%s' % i)
PYSCRIPT
)"
  local ntable; ntable="$(num "$(printf '%s\n' "${w24out}" | sed -n 's/^TABLE=//p')")"
  local nlaid; nlaid="$(num "$(printf '%s\n' "${w24out}" | sed -n 's/^LAID=//p')")"
  case "${w24out}" in
    NOTABLE*) red "W24:找不到 kControls —— 掃描範圍錯了"; w24bad=1 ;;
    NOLAYOUT*) red "W24:找不到 LayoutSettingsPageDip —— 掃描範圍錯了"; w24bad=1 ;;
  esac
  need_scope "W24 控制項表" "${ntable}" 60 || w24bad=1
  need_scope "W24 版面 id" "${nlaid}" 60 || w24bad=1
  local npages; npages="$(num "$(printf '%s\n' "${w24out}" | sed -n 's/^PAGES=//p')")"
  need_scope "W24 頁數" "${npages}" 4 || w24bad=1
  local w24dup; w24dup="$(printf '%s\n' "${w24out}" | grep '^ON_TWO_PAGES=' || true)"
  if [ -n "${w24dup}" ]; then
    red "W24:同一顆控制項被排進兩頁 —— 切了頁還會留一顆在畫面上"
    printf '%s\n' "${w24dup}" | head -6 >&2
    w24bad=1
  fi
  local w24diff; w24diff="$(printf '%s\n' "${w24out}" | grep '^BUILT_BUT_NOT_LAID=\|^LAID_BUT_NOT_BUILT=' || true)"
  if [ -n "${w24diff}" ]; then
    red "W24:kControls 與 ui_layout.cc 的版面對不上 —— 建了沒地方擺(停在 0,0)或擺了沒建"
    printf '%s\n' "${w24diff}" | head -6 >&2
    w24bad=1
  fi
  # ⚠ 頁名也是「哪一頁上有什麼」的一部分。它以前是 settings_window.cc 裡
  #   一個與 SettingsPage 平行的陣列,順序錯開一格的樣子是「側欄寫著
  #   『連網』,點下去出現的是進階頁」——而那個檔案在 Ubuntu 上編不起來。
  # ⚠ 只禁**頁名**那五條。kNavStatus*(側欄底部那兩行狀態)是另一回事,
  #   它本來就住在這個檔案的繪製碼裡 —— 一條會誤報的檢查,下一步就是
  #   被關掉(§3.1 的教訓)。第一版寫成 `grep 'UiString::kNav'`,
  #   當場就打到 kNavStatusOffline。
  local w24names
  w24names="$(grep -nE 'UiString::kNav(Schemas|Appearance|Text|Network|Advanced)\b' "${sw}" 2>/dev/null || true)"
  if [ -n "${w24names}" ]; then
    red "W24:settings_window.cc 又自己帶了一份側欄頁名 —— 它必須走 common/ui_layout.h 的 SettingsPageName()"
    printf '%s\n' "${w24names}" | head -3 >&2
    w24bad=1
  fi
  if ! grep -q 'UiString SettingsPageName(int page)' "${CODE_DIR}/common/ui_layout.cc" 2>/dev/null; then
    red "W24:common/ui_layout.cc 沒有 SettingsPageName —— 頁名又不是純函式了"
    w24bad=1
  fi
  [ "${w24bad}" -eq 0 ] && ok "W24 版面全部在 common/ui_layout.cc;${ntable} 顆控制項與 ${npages} 頁的版面兩個方向都對得上,而且沒有一顆同時屬於兩頁"

  # ── W25:內容區必須捲得動,而且捲動量真的要套到控制項上 ──
  #
  # ⚠ 外觀頁的內容高 890 DIP。150% 的 1080p 筆電 client 約 667 DIP ——
  #   **視窗拉到最大也碰不到深淺色三態**,而那三顆是整個 UI 上唯一的入口。
  #
  # ⚠ **上一輪這一條是假綠的,而且是被實測拆出來的。** 舊版對整份
  #   settings_window.cc `grep -q` 七個字串。三種拆法都騙得過它:
  #     (a) `const int y = p->rect.y - scroll_;` → `= p->rect.y;`
  #         捲軸拖得動、內容一動也不動。七個字串一個不少。
  #     (b) 捲出可視範圍的控制項改回 `ShowWindow(SW_HIDE)`
  #         —— 它一退出 Tab 順序,鍵盤使用者就再也碰不到。一樣全綠。
  #     (c) 主視窗的 WS_VSCROLL 拿掉(捲軸整條消失)—— 而 :172 那顆
  #         IDC_DIAG 唯讀 EDIT 也寫著 WS_VSCROLL,所以整檔 grep 永遠命中。
  #         `EnsureFocusVisible` 同一形狀:名字出現兩次(呼叫、定義),
  #         把訊息迴圈裡的**呼叫**刪掉、定義留著,也是綠的。
  #
  #   所以現在驗的是**呼叫位置與資料流**,不是「檔案裡有沒有這個字」:
  #     · 三件事的決定權搬到 common/ui_layout.cc 的 ScrollPlaceControlDip(),
  #       那是一支純函式,由兩條真的單元測試釘住;
  #     · 這裡驗的是 LayoutUi() 真的把 `scroll_` 送進去、而且 y / 裁切 /
  #       ShowWindow 的引數都是從它的回傳值來的。
  #
  # ⚠ 這一段的 python 失敗時**必須是紅**。上一次寫錯一個跳脫字元,
  #   python 當場 SyntaxError、輸出是空的,而這一條印了 ok ——
  #   所以它必須先印 SCOPE_OK,沒有那一行就當作沒跑過。
  check
  local w25bad=0
  local w25out; w25out="$("${PY}" - "${sw}" "${CODE_DIR}/common/ui_layout.cc" <<'PYSCRIPT'
import sys as _s
# ⚠ windows-latest 的 runner 上 python 的 print 吐的是 CRLF。bash 的 $(...)
#   只剥掉末尾的 \n,於是每一行都帶著 \r —— `case "${line}" in SCOPE_OK)`
#   就對不上,而症狀是「未知的回報:SCOPE_OK」這種看不懂的紅字。
#   (實際發生過:CI run 31331902667 的 W12。)
_s.stdout.reconfigure(encoding='utf-8', newline='')
import re, sys
sw = open(sys.argv[1], encoding='utf-8', errors='replace').read()
lay = open(sys.argv[2], encoding='utf-8', errors='replace').read()

def body_of(src, head, endpat='\n}\n'):
    i = src.find(head)
    if i < 0:
        return None
    j = src.find(endpat, i)
    return src[i:j] if j > 0 else src[i:]

out = []

# -- 1. main window creation must carry WS_VSCROLL --
i = sw.find('hwnd_ = ::CreateWindowExW(')
if i < 0:
    out.append('NO_MAINWIN')
else:
    call = sw[i:sw.find(');', i)]
    if 'kClass' not in call:
        out.append('MAINWIN_NOT_KCLASS')
    if 'WS_VSCROLL' not in call:
        out.append('NO_MAINWIN_VSCROLL')

# -- 2. message cases must reach their handlers --
for msg, fn in (('WM_VSCROLL', 'OnVScroll('), ('WM_MOUSEWHEEL', 'OnMouseWheel(')):
    m = re.search(r'case ' + msg + r':(.{0,200}?)(?=\n *case |\n *default:)', sw, re.S)
    if not m:
        out.append('NO_CASE=' + msg)
    elif fn not in m.group(1):
        out.append('CASE_DEAD=' + msg)

# -- 3. keyboard focus must be *called* from the message loop --
loop = body_of(sw, 'while (::GetMessageW(', '\n  }\n')
if loop is None:
    out.append('NO_MSGLOOP')
elif 'EnsureFocusVisible()' not in loop:
    out.append('NO_FOCUS_CALL')
efv = body_of(sw, 'void SettingsWindow::EnsureFocusVisible()')
if efv is None:
    out.append('NO_FOCUS_DEF')
elif 'SetScroll(' not in efv:
    out.append('FOCUS_DEF_DEAD')

# -- 4. LayoutUi: scrollbar fed, and the three wires --
lu = body_of(sw, 'void SettingsWindow::LayoutUi() {')
if lu is None:
    out.append('NO_LAYOUTUI')
else:
    if 'si.nPos = scroll_;' not in lu or '::SetScrollInfo(hwnd_, SB_VERT, &si,' not in lu:
        out.append('SCROLLBAR_NOT_FED')
    if not re.search(r'ScrollPlaceControlDip\(\s*p->rect,\s*scroll_,\s*viewport_h\s*\)', lu):
        out.append('NO_SCROLL_ARG')
    if not re.search(r'place\(id, RectI\{p->rect\.x, sp\.y_dip,', lu):
        out.append('Y_NOT_FROM_FN')
    if not re.search(r'ClipToViewport\(i, c, p->rect\.w, sp\.clip_h_dip\)', lu):
        out.append('CLIP_NOT_FROM_FN')
    if not re.search(r'::ShowWindow\(c, sp\.visible \? SW_SHOW : SW_HIDE\);', lu):
        out.append('SHOW_NOT_FROM_FN')
    n_hide = lu.count('SW_HIDE')
    if n_hide != 2:
        out.append('EXTRA_HIDE=%d' % n_hide)

# -- 5. the pure side --
if 'int ScrollMaxDip(' not in lay:
    out.append('NO_SCROLLMAX')
if 'ScrolledPlacement ScrollPlaceControlDip(' not in lay:
    out.append('NO_PUREFN')
if '(void)window_h_dip' in lay:
    out.append('VOID_HEIGHT')

print('SCOPE_OK')
for line in out:
    print(line)
PYSCRIPT
)"
  w25msg() { red "W25:$1"; w25bad=1; }
  case "${w25out}" in
    SCOPE_OK*) ;;
    *) w25msg "這一條的掃描程式根本沒跑完(沒有 SCOPE_OK)。
     上一版就是這樣失效的:python 當場 SyntaxError、輸出是空的,
     而「沒有任何違規」被印成了 ok。實際輸出:
${w25out}" ;;
  esac
  local line
  while IFS= read -r line; do
    line="${line%$'\r'}"   # 雙保險:助手萬一又吐 CRLF
    case "${line}" in
      ''|SCOPE_OK) continue ;;
      NO_MAINWIN)  w25msg "找不到 hwnd_ = ::CreateWindowExW( —— 掃描範圍錯了" ;;
      MAINWIN_NOT_KCLASS) w25msg "hwnd_ 的建立呼叫裡沒有 kClass —— 抓錯呼叫了,這一條不算數" ;;
      NO_MAINWIN_VSCROLL)
        w25msg "**主視窗**的建立呼叫裡沒有 WS_VSCROLL —— 捲軸整條不存在。
     (整檔 grep 拉不出這件事:IDC_DIAG 那顆唯讀 EDIT 也寫著 WS_VSCROLL。)" ;;
      NO_CASE=*)   w25msg "WndProc 裡沒有 ${line#NO_CASE=} 分支 —— 那一條路是死的" ;;
      CASE_DEAD=*) w25msg "${line#CASE_DEAD=} 分支在,卻沒有呼叫對應的處理常式 ——
     訊息被吞下來什麼都不做,與沒接一樣,而 grep 看得到字串" ;;
      NO_MSGLOOP)  w25msg "找不到訊息迴圈 —— 掃描範圍錯了" ;;
      NO_FOCUS_CALL)
        w25msg "訊息迴圈裡**沒有呼叫** EnsureFocusVisible() —— Tab 走到視窗外的
     控制項時畫面一動也不動,使用者在盲按。(定義留著不算:
     那個名字在檔案裡本來就有兩次,整檔 grep 永遠是綠的。)" ;;
      NO_FOCUS_DEF)   w25msg "找不到 EnsureFocusVisible 的定義" ;;
      FOCUS_DEF_DEAD) w25msg "EnsureFocusVisible 沒有呼叫 SetScroll() —— 它不會把任何東西捲進來" ;;
      NO_LAYOUTUI)    w25msg "找不到 LayoutUi —— 掃描範圍錯了" ;;
      SCROLLBAR_NOT_FED)
        w25msg "捲軸沒有吃到 scroll_(缺 si.nPos = scroll_ 或 SetScrollInfo)——
     拇指位置與實際捲動量會對不上" ;;
      NO_SCROLL_ARG)
        w25msg "LayoutUi 沒有把 **scroll_** 送進 ScrollPlaceControlDip() ——
     捲軸拖得動、內容一動也不動。這正是上一輪被實測拆掉而全綠的那一行。" ;;
      Y_NOT_FROM_FN)
        w25msg "控制項的 y **不是**從 ScrollPlaceControlDip() 的回傳值來的(預期
     place(id, RectI{p->rect.x, sp.y_dip, ...}))—— 寫成 p->rect.y 就是不捲了" ;;
      CLIP_NOT_FROM_FN)
        w25msg "裁切高度不是從 ScrollPlaceControlDip() 來的(預期
     ClipToViewport(i, c, p->rect.w, sp.clip_h_dip))" ;;
      SHOW_NOT_FROM_FN)
        w25msg "ShowWindow 的引數不是 sp.visible(預期
     ::ShowWindow(c, sp.visible ? SW_SHOW : SW_HIDE);)—— 寫死 SW_HIDE 的話,
     捲出可視範圍的控制項會退出 Tab 順序,鍵盤使用者再也碰不到它" ;;
      EXTRA_HIDE=*)
        w25msg "LayoutUi 裡的 SW_HIDE 有 ${line#EXTRA_HIDE=} 處(只允許 2:「這一頁上不
     出現」那一支,加上 sp.visible 那個三元式)—— 多出來的那一個很可能
     在把捲出去的控制項藏起來" ;;
      NO_SCROLLMAX)
        w25msg "common/ui_layout.cc 沒有 ScrollMaxDip —— 捲動範圍不是純函式就測不到" ;;
      NO_PUREFN)
        w25msg "common/ui_layout.cc 沒有 ScrollPlaceControlDip —— 捲動後的位置/裁切/顯示
     又回到 settings_window.cc 裡了,而那裡單元測試看不到" ;;
      VOID_HEIGHT)
        w25msg "ui_layout.cc 又把 window_h_dip 丟掉了 —— 「排到視窗底部以外」對測試
     而言會再一次不存在" ;;
      *) w25msg "未知的回報:${line}" ;;
    esac
  done <<< "${w25out}"
  [ "${w25bad}" -eq 0 ] && ok "W25 內容區捲得動:主視窗有捲軸、滾輪/捲軸/鍵盤焦點三條路都接到處理常式,而且 y / 裁切 / 顯示與否三條接線都從 ScrollPlaceControlDip() 來"

  # ── W26:狀態列寬度一變就要重新夾進工作區 ─────────────────────
  #
  # ⚠ 舊版 Relayout 用 SWP_NOMOVE:左上角釘死、只往右長。而那一橫是
  #   右錨定的,「未就緒(1 格)→ 就緒(4 格)」多出 80~110 DIP,
  #   扣掉 12 的邊距之後有 70~100 DIP 在螢幕外面 —— 「設定」整格點不到。
  check
  local w26bad=0
  local relay; relay="$("${PY}" - "${bar}" <<'PYSCRIPT'
import re, sys
s = open(sys.argv[1], encoding='utf-8', errors='replace').read()
m = re.search(r'void StatusBar::Relayout\(\) \{(.*?)\n\}\n', s, re.S)
if not m:
    print('NOFUNC'); raise SystemExit
body = m.group(1)
if 'SWP_NOMOVE' in body and 'ApplyPlacement' not in body:
    print('NOMOVE')
if 'ApplyPlacement' not in body:
    print('NOPLACE')
PYSCRIPT
)"
  case "${relay}" in
    *NOFUNC*) red "W26:找不到 StatusBar::Relayout —— 掃描範圍錯了"; w26bad=1 ;;
  esac
  if printf '%s\n' "${relay}" | grep -q 'NOPLACE\|NOMOVE'; then
    red "W26:Relayout 改了寬度卻沒有重走 PlaceStatusBar —— 那一橫變寬時右端會被推出螢幕,「設定」整格點不到"
    w26bad=1
  fi
  # 那支純函式必須收得到寬度(而不是自己去讀視窗現在的寬度)。
  if ! grep -q 'void StatusBar::ApplyPlacement(int w_dip)' "${bar}" 2>/dev/null; then
    red "W26:ApplyPlacement 沒有收寬度參數 —— 從 GetWindowRect 讀回來的是舊寬度"
    w26bad=1
  fi
  # ── 按下去畫面要立刻動,而且動的是**引擎說的**那個值 ──────────
  #
  # ⚠ 這一條的判準**反過來了**,而反過來的理由是使用者實機回報。
  #
  #   舊版要求兩格「樂觀寫入 + 重畫」:點下去就自己翻,不等引擎。
  #   那個要求當時解的是真問題(那兩格唯一的更新路徑是 OnSnapshot,
  #   而 OnSnapshot 要等使用者真的打一個字,所以不寫的話點下去畫面
  #   完全不變)。
  #
  #   但代價是那一橫可以顯示一個**從來沒有發生過的狀態**。使用者回報:
  #   設定裡選了簡體、畫面說「简」、打出來是繁體。那一格在替一件沒有
  #   發生的事作證,而 W26 舊版正是**要求**它那樣做。
  #
  #   現在的形狀是「送出去 → 立刻向引擎回讀」:
  #     · 不得有樂觀寫入(本地狀態的指派)
  #     · 一定要有送出去的呼叫
  #     · 一定要有 RefreshFromEngine(),而且在送出去**之後**
  #
  #   回讀一樣不需要使用者先打一個字 —— 它解掉舊要求要解的問題,
  #   而且不必宣稱任何沒有證據的事。
  #
  # ⚠ 順序仍然是判準的一部分:先回讀才送出去的話,讀到的是舊值,
  #   那一格會停在點下去之前的樣子。
  local w26c; w26c="$("${PY}" - "${bar}" <<'PYSCRIPT'
import sys as _s
# ⚠ windows-latest 的 runner 上 python 的 print 吐的是 CRLF,見 W25 的說明。
_s.stdout.reconfigure(encoding='utf-8', newline='')
import re, sys
s = open(sys.argv[1], encoding='utf-8', errors='replace').read()
out = []
m = re.search(r'void StatusBar::ClickCell\(int cell\) \{(.*?)\n\}\n', s, re.S)
if not m:
    print('SCOPE_OK')
    print('NOFUNC')
    raise SystemExit
body = m.group(1)

# (格名, 送出去的呼叫, 不得出現的樂觀寫入)
cells = (
    ('kCellMode', 'SetAsciiModeAll(', 'ascii_mode_ = '),
    ('kCellVariant', 'SetVariantPref(', 'variant_ = '),
)
for name, send, optimistic in cells:
    c = re.search(r'case ' + name + r': \{(.*?)\n    \}', body, re.S)
    if not c:
        out.append('NOCELL=' + name)
        continue
    b = c.group(1)
    # 註解裡會提到這些名字,所以先把註解整行剔掉再判斷。
    code = '\n'.join(l for l in b.split('\n') if not l.lstrip().startswith('//'))
    if optimistic in code:
        out.append('OPTIMISTIC=' + name)
    isend = code.find(send)
    if isend < 0:
        out.append('NOSEND=' + name)
        continue
    iread = code.find('RefreshFromEngine();')
    if iread < 0:
        out.append('NOREADBACK=' + name)
    elif iread < isend:
        out.append('READBACK_BEFORE_SEND=' + name)

# 回讀那一支本身要真的去問引擎,而且要重畫 —— 定義留著、身體空掉
# 一樣是綠的,那正是上一輪被拆掉的形狀。
r = re.search(r'void StatusBar::RefreshFromEngine\(\) \{(.*?)\n\}\n', s, re.S)
if not r:
    out.append('NOREFRESHDEF')
else:
    rb = r.group(1)
    if 'ReadBackStatus()' not in rb:
        out.append('REFRESH_NOT_ASKING')
    if 'Relayout();' not in rb or '::InvalidateRect(' not in rb:
        out.append('REFRESH_NOT_REPAINTING')

# ── kHidden 那一格點不到,是 ClickCell 那一段不可達的**唯一**依據 ──
#
# ClickCell 的 kCellVariant 分支明著寫「variant_ 不可能是 kHidden」。
# 撐住那句話的是兩行 Win32 程式碼:空字串的格子零寬、HitCell 跳過
# 零寬的格子。兩行都刪得掉,而刪掉之後那一格會在**沒有任何證據**的
# 狀態下被點到 —— 使用者按到一個看不見的開關,而方向還是猜的。
# 不能只靠一句註解宣稱它不可達 —— 那正是這一輪在拆的東西。
rl = re.search(r'void StatusBar::Relayout\(\) \{(.*?)\n\}\n', s, re.S)
if not rl:
    out.append('NORELAYOUTDEF')
elif ('if (c.text.empty()) {' not in rl.group(1)
      or 'c.rc = RECT{0, 0, 0, 0};' not in rl.group(1)):
    out.append('EMPTY_CELL_TAKES_SPACE')
hc = re.search(r'int StatusBar::HitCell\(POINT pt\) const \{(.*?)\n\}\n', s, re.S)
if not hc:
    out.append('NOHITCELLDEF')
elif 'if (r.right <= r.left) continue;' not in hc.group(1):
    out.append('HITCELL_HITS_ZERO_WIDTH')
print('SCOPE_OK')
for line in out:
    print(line)
PYSCRIPT
)"
  w26msg() { red "W26:$1"; w26bad=1; }
  case "${w26c}" in
    SCOPE_OK*) ;;
    *) w26msg "简/繁 那一段的掃描程式沒跑完(沒有 SCOPE_OK)。實際輸出:
${w26c}" ;;
  esac
  local l26
  while IFS= read -r l26; do
    l26="${l26%$'\r'}"   # 雙保險:助手萬一又吐 CRLF
    case "${l26}" in
      ''|SCOPE_OK) continue ;;
      NOFUNC)   w26msg "找不到 StatusBar::ClickCell —— 掃描範圍錯了" ;;
      NOCELL=*) w26msg "ClickCell 裡找不到 ${l26#NOCELL=} 分支 —— 掃描範圍錯了" ;;
      OPTIMISTIC=*)
        w26msg "${l26#OPTIMISTIC=} 那一格又樂觀寫入本地狀態了 —— 點下去畫面就翻,
     而引擎有沒有照做完全沒問過。使用者回報的「畫面說简、打出來是繁」
     就是這個形狀。送出去之後走 RefreshFromEngine(),讓引擎說了算。" ;;
      NOSEND=*)
        w26msg "${l26#NOSEND=} 那一格沒有把新值送出去 —— 點了等於沒點" ;;
      NOREADBACK=*)
        w26msg "${l26#NOREADBACK=} 那一格送出去之後沒有 RefreshFromEngine() ——
     **那一格要等使用者真的打一個字才會變**,而使用者會以為沒點到,
     然後再點一次(於是切回去了)。" ;;
      READBACK_BEFORE_SEND=*)
        w26msg "${l26#READBACK_BEFORE_SEND=} 那一格先回讀才送出去 —— 讀到的是舊值,
     那一格會停在點下去之前的樣子" ;;
      NOREFRESHDEF) w26msg "找不到 StatusBar::RefreshFromEngine 的定義" ;;
      NORELAYOUTDEF) w26msg "找不到 StatusBar::Relayout 的定義 —— 掃描範圍錯了" ;;
      NOHITCELLDEF) w26msg "找不到 StatusBar::HitCell 的定義 —— 掃描範圍錯了" ;;
      EMPTY_CELL_TAKES_SPACE)
        w26msg "Relayout 沒有把空字串的格子設成 {0,0,0,0} —— 簡/繁 那一格在
     kHidden(引擎沒有回報任何字形)時畫的是空字串,而它現在仍然佔位置。
     使用者會按到一個**看不見的開關**,而 ClickCell 那一段明著寫了
     「variant_ 不可能是 kHidden」—— 這一行就是那句話的依據" ;;
      HITCELL_HITS_ZERO_WIDTH)
        w26msg "HitCell 不再跳過零寬的格子(預期 if (r.right <= r.left) continue;)
     —— 同上:那一格在沒有任何證據的狀態下按得到,而方向是猜的" ;;
      REFRESH_NOT_ASKING)
        w26msg "RefreshFromEngine 沒有呼叫 ReadBackStatus() —— 它根本沒去問引擎,
     而只看「有沒有呼叫 RefreshFromEngine」的檢查會是綠的" ;;
      REFRESH_NOT_REPAINTING)
        w26msg "RefreshFromEngine 讀了狀態卻沒有 Relayout() + InvalidateRect() ——
     那一格照樣不會變,使用者看到的與根本沒讀一模一樣" ;;
      *) w26msg "未知的回報:${l26}" ;;
    esac
  done <<< "${w26c}"
  # ⚠ 這句話要說**現在**的判準。舊版寫的是「简/繁 那一格按下去立刻改變」,
  #   而「立刻改變」正是新判準明文禁止的樂觀寫入 —— 上面那一段抓的是
  #   `variant_ = ` / `ascii_mode_ = `,通過的意思是「沒有樂觀寫入」。
  #   讀守門輸出的人會照這句話去理解程式碼該長什麼樣子,說反了就是
  #   叫下一個人把缺陷寫回來。
  [ "${w26bad}" -eq 0 ] && ok "W26 狀態列寬度一變就重走 PlaceStatusBar,而且 中/En 與 简/繁 兩格都**不**樂觀寫入 —— 送出去之後立刻向引擎回讀,畫面上那個字是引擎說的"

  # ── W27:三種處境三句話,而且那三句話真的流到畫面上 ─────────────
  #
  # ⚠ 這一條守的是 common/service_state.h 檔頭那個缺陷:
  #   「還在準備 / 準備失敗 / 引擎不在」被壓成一個布林,於是三種都畫
  #   同一句紅字「輸入法沒有在跑」—— 而第一種那句話是假的,
  #   而且它正好是使用者第一次安裝時看到的那一句。
  #
  # ⚠ **這裡刻意不用 `grep -q 某個名字` 掃整個檔案。** 那種寫法上一輪
  #   剛被實測拆穿:名字在註解或別的函式裡出現一次,那一條就永遠綠。
  #   下面逐一把**函式本體**挖出來,比對的是呼叫位置與**資料真的流過去**
  #   (`service_state_ = CurrentServiceState();`、
  #    `c.text = UiText(StatusTextFor(service_state_));`),
  #   以及對照表回了幾條**相異**的字串 —— 合併回同一句就少一條。
  check
  local w27bad=0
  local w27out; w27out="$("${PY}" - "${bar}" "${sw}" "${CODE_DIR}/common/service_state.cc" <<'PYSCRIPT'
import re, sys

paths = sys.argv[1:4]
srcs = []
for p in paths:
    try:
        srcs.append(open(p, encoding='utf-8', errors='replace').read())
    except OSError:
        print('NOFILE=%s' % p)
        srcs.append('')
bar, sw, st = srcs

FOUND = [0]

def body(src, key):
    """把一個函式的本體挖出來。挖不到 = 掃描範圍錯了,要紅,不是零個違規。"""
    i = src.find(key)
    if i < 0:
        return None
    b = src.find('{', i)
    if b < 0:
        return None
    j = src.find('\n}\n', b)
    if j < 0:
        return None
    FOUND[0] += 1
    return src[b + 1:j]

def norm(s):
    return ' '.join(s.split())

def want(tag, src, key, needles, forbidden=()):
    bd = body(src, key)
    if bd is None:
        print('NOFUNC=%s' % tag)
        return
    n = norm(bd)
    for x in needles:
        if norm(x) not in n:
            print('MISS=%s :: %s' % (tag, x))
    for x in forbidden:
        if norm(x) in n:
            print('LEFTOVER=%s :: %s' % (tag, x))

# ── 那一橫 ───────────────────────────────────────────────────────
want('StatusBar::Relayout', bar, 'void StatusBar::Relayout()',
     ['service_state_ = CurrentServiceState();',
      'c.text = UiText(StatusTextFor(service_state_));',
      'if (!StateShowsCells(service_state_))'],
     # 舊的合併判斷不可以留在這裡。
     ['deploy_done()'])

want('StatusBar::CurrentServiceState', bar,
     'ServiceState StatusBar::CurrentServiceState()',
     ['facts.engine_present = engine_ != nullptr;',
      'facts.deploy_done = engine_ && engine_->deploy_done();',
      'facts.deploy_ok = engine_ && engine_->deploy_ok();',
      'facts.engine_says_not_ready = engine_not_ready_.load() || '
      '(engine_ && PhaseSaysPreparing(engine_->redeploy_phase()));',
      'return ServiceStateOf(facts);'])

want('StatusBar::OnSnapshot', bar, 'void StatusBar::OnSnapshot(',
     ['SnapshotSaysNotReady(snap.status_flags)',
      'SnapshotFlagsAreUsable(snap.status_flags)'])

want('StatusBar::ThreadMain', bar, 'void StatusBar::ThreadMain()',
     ['::SetTimer(hwnd_, kStateTimer, kStatePollMs, nullptr);'])

want('StatusBar::WndProc', bar, 'LRESULT CALLBACK StatusBar::WndProc(',
     ['case WM_TIMER:',
      'const ServiceState now = self->CurrentServiceState();',
      'if (now != self->service_state_) { self->Relayout();'])

want('StatusBar::Paint', bar, 'void StatusBar::Paint(',
     ['StateIsFailure(service_state_)'])

# ── 設定側欄 ─────────────────────────────────────────────────────
want('SettingsWindow::OnPaint', sw, 'void SettingsWindow::OnPaint(',
     ['const ServiceState state = SidebarServiceState();',
      'UiText(SidebarStatusTextFor(state))',
      'StateIsFailure(state)'],
     # 兩態三元式的殘骸。留著就代表側欄還是只會說兩句話。
     ['UiString::kNavStatusNotRunning'])

want('SettingsWindow::SidebarServiceState', sw,
     'ServiceState SettingsWindow::SidebarServiceState()',
     ['facts.engine_present = engine_ != nullptr;',
      'facts.deploy_done = engine_ && engine_->deploy_done();',
      'facts.deploy_ok = engine_ && engine_->deploy_ok();',
      'facts.engine_says_not_ready = deploying_ || '
      '(engine_ && PhaseSaysPreparing(engine_->redeploy_phase()));',
      'return ServiceStateOf(facts);'])

want('SettingsWindow::WndProc', sw, 'LRESULT CALLBACK SettingsWindow::WndProc(',
     ['if (self && w == kServiceStateTimer) self->OnServiceStateTick();'])

want('SettingsWindow::ThreadMain', sw, 'void SettingsWindow::ThreadMain()',
     ['::SetTimer(hwnd_, kServiceStateTimer, kServiceStatePollMs, nullptr);'])

# ── 對照表本身:三種不可以回同一句 ───────────────────────────────
for tag, key, floor in (('StatusTextFor', 'UiString StatusTextFor(', 4),
                        ('SidebarStatusTextFor',
                         'UiString SidebarStatusTextFor(', 4)):
    bd = body(st, key)
    if bd is None:
        print('NOFUNC=%s' % tag)
        continue
    names = set(re.findall(r'return UiString::(k[A-Za-z0-9_]+);', bd))
    print('DISTINCT=%s=%d' % (tag, len(names)))
    if len(names) < floor:
        print('MERGED=%s=%d' % (tag, len(names)))

print('FUNCS=%d' % FOUND[0])
PYSCRIPT
)"
  local nfuncs; nfuncs="$(num "$(printf '%s\n' "${w27out}" | sed -n 's/^FUNCS=//p')")"
  # ⚠ §2-G2:挖不到函式時的行為必須是**紅**,不是「零個違規」。
  need_scope "W27 挖到的函式數" "${nfuncs}" 12 || w27bad=1
  local w27miss; w27miss="$(printf '%s\n' "${w27out}" | grep '^NOFUNC=\|^NOFILE=' || true)"
  if [ -n "${w27miss}" ]; then
    red "W27:挖不到這些函式 —— 掃描範圍錯了,不是沒有違規"
    printf '%s\n' "${w27miss}" | head -6 >&2
    w27bad=1
  fi
  local w27gap; w27gap="$(printf '%s\n' "${w27out}" | grep '^MISS=' || true)"
  if [ -n "${w27gap}" ]; then
    red "W27:三種處境的判斷沒有真的流到畫面上(下面每一行是一個斷掉的接點)"
    printf '%s\n' "${w27gap}" | head -8 >&2
    w27bad=1
  fi
  local w27old; w27old="$(printf '%s\n' "${w27out}" | grep '^LEFTOVER=' || true)"
  if [ -n "${w27old}" ]; then
    red "W27:舊的兩態判斷還留在繪製碼裡 —— 三種處境又會被壓回同一句"
    printf '%s\n' "${w27old}" | head -4 >&2
    w27bad=1
  fi
  local w27merged; w27merged="$(printf '%s\n' "${w27out}" | grep '^MERGED=' || true)"
  if [ -n "${w27merged}" ]; then
    red "W27:對照表把不同狀態指到同一條字串了(這就是那個缺陷本身)"
    printf '%s\n' "${w27merged}" | head -4 >&2
    w27bad=1
  fi
  [ "${w27bad}" -eq 0 ] && ok "W27 三種處境三句話:${nfuncs} 個函式本體逐一驗過呼叫位置與資料流,而且那一橫與側欄都會自己更新"

  # ── W28:自繪的列矩形只准從 RowRect() 來 ─────────────────────
  #
  # **2026-08-10 在 windows-latest 上實測**(見 tests/test_win32_listview.cc):
  # report 模式的 ListView 在 CDDS_ITEMPREPAINT 給的 NMCUSTOMDRAW::rc 是
  # (0,0,0,0)。拿它去 FillRect + DrawTextW 什麼都不會畫,而 CDRF_SKIPDEFAULT
  # 又把控制項自己的繪製擋掉 —— 結果是**一整片空白而且沒有任何錯誤**。
  # 使用者截圖裡「啟用的方式底下是一個空白的 list」就是這樣來的。
  #
  # ⚠ 這一條擋的是**下一個** ListView。像素那一條(真的畫、真的數)在
  #   rime_tests.exe 裡,但它只認得現有那兩個;有人第三次寫同一段自繪碼時,
  #   會先撞到這裡。
  check
  local w28; w28="$(hits 'nmcd\.rc')"
  local n28; n28="$(count_of "${w28}")"
  local w28bad=0
  for f in $(printf '%s\n' "${w28}" | grep . | cut -d: -f1 | sort -u); do
    case "${f}" in
      */service/ui_listview.cc) ;;
      */tests/test_win32_listview.cc) ;;
      *) red "W28:${f#${CODE_DIR}/} 直接用 NMCUSTOMDRAW::rc —— 它在 report 模式下是 (0,0,0,0),那一列會畫成空白。改走 RowRect()"; w28bad=1 ;;
    esac
  done
  # ⚠ 範圍非空:一處都沒有的話,不是「都很乾淨」,是掃錯地方了 ——
  #   ui_listview.cc 自己一定用得到它。
  need_scope "W28" "${n28}" 1 || w28bad=1
  [ "${w28bad}" -eq 0 ] && ok "W28 自繪的列矩形只從 RowRect() 來(${n28} 處 nmcd.rc 全在 ui_listview.cc 與它的測試裡)"

  # ── W30:OpenAt() 的引數不可以是字面數字 ──────────────────────
  #
  # status_bar.cc 本來寫的是 `OpenAt(StateIsFailure(...) ? 3 : 0)`,而旁邊
  # 的註解說 3 是「進階」。這一輪 ui_layout.h 把 kPageNetwork 插在
  # kPageAdvanced **前面**(離線為預設的產品,那顆開關不該藏在最後一頁),
  # 於是 3 變成了「連網」—— 出事的時候會把使用者帶到一頁**沒有**
  # 「重新整理字詞」的地方,而畫面上沒有任何東西看起來不對:
  # 每一頁都有名字、每一頁都有內容。
  #
  # 側欄的頁數還會長(§5.3),所以這不是一次性的錯 —— 是一個每次加頁
  # 都會再犯一次的形狀。「哪一頁」永遠要用 common/ui_layout.h 的列舉說。
  # ⚠ 2026-08(#84):這一條**曾經是假綠**。舊判準是一行 grep:
  #     grep -E 'OpenAt\([^)]*[^A-Za-z_0-9][0-9]+'
  #   而唯一的呼叫點長這樣(兩行,而且引數裡自己就有一對括號):
  #       settings_->OpenAt(StateIsFailure(service_state_) ? kPageAdvanced
  #                                                        : kPageSchemas);
  #   `[^)]*` 過不了 `StateIsFailure(service_state_)` 的那個右括號,grep
  #   又是逐行的 —— 所以把兩個引數都改回字面數字之後,那條 grep **一次
  #   都沒有命中**。反向測試當時報 ok,是因為 W29 的常駐紅讓
  #   「rc != 0」自動成立;恆假防護一補上,它立刻現形。
  #   改成括號配對取出整段引數(跨行),再找裸的數字字面。
  check
  local w30bad=0
  local w30out; w30out="$("${PY}" - "${CODE_DIR}" <<'PYSCRIPT'
import os, re, sys
root = sys.argv[1]
n = 0
for dirpath, _, files in os.walk(root):
    for fn in sorted(files):
        if not (fn.endswith('.cc') or fn.endswith('.h')):
            continue
        full = os.path.join(dirpath, fn)
        rel = os.path.relpath(full, root).replace(os.sep, '/')
        txt = open(full, encoding='utf-8', errors='replace').read()
        for m in re.finditer(r'OpenAt\(', txt):
            n += 1
            # 括號配對取出整段引數 —— 跨行,而且容得下巢狀的括號。
            i = m.end() - 1
            depth = 0
            args = ''
            for k in range(i, len(txt)):
                if txt[k] == '(':
                    depth += 1
                elif txt[k] == ')':
                    depth -= 1
                    if depth == 0:
                        args = txt[i + 1:k]
                        break
            line = txt.count('\n', 0, m.start()) + 1
            # 裸的數字字面。`kPage0` 之類的名字不算(前後都要不是識別字)。
            if re.search(r'(?<![A-Za-z_0-9])[0-9]+(?![A-Za-z_0-9])', args):
                flat = ' '.join(args.split())
                print('BAD30=%s:%d OpenAt(%s)' % (rel, line, flat[:80]))
print('N30=%d' % n)
PYSCRIPT
)"
  local n30; n30="$(num "$(printf '%s\n' "${w30out}" | sed -n 's/^N30=//p')")"
  local line30
  while IFS= read -r line30; do
    case "${line30}" in
      BAD30=*)
        red "W30:OpenAt() 的引數裡有字面數字 —— 頁的順序會變(kPageNetwork 這一輪就插進去了)。改用 common/ui_layout.h 的 kPage* 列舉"
        printf '     %s\n' "${line30#BAD30=}" >&2
        w30bad=1 ;;
    esac
  done <<< "${w30out}"
  # ⚠ 範圍非空:宣告 + 定義 + 至少一個呼叫點。掃到零處而報「乾淨」
  #   正是這張檢核表自己最可能的失效方式。
  need_scope "W30" "${n30}" 3 || w30bad=1
  [ "${w30bad}" -eq 0 ] && ok "W30 OpenAt() 的每一處都用列舉說是哪一頁(${n30} 處,引數是跨行括號配對取出來的)"

  # ── W31:清單的「哪一列被選」只准有一份 ──────────────────────
  #
  # #80 的形狀:側欄有兩份「現在是哪一列」—— page_ 驅動內容,comctl32 的
  # LVIS_SELECTED 驅動反白 —— 兩份沒有地方對帳,分岔之後畫面上是**兩列
  # 同時反白**,而 WM_CLOSE 只 SW_HIDE,髒狀態跟著進程活著。
  #
  # ⚠ 這一條之所以存在,是因為上一輪**修好了側欄卻沒有守住它**:
  #   覆核者實跑了兩個植入,兩個都全綠 ——
  #     J:側欄反白改回從 CDIS_SELECTED 畫
  #     K:SelectOnlyRow 拿掉「先全清」
  #   連專門為它寫的 tests/test_win32_sidebar.cc 都抓不到 J(那一支走
  #   自己的 ProbeProc,不是 settings_window.cc 的 DrawSidebar)。
  #   而同一個缺陷在**方案清單**上原封不動地留著。
  #
  # 判準有三條,而且第二條是**跟著程式碼長**的:哪一個清單受管,由
  # SelectOnlyRow() 的呼叫點決定,所以下一個清單一接上去就自動被守住。
  #
  #   1. LVM_SETITEMSTATE 只准出現在 service/ui_listview.cc(單一寫入點)
  #   2. 受管清單的自繪**不可以**從 CDIS_SELECTED 決定反白
  #   2b. **任何**自繪處理常式都不可以讀 NMCUSTOMDRAW::uItemState 的
  #       CDIS_SELECTED —— 要問就問控制項本人
  #       (service/ui_listview.cc 的 RowIsSelected)。
  #   3. SelectOnlyRow 裡「全清」必須在「設定」**之前**
  #
  # ⚠ 2b 以前是 2 的一個**例外**:「沒有人程式化寫它選取的清單不在此限,
  #   那種清單只有一份真相,不可能分岔;連網紀錄那一個就是。」
  #   那個例外是錯的,而錯的地方不是分岔 —— 是那個位元本身。
  #   Windows run #171 上 tests/test_win32_sidebar.cc 走了一次真的自繪:
  #   控制項自己(LVM_GETITEMSTATE / LVM_GETSELECTEDCOUNT)說被選的只有
  #   1 列,而 uItemState **5 列裡說 5 列**都被選。照它畫的話,連網紀錄
  #   是**每一列都反白**;那不是兩份分岔,是直接讀錯。所以例外收掉。
  check
  local w31bad=0
  local w31out; w31out="$("${PY}" - "${CODE_DIR}" <<'PYSCRIPT'
import os, re, sys
root = sys.argv[1]

def read(rel):
    p = os.path.join(root, rel)
    try:
        return open(p, encoding='utf-8', errors='replace').read()
    except OSError:
        return None

def body_after(src, start):
    # 從 start 之後第一個 '{' 起做大括號配對,回傳函式本體。
    i = src.find('{', start)
    if i < 0:
        return ''
    depth = 0
    for j in range(i, len(src)):
        if src[j] == '{':
            depth += 1
        elif src[j] == '}':
            depth -= 1
            if depth == 0:
                return src[i:j + 1]
    return src[i:]

# ── 1. 單一寫入點 ────────────────────────────────────────────
setstate = 0
for dirpath, _, files in os.walk(root):
    for fn in files:
        if not (fn.endswith('.cc') or fn.endswith('.h')):
            continue
        full = os.path.join(dirpath, fn)
        rel = os.path.relpath(full, root)
        txt = open(full, encoding='utf-8', errors='replace').read()
        n = txt.count('LVM_SETITEMSTATE')
        if not n:
            continue
        setstate += n
        if rel.replace(os.sep, '/') != 'service/ui_listview.cc':
            print('BADSET=%s(%d 處)' % (rel.replace(os.sep, '/'), n))
print('NSET=%d' % setstate)

# ── 2. 受管清單 = SelectOnlyRow() 的第一個引數 ────────────────
lv = read('service/ui_listview.cc') or ''
sw = read('service/settings_window.cc') or ''
managed = []
for m in re.finditer(r'SelectOnlyRow\(\s*([A-Za-z_][A-Za-z_0-9]*)\s*,', sw):
    if m.group(1) not in managed:
        managed.append(m.group(1))
print('NMANAGED=%d' % len(managed))
for name in managed:
    print('MANAGED=%s' % name)

# 每一個受管清單,找出「畫它」的那個函式(本體裡有 RowRect(<名字>, cd)。
for name in managed:
    found = False
    for m in re.finditer(r'LRESULT\s+SettingsWindow::(\w+)\s*\(', sw):
        b = body_after(sw, m.end())
        if re.search(r'RowRect\(\s*%s\s*,' % re.escape(name), b):
            found = True
            if 'CDIS_SELECTED' in b:
                print('BADDRAW=%s 在 %s() 裡從 CDIS_SELECTED 畫反白' %
                      (name, m.group(1)))
    if not found:
        print('NODRAW=%s' % name)

# ── 2b. 任何自繪都不准讀 uItemState 的 CDIS_SELECTED ──────────
#   判準與受管與否無關:那個位元在 run #171 上 5 列裡說 5 列被選。
ndraw = 0
for m in re.finditer(r'LRESULT\s+SettingsWindow::(\w+)\s*\(', sw):
    b = body_after(sw, m.end())
    if 'CDDS_ITEMPREPAINT' not in b:
        continue
    ndraw += 1
    if re.search(r'uItemState\s*&\s*CDIS_SELECTED', b):
        print('BADSTATE=%s' % m.group(1))
print('NDRAW=%d' % ndraw)

# ⚠ 反過來也要成立:一個清單的自繪如果從**我們自己的**狀態決定反白
#   (const bool selected = (i == xxx_)),那它就一定要走 SelectOnlyRow ——
#   不然 comctl32 那一份沒有人同步,兩份照樣分岔。而受管名單是從
#   SelectOnlyRow() 的呼叫點數出來的,所以呼叫點一被拿掉,那個清單就
#   **悄悄退出受管**,上面那一圈再也掃不到它(覆核者實測的拆法 N2)。
#   ⚠ 沒有程式化選取的清單不在這一圈裡(它沒有「自己的那一份」可以
#     跟 comctl32 分岔),但它一樣受 2b 管:反白要問控制項本人,
#     不准讀 uItemState 的 CDIS_SELECTED。
for m in re.finditer(r'LRESULT\s+SettingsWindow::(\w+)\s*\(', sw):
    b = body_after(sw, m.end())
    if not re.search(r'const bool selected\s*=\s*\(\s*\w+\s*==\s*\w+_\s*\)', b):
        continue
    for name in re.findall(r'RowRect\(\s*(\w+)\s*,\s*cd', b):
        if name not in managed:
            print('UNMANAGED=%s@%s' % (name, m.group(1)))

# ── 3. 全清必須在設定之前 ────────────────────────────────────
i = lv.find('void SelectOnlyRow(')
if i < 0:
    print('NOFUNC=SelectOnlyRow')
else:
    b = body_after(lv, i)
    clear = b.find('static_cast<WPARAM>(-1)')
    setrow = b.find('static_cast<WPARAM>(row)')
    if clear < 0:
        print('NOCLEAR=1')
    else:
        if setrow < 0:
            print('NOSET=1')
        elif clear > setrow:
            print('ORDER=1')
    # ⚠ 形狀在、順序對,**不等於那一發有作用**。LVM_SETITEMSTATE 只動
    #   stateMask 點名的那幾個位元:stateMask 少了 LVIS_SELECTED,這一發
    #   一個位元都不會改 —— 舊那一列的 LVIS_SELECTED 原封不動留著,
    #   也就是 #80 本人。覆核者實測的拆法 N5 就是只把 clear.stateMask
    #   改成 0,而這一條與 syntax_check_mingw.sh **雙雙 exit 0**。
    for tag, wp in (('CLEAR', '-1'), ('SET', 'row')):
        mm = re.search(r'static_cast<WPARAM>\(' + wp +
                       r'\)[^;]*reinterpret_cast<LPARAM>\(&(\w+)\)', b)
        if not mm:
            continue
        v = re.escape(mm.group(1))
        msk = re.search(r'\b' + v + r'\s*\.\s*stateMask\s*=([^;]*);', b)
        stv = re.search(r'\b' + v + r'\s*\.\s*state\s*=([^;]*);', b)
        if not msk or 'LVIS_SELECTED' not in msk.group(1):
            print('NOMASK=%s' % tag)
        if tag == 'CLEAR' and stv and 'LVIS_SELECTED' in stv.group(1):
            print('CLEARSELECTS=1')
        if tag == 'SET' and (not stv or 'LVIS_SELECTED' not in stv.group(1)):
            print('SETNOSEL=1')
PYSCRIPT
)"
  local nset; nset="$(num "$(printf '%s\n' "${w31out}" | grep '^NSET=' | cut -d= -f2)")"
  local nman; nman="$(num "$(printf '%s\n' "${w31out}" | grep '^NMANAGED=' | cut -d= -f2)")"
  local ndraw; ndraw="$(num "$(printf '%s\n' "${w31out}" | grep '^NDRAW=' | cut -d= -f2)")"
  # ⚠ 範圍非空:兩個都是零的話不是「很乾淨」,是掃錯地方了 ——
  #   ui_listview.cc 自己一定用得到 LVM_SETITEMSTATE,而側欄一定受管。
  need_scope "W31 LVM_SETITEMSTATE" "${nset}" 2 || w31bad=1
  need_scope "W31 受管清單" "${nman}" 2 || w31bad=1
  #   自繪處理常式至少三個(側欄、方案清單、連網紀錄)。掃到 0 個
  #   而報「沒有人讀那個位元」正是 §2-G 的失效方式。
  need_scope "W31 自繪處理常式" "${ndraw}" 3 || w31bad=1
  local w31line
  while IFS= read -r w31line; do
    case "${w31line}" in
      BADSET=*)
        red "W31:${w31line#BADSET=} 自己下 LVM_SETITEMSTATE —— 選取的寫入點只能有一個(service/ui_listview.cc 的 SelectOnlyRow),不然「先全清」保證不了"
        w31bad=1 ;;
      BADDRAW=*)
        red "W31:${w31line#BADDRAW=} —— 那是 comctl32 的那一份,不是我們的。兩份會分岔,而分岔的樣子是兩列同時反白(#80)"
        w31bad=1 ;;
      BADSTATE=*)
        red "W31:${w31line#BADSTATE=}() 從 NMCUSTOMDRAW::uItemState 的 CDIS_SELECTED 決定反白 —— 那個位元不能用(run #171 實測:它 5 列裡說 5 列被選,而控制項自己說 1 列)。問控制項本人:service/ui_listview.cc 的 RowIsSelected()"
        w31bad=1 ;;
      NODRAW=*)
        red "W31:找不到畫 ${w31line#NODRAW=} 的那個函式(預期它的本體裡有 RowRect(${w31line#NODRAW=}, cd))—— 掃描範圍錯了"
        w31bad=1 ;;
      NOFUNC=*)
        red "W31:找不到 SelectOnlyRow 的定義 —— 掃描範圍錯了"
        w31bad=1 ;;
      NOCLEAR=*)
        red "W31:SelectOnlyRow 沒有「先全清」(WPARAM = -1)—— LVS_SINGLESEL 管的是使用者點不出第二個,**不管**程式化的 LVM_SETITEMSTATE"
        w31bad=1 ;;
      ORDER=*)
        red "W31:SelectOnlyRow 的全清跑在設定**之後** —— 等於把剛設好的那一列也清掉"
        w31bad=1 ;;
      NOSET=*)
        red "W31:SelectOnlyRow 只清不設(找不到 static_cast<WPARAM>(row) 那一發)—— 選取被清光之後沒有人補上,那一列永遠不會反白"
        w31bad=1 ;;
      NOMASK=CLEAR*)
        red "W31:SelectOnlyRow 的全清沒有把 LVIS_SELECTED 放進 stateMask —— LVM_SETITEMSTATE 只動 stateMask 點名的位元,這一發一個位元都不會改,舊那一列的反白原封不動留著(#80)"
        w31bad=1 ;;
      NOMASK=SET*)
        red "W31:SelectOnlyRow 設定那一發沒有把 LVIS_SELECTED 放進 stateMask —— 那一發不會碰到選取位元,等於只清不設"
        w31bad=1 ;;
      CLEARSELECTS=*)
        red "W31:SelectOnlyRow 的「全清」把 LVIS_SELECTED 設成 1 了 —— 那是把每一列都選起來,不是清"
        w31bad=1 ;;
      SETNOSEL=*)
        red "W31:SelectOnlyRow 設定那一發的 state 裡沒有 LVIS_SELECTED —— 它只是把那一列的選取關掉,而不是選起來"
        w31bad=1 ;;
      UNMANAGED=*)
        red "W31:${w31line#UNMANAGED=} 的反白從自己的狀態畫,卻沒有走 SelectOnlyRow —— comctl32 那一份沒有人同步,兩份會分岔(#80),而它同時也退出了上面那一圈的掃描範圍"
        w31bad=1 ;;
    esac
  done <<< "${w31out}"
  [ "${w31bad}" -eq 0 ] && ok "W31 清單的選取只有一個寫入點(${nset} 處 LVM_SETITEMSTATE 全在 ui_listview.cc),${nman} 個受管清單的反白都從自己的狀態畫,${ndraw} 個自繪處理常式沒有一個讀 uItemState 的 CDIS_SELECTED"

  # ── W32:介面執行緒上不准出現「會等引擎」的呼叫 ────────────────
  #
  # #79 的根因只有一句話:**UI 執行緒把自己的存活押在引擎的工作何時
  # 回來。** 設定視窗與懸浮那一橫各有自己的訊息迴圈,它們一停,
  # GetMessageW 就再也不會被呼叫 —— 畫面停在最後一格合成的樣子,
  # 任何點擊都沒有人處理,而系統匣圖示也掛在設定視窗那條執行緒上。
  #
  # ⚠ 「有上限的等待」**不算修好**。status_bar.cc 上一輪就是這樣寫的:
  #   `SchemaListForUi(1500, &popup_items_)` —— 那一橫凍結 1.5 秒
  #   (點不動也拖不動),逾時就什麼都不做(按了一下沒事發生),
  #   而且**每按一次就再排一件**進佇列。BeginDeploy 會清方案快取,
  #   所以整個部署期間每一次按都走這條路,可以無限累積。
  #
  # 判準:那兩個檔案裡只准出現不等的那幾支(SchemaListFromCache /
  # RefreshSchemaListAsync / PostAsync),會等的那幾支一律紅。
  # 另外:排非同步查詢之前必須先問過「已經有一件在飛了嗎」。
  check
  local w32bad=0
  local w32out; w32out="$("${PY}" - "${CODE_DIR}" <<'PYSCRIPT'
import os, re, sys
root = sys.argv[1]

UI = ['service/status_bar.cc', 'service/settings_window.cc']
# 會等引擎回來的那幾支。⚠ SchemaListCached() 名字裡有 Cached,但快取冷的
#   時候它會退回同步的 SchemaList() —— 正是最容易被誤放進 UI 的一支。
# ⚠ 一律用 \b 前綴比對。`ReloadSchemaList()` 這個**產品端自己的**函式名
#   裡就含有 `SchemaList()` —— 純子字串比對會把它報成違規,而一條永遠
#   紅的檢查最後一定會被關掉(§3.1 的教訓)。
BLOCKING = [r'\bSchemaListForUi\(', r'\bSchemaListCached\(',
            r'\bSchemaList\(', r'\bWaitDeploy\(']
ASYNC = [r'\bSchemaListFromCache\(', r'\bRefreshSchemaListAsync\(',
         r'\bPostAsync\(']

def read(rel):
    try:
        return open(os.path.join(root, rel), encoding='utf-8',
                    errors='replace').read()
    except OSError:
        return None

nasync = 0
for rel in UI:
    txt = read(rel)
    if txt is None:
        print('NOFILE=%s' % rel)
        continue
    for b in BLOCKING:
        if re.search(b, txt):
            pretty = b.replace('\\b', '').replace('\\(', '(')
            pretty = pretty.replace('\\)', ')')
            print('BLOCKING=%s 裡有 %s' % (rel, pretty))
    for a in ASYNC:
        nasync += len(re.findall(a, txt))
print('NASYNC=%d' % nasync)

# 排隊之前要先問「已經有一件在飛了嗎」。
bar = read('service/status_bar.cc') or ''
i = bar.find('void StatusBar::OpenSchemaPopup(')
if i < 0:
    print('NOOPEN=1')
else:
    j = bar.find('{', i)
    depth = 0
    body = ''
    for k in range(j, len(bar)):
        if bar[k] == '{':
            depth += 1
        elif bar[k] == '}':
            depth -= 1
            if depth == 0:
                body = bar[j:k + 1]
                break
    post = body.find('RefreshSchemaListAsync(')
    gate = body.find('schema_query_inflight_')
    if post < 0:
        print('NOPOST=1')
    elif gate < 0 or gate > post:
        print('NOGATE=1')
PYSCRIPT
)"
  local nasync; nasync="$(num "$(printf '%s\n' "${w32out}" | grep '^NASYNC=' | cut -d= -f2)")"
  # ⚠ 範圍非空:那兩個檔案本來就靠這幾支拿方案清單。掃到零個而報「乾淨」
  #   等於「兩個檔案都被改名了而我沒發現」。
  need_scope "W32 非同步呼叫" "${nasync}" 3 || w32bad=1
  local w32line
  while IFS= read -r w32line; do
    case "${w32line}" in
      BLOCKING=*)
        red "W32:${w32line#BLOCKING=} —— 那一支會等引擎回來,而這個檔案跑在介面執行緒上(#79)。有上限的等待也不行:1.5 秒的凍結仍然是凍結"
        w32bad=1 ;;
      NOFILE=*)
        red "W32:找不到 ${w32line#NOFILE=} —— 掃描範圍錯了"
        w32bad=1 ;;
      NOOPEN=*|NOPOST=*)
        red "W32:找不到 OpenSchemaPopup 裡那次非同步查詢 —— 掃描範圍錯了"
        w32bad=1 ;;
      NOGATE=*)
        red "W32:OpenSchemaPopup 排非同步查詢之前沒有先問 schema_query_inflight_ —— 部署期間快取一直是冷的,每按一次就多排一件沒有人讀的工作"
        w32bad=1 ;;
    esac
  done <<< "${w32out}"
  [ "${w32bad}" -eq 0 ] && ok "W32 介面執行緒上沒有會等引擎的呼叫(${nasync} 處走不等的那幾支),而且方案查詢不會每按一次就多排一件"

  # ── W33:三種不同的情況,畫面上不可以是同一句話 ────────────────
  #
  # `Engine::Post` 以前把 `queue_.Call()` 的 `WorkQueue::Status` 整個丟掉,
  # 而那個 Status 是「有沒有人會做這件事」唯一的答案。後果一路傳到畫面上:
  #
  #   · `SchemaList()` 回一個空 vector,而「引擎在停,這件工作根本沒有
  #     入列」與「一個方案都沒有」是**同一個空 vector**;
  #   · 快取的判準是 `schema_cache_.empty()`,所以「真的一種都沒有」
  #     永遠被當成「還沒問過」—— 每次打開設定都再排一件查詢,
  #     而懸浮那一橫的選單永遠停在「正在讀方案…」;
  #   · 設定視窗把 `RefreshSchemaListAsync` 的回傳值丟掉,於是排不進去
  #     (沒有人會回來)的時候畫面上說的是「目前一種都沒有,到進階按
  #     重新整理字詞」—— 一句對這個情況完全沒有用的話,而且它永遠不會變。
  #
  # 這一條守的就是那條鏈:Status 交得出來、空與無效分得開、三種說法
  # 各有各的字,而那一格的高度從它自己的字算出來(#76 的形狀)。
  check
  local w33bad=0
  local w33out; w33out="$("${PY}" - "${CODE_DIR}" <<'PYSCRIPT'
import os, re, sys
root = sys.argv[1]

def read(rel):
    try:
        return open(os.path.join(root, rel), encoding='utf-8',
                    errors='replace').read()
    except OSError:
        return None

def body_of(txt, sig):
    i = txt.find(sig)
    if i < 0:
        return None
    j = txt.find('{', i)
    depth = 0
    for k in range(j, len(txt)):
        if txt[k] == '{':
            depth += 1
        elif txt[k] == '}':
            depth -= 1
            if depth == 0:
                return txt[j:k + 1]
    return None

eh = read('service/engine.h')
ec = read('service/engine.cc')
sw = read('service/settings_window.cc')
ul = read('common/ui_layout.cc')
for name, txt in (('engine.h', eh), ('engine.cc', ec),
                  ('settings_window.cc', sw), ('ui_layout.cc', ul)):
    if txt is None:
        print('NOFILE=%s' % name)

n_scope = 0
if eh:
    # 1. Post 要把 Status 交出去,不是 void。
    if not re.search(r'WorkQueue::Status Post\(const char\* label', eh):
        print('POST_VOID=1')
    else:
        n_scope += 1
    # 2. 兩支方案清單都要帶 out 參數、回 Status,而且不准被無聲丟掉。
    for fn in ('SchemaList', 'SchemaListCached'):
        m = re.search(r'\[\[nodiscard\]\] WorkQueue::Status %s\(\s*\n?\s*'
                      r'std::vector<std::pair<std::string, std::string>>\* out\);'
                      % fn, eh)
        if not m:
            print('SIG=%s' % fn)
        else:
            n_scope += 1

if ec:
    # 3. 沒跑完就不可以動 out —— 呼叫端讀到的必須是它自己的初始值。
    b = body_of(ec, 'WorkQueue::Status Engine::SchemaList(')
    if b is None:
        print('NOBODY=SchemaList')
    else:
        n_scope += 1
        if 'st == WorkQueue::Status::kDone && out' not in b:
            print('WRITES_ON_FAILURE=1')
    # 4. 「問過了」與「答案不是空的」要分開。
    b = body_of(ec, 'bool Engine::SchemaListFromCache(')
    if b is None:
        print('NOBODY=SchemaListFromCache')
    else:
        n_scope += 1
        if 'schema_cache_valid_' not in b:
            print('CACHE_EMPTY_MEANS_COLD=1')
        if re.search(r'schema_cache_\.empty\(\)', b):
            print('CACHE_STILL_USES_EMPTY=1')
    b = body_of(ec, 'void Engine::InvalidateSchemaCache(')
    if b is None:
        print('NOBODY=InvalidateSchemaCache')
    else:
        n_scope += 1
        if 'schema_cache_valid_ = false;' not in b:
            print('INVALIDATE_LEAVES_VALID=1')

if sw:
    b = body_of(sw, 'void SettingsWindow::ReloadSchemaList(')
    if b is None:
        print('NOBODY=ReloadSchemaList')
    else:
        n_scope += 1
        # 5. 三種說法都要被指派過。少一種 = 那一種在畫面上不存在。
        for note in ('kSchemaNoteEmpty', 'kSchemaNoteLoading',
                     'kSchemaNoteUnavailable'):
            if note not in b:
                print('NOTE_MISSING=%s' % note)
        # 6. 排不進去要看得出來:RefreshSchemaListAsync 的回傳值一定要用。
        if not re.search(r'schema_note_\s*=\s*engine_->RefreshSchemaListAsync', b):
            print('ASYNC_RESULT_DROPPED=1')
        # 7. 那三句話只能從 SchemaNoteLines 來。
        if 'SchemaNoteLines(schema_note_)' not in b:
            print('NOTE_TEXT_HARDCODED=1')
        if re.search(r'UiString::kSchemasEmpty', b):
            print('NOTE_TEXT_PINNED_TO_EMPTY=1')
    b = body_of(sw, 'PageState SettingsWindow::PageStateNow()')
    if b is None:
        print('NOBODY=PageStateNow')
    else:
        n_scope += 1
        if 's.schema_note = schema_note_;' not in b:
            print('NOTE_BRANCH_HARDCODED=1')

if ul:
    # 8. 那一格的高度要從它自己的字算,不是寫死行數(#76 的形狀)。
    i = ul.find('emit(IDC_SCHEMAS_EMPTY')
    if i < 0:
        print('NOEMIT=1')
    else:
        n_scope += 1
        seg = ul[max(0, i - 900):i + 200]
        if 'SchemaNoteLines(state.schema_note)' not in seg:
            print('LAYOUT_IGNORES_NOTE=1')
        if re.search(r'emit\(IDC_SCHEMAS_EMPTY,\s*st\.Push\(\s*t5h\s*\*', ul):
            print('LAYOUT_HARDCODED_LINES=1')
print('NSCOPE=%d' % n_scope)
PYSCRIPT
)"
  local w33n; w33n="$(num "$(printf '%s\n' "${w33out}" | grep '^NSCOPE=' | cut -d= -f2)")"
  # ⚠ 範圍非空:上面每一格都要真的找到那個函式才算數。找到零個而報乾淨,
  #   就是「檔案被改名了而我沒發現」。
  # 9 = engine.h 3(Post + 兩支簽名)+ engine.cc 3 + settings_window.cc 2
  #     + ui_layout.cc 1。少一個就是某個函式被改名/搬走了。
  need_scope "W33 掃到的函式" "${w33n}" 9 || w33bad=1
  local w33line
  while IFS= read -r w33line; do
    case "${w33line}" in
      POST_VOID=*)
        red "W33:Engine::Post 又把 WorkQueue::Status 丟掉了(預期 WorkQueue::Status Post(...))—— 「引擎沒有回應」會再度變成「答案是空的」"
        w33bad=1 ;;
      SIG=*)
        red "W33:Engine::${w33line#SIG=} 的簽名不對 —— 要帶 out 參數、回 WorkQueue::Status,而且 [[nodiscard]](回一個空 vector 就是那個缺陷本身)"
        w33bad=1 ;;
      WRITES_ON_FAILURE=*)
        red "W33:Engine::SchemaList 在工作沒跑完時就動了 *out —— 呼叫端會把自己的初始值當成答案"
        w33bad=1 ;;
      CACHE_EMPTY_MEANS_COLD=*|CACHE_STILL_USES_EMPTY=*)
        red "W33:方案快取又用「是不是空的」當「問過了沒有」—— 真的一種都沒有的使用者會永遠看到「正在讀取」,而每次打開設定都再排一件查詢"
        w33bad=1 ;;
      INVALIDATE_LEAVES_VALID=*)
        red "W33:InvalidateSchemaCache 沒有把 schema_cache_valid_ 收掉 —— 部署完的清空是假的"
        w33bad=1 ;;
      NOTE_MISSING=*)
        red "W33:ReloadSchemaList 裡沒有 ${w33line#NOTE_MISSING=} —— 那一種情況在畫面上不存在,使用者看到的是另外兩種的其中一句"
        w33bad=1 ;;
      ASYNC_RESULT_DROPPED=*)
        red "W33:ReloadSchemaList 丟掉了 RefreshSchemaListAsync 的回傳值 —— 查詢排不進去時沒有人會回來,而畫面上那句話會永遠停在那裡"
        w33bad=1 ;;
      NOTE_TEXT_HARDCODED=*|NOTE_TEXT_PINNED_TO_EMPTY=*)
        red "W33:那一格的三句話不是從 common/ui_layout.h 的 SchemaNoteLines() 來 —— 版面用那一支算高度,兩邊各挑各的就會把字切掉"
        w33bad=1 ;;
      NOTE_BRANCH_HARDCODED=*)
        red "W33:PageStateNow 沒有把 schema_note_ 交給版面(預期 s.schema_note = schema_note_;)—— 三種說法會被照同一種的高度裁"
        w33bad=1 ;;
      LAYOUT_IGNORES_NOTE=*|LAYOUT_HARDCODED_LINES=*)
        red "W33:IDC_SCHEMAS_EMPTY 的高度又寫死行數了 —— #76 的根因就是各段各自寫死,而英文一律比較長"
        w33bad=1 ;;
      NOEMIT=*|NOBODY=*|NOFILE=*)
        red "W33:掃描範圍錯了(${w33line})"
        w33bad=1 ;;
    esac
  done <<< "${w33out}"
  [ "${w33bad}" -eq 0 ] && ok "W33 三種情況(真的沒有 / 還在讀 / 讀不到)在畫面上是三句不同的話,而且那一格的高度跟著它自己的字走"

  # ── W29:連網那一頁,三件事的決定權都不在繪製碼裡 ─────────────
  #
  # 這一頁上有三件事,寫壞了**畫面看起來完全正常**:
  #
  #   1. 開關關著時按「檢查更新」照樣連出去。畫面上多一句「正在檢查
  #      更新…」,而那正是使用者以為不會發生的事。
  #   2. 開關的狀態從 settings_ 讀而不是從 NetGate 讀。兩份真相會漂移,
  #      症狀是「開關看起來開了,按下去卻說被擋下」。
  #   3. 紀錄是空的那一個分支被寫死。使用者看到一個空表格加一顆清除鍵,
  #      而「開關從沒開過所以紀錄是空的」那句話再也說不出口 ——
  #      那句話正是使用者用來驗證我們的方式。
  #
  # ⚠ 所以這一條驗的是**呼叫位置與資料流**,不是「檔案裡有沒有這個字」。
  #   settings_window.cc 在 Ubuntu 上編不起來,那三件事的判斷全部搬到
  #   common/net_ui.cc 與 common/update_flow.cc(純函式,有單元測試);
  #   這裡驗的是那幾條接線真的接上了,而且沒有第二條路繞過去。
  #
  # ⚠ 這一段的 python 失敗時**必須是紅**(見 W25 的教訓),所以它先印
  #   SCOPE_OK;沒有那一行就當作沒跑過。
  #
  # ⚠ 2026-08 修正(#84):這一條有五個子項掃的是 **win-netui 那一版的
  #   名字**(DecideUpdateAction / UpdateThreadEntry / OnUpdateCheckDone /
  #   RunUpdateCheck / UpdateStateText),而產品碼早就改名並且**變強了**
  #   —— 硬執行下沉到 common/net_gate_core.cc 的 RunFetch()(每一跳重問
  #   開關,有測試),呈現面下沉到 common/update_flow.cc。
  #   後果不是「難看」:那 5 條常駐紅讓 `--self-check` 的判準
  #   (「植入之後 rc != 0」)對**每一個**植入自動成立,於是整張反向
  #   測試表變成假綠 —— 覆核者實測植入 J(側欄反白改回從 CDIS_SELECTED
  #   畫)之後,守門的輸出一個位元都沒變。
  #
  #   所以這一輪把錨點換成今天真的承載這頁決定的那幾支,而且順手補上
  #   舊判準**從來沒驗過**的三件:
  #     · 「問過開關」要問在 CreateThread **之前**(存在式 grep 分不出
  #       這兩件事,而問在後面等於沒問)
  #     · `StartUpdateDownload()` 是通往網路的**第二道門**,舊判準只看
  #       `StartUpdateCheck()`
  #     · 工作執行緒要把結果 `PostMessageW(WM_RIME_UPDATE_DONE)` 回 UI
  #       執行緒(settings_window.h:200 明寫「UI 狀態只在 UI 執行緒上動」)
  #
  #   另外收掉三個因為 body_of 回 None 而**永遠跑不到**的死子項,以及
  #   一條假綠(舊的 UPDATE_RUNS_ON_THE_UI_THREAD 掃 `RunUpdateCheck`,
  #   那個名字全樹已經不存在 → 它永遠不會紅。紅的會被查,假綠的不會)。
  check
  local w29bad=0
  local w29out; w29out="$("${PY}" - "${sw}" "${CODE_DIR}/common/net_ui.cc" "${CODE_DIR}/common/update_flow.cc" <<'PYSCRIPT'
import sys as _s
_s.stdout.reconfigure(encoding='utf-8', newline='')
import re, sys
sw = open(sys.argv[1], encoding='utf-8', errors='replace').read()

def slurp(p):
    try:
        return open(p, encoding='utf-8', errors='replace').read()
    except OSError:
        return ''

net = slurp(sys.argv[2])
flow = slurp(sys.argv[3])

def body_of(src, head, endpat='\n}\n'):
    i = src.find(head)
    if i < 0:
        return None
    j = src.find(endpat, i)
    return src[i:j] if j > 0 else src[i:]

def braced_after(src, i):
    # 從 i 之後第一個 '{' 起做大括號配對,回傳那一塊(含大括號)。
    j = src.find('{', i)
    if j < 0:
        return ''
    depth = 0
    for k in range(j, len(src)):
        if src[k] == '{':
            depth += 1
        elif src[k] == '}':
            depth -= 1
            if depth == 0:
                return src[j:k + 1]
    return src[j:]

out = []

# -- 1. 純函式那一側還在 common/ --
#    ⚠ 這裡列的是**今天真的承載這頁決定**的那幾支。舊版列的
#      DecideUpdateAction 全樹已經不存在(見上面的說明),要求一個不存在
#      的東西只會常駐紅,而常駐紅會把整張反向測試表一起變成假綠。
for name in ('UiString NetSwitchSummary(',
             'NetLogView BuildNetLogView('):
    if name not in net:
        out.append('NO_PUREFN=common/net_ui.cc 的 ' + name.split(' ')[-1].rstrip('('))
for name in ('UpdateCard DescribeUpdateCard(',
             'UiString UpdateFailureText(',
             'bool UpdateFailureNeedsSwitch('):
    if name not in flow:
        out.append('NO_PUREFN=common/update_flow.cc 的 ' + name.split(' ')[-1].rstrip('('))

# -- 2. 開關:狀態從 NetGate 讀,那句話從純函式來 --
ref = body_of(sw, 'void SettingsWindow::RefreshNetworkPage() {')
if ref is None:
    out.append('NO_REFRESH')
else:
    if 'net_gate_.Enabled()' not in ref:
        out.append('SWITCH_NOT_FROM_GATE')
    if not re.search(r'SetText\(hwnd_, IDC_NET_STATE, UiText\(NetSwitchSummary\(', ref):
        out.append('SUMMARY_NOT_FROM_FN')
    if not re.search(r'BuildNetLogView\(\s*net_gate_\.ReadLog\(\)', ref):
        out.append('LOG_NOT_FROM_GATE')
    if 'net_log_empty_ = view.empty;' not in ref:
        out.append('EMPTY_NOT_STORED')

# -- 3. 版面的執行期分支:兩格都要是真的狀態 --
ps = body_of(sw, 'PageState SettingsWindow::PageStateNow() const {')
if ps is None:
    out.append('NO_PAGESTATE')
else:
    if 's.net_log_empty = net_log_empty_;' not in ps:
        out.append('EMPTY_BRANCH_HARDCODED')
    if 's.schema_list_empty = order_.empty();' not in ps:
        out.append('SCHEMA_BRANCH_HARDCODED')
n_ps = len(re.findall(r'LayoutSettingsPageDip\([^;]*?PageStateNow\(\)', sw, re.S))
n_all = len(re.findall(r'LayoutSettingsPageDip\(', sw))
if n_all == 0:
    out.append('NO_LAYOUT_CALL')
elif n_ps != n_all:
    out.append('LAYOUT_STATE_BYPASSED=%d/%d' % (n_ps, n_all))

# -- 4. 開關只能走 NetGate::SetEnabled,而且失敗要說出來 --
tog = body_of(sw, 'void SettingsWindow::OnNetSwitchToggled() {')
if tog is None:
    out.append('NO_TOGGLE')
else:
    if 'net_gate_.SetEnabled(on)' not in tog:
        out.append('TOGGLE_NOT_THROUGH_GATE')
    if 'kStatusSaveFailed' not in tog:
        out.append('TOGGLE_FAILS_SILENTLY')

# -- 5. 通往網路的**兩道門**:開關先問,問在建執行緒之前,而且真的收手 --
#    ⚠ 位置式,不是存在式。「問過開關」在問的位置錯掉時毫無意義,
#      而純存在式的 grep 分不出這兩件事。
#    ⚠ 兩個都驗:StartUpdateDownload 是第二道門,舊判準從來沒看過它。
for fn in ('StartUpdateCheck', 'StartUpdateDownload'):
    up = body_of(sw, 'void SettingsWindow::%s() {' % fn)
    if up is None:
        out.append('NO_STARTER=' + fn)
        continue
    i_gate = up.find('net_gate_.Enabled()')
    i_thread = up.find('CreateThread')
    if i_gate < 0:
        out.append('UPDATE_SKIPS_THE_SWITCH=' + fn)
    if i_thread < 0:
        out.append('UPDATE_NOT_ON_A_THREAD=' + fn)
    if i_gate >= 0 and i_thread >= 0 and i_gate > i_thread:
        out.append('SWITCH_ASKED_TOO_LATE=' + fn)
    if 'UpdateFailure::kSwitchOff' not in up:
        out.append('NO_SWITCHOFF_REASON=' + fn)
    # 那個不成立的分支要真的收手 —— 算了判斷卻照跑是「看起來有做」。
    i_if = up.find('if (!net_gate_.Enabled())')
    if i_if < 0:
        if i_gate >= 0:
            out.append('UPDATE_IGNORES_THE_VERDICT=' + fn)
    else:
        blk = braced_after(up, i_if)
        if 'return;' not in blk:
            out.append('UPDATE_IGNORES_THE_VERDICT=' + fn)
    # 阻塞的那兩支不可以直接在 UI 執行緒上呼叫。
    if 'update_.Check(' in up or 'update_.DownloadAndVerify(' in up:
        out.append('UPDATE_RUNS_ON_THE_UI_THREAD=' + fn)

# -- 6. 那條背景執行緒:兩條路都不是死的,而且結果要回 UI 執行緒 --
th = body_of(sw, 'DWORD WINAPI SettingsWindow::UpdateWorkerEntry(LPVOID')
if th is None:
    out.append('NO_UPDATE_THREAD')
else:
    if 'update_.Check(' not in th:
        out.append('THREAD_DOES_NOT_CHECK')
    if 'update_.DownloadAndVerify(' not in th:
        out.append('THREAD_DOES_NOT_DOWNLOAD')
    if 'PostMessageW(' not in th or 'WM_RIME_UPDATE_DONE' not in th:
        out.append('THREAD_DOES_NOT_POST_BACK')

# -- 7. 回來之後:只動狀態,然後把整頁重算一次 --
done = body_of(sw, 'void SettingsWindow::OnUpdateWorkerDone() {')
if done is None:
    out.append('NO_UPDATE_DONE')
elif 'RefreshNetworkAndUpdateCard()' not in done:
    out.append('RESULT_DOES_NOT_REFRESH')

card = body_of(sw, 'void SettingsWindow::RefreshNetworkAndUpdateCard() {')
if card is None:
    out.append('NO_UPDATE_CARD')
else:
    if 'RefreshNetworkPage()' not in card:
        out.append('CARD_DOES_NOT_REFRESH_LOG')
    if 'DescribeUpdateCard(' not in card:
        out.append('RESULT_TEXT_HARDCODED')
    if 'st.network_enabled = on;' not in card:
        out.append('CARD_NOT_FROM_GATE')
    if re.search(r'SetText\(hwnd_,\s*IDC_UPDATE_STATUS,\s*UiText\(UiString::k', card):
        out.append('RESULT_TEXT_HARDCODED')

# -- 6. 清除紀錄要問一聲 --
clr = body_of(sw, 'void SettingsWindow::DoClearNetLog() {')
if clr is None:
    out.append('NO_CLEAR')
else:
    if 'ConfirmDialog(' not in clr:
        out.append('CLEAR_WITHOUT_CONFIRM')
    if 'net_gate_.ClearLog()' not in clr:
        out.append('CLEAR_DOES_NOTHING')

print('SCOPE_OK')
print('LAYOUTCALLS=%d' % n_all)
for line in out:
    print(line)
PYSCRIPT
)"
  w29msg() { red "W29:$1"; w29bad=1; }
  case "${w29out}" in
    SCOPE_OK*) ;;
    *) w29msg "這一條的掃描程式根本沒跑完(沒有 SCOPE_OK)。實際輸出:
${w29out}" ;;
  esac
  local w29calls; w29calls="$(num "$(printf '%s\n' "${w29out}" | sed -n 's/^LAYOUTCALLS=//p')")"
  need_scope "W29 版面呼叫點" "${w29calls}" 2 || w29bad=1
  local line29
  while IFS= read -r line29; do
    line29="${line29%$'\r'}"
    case "${line29}" in
      ''|SCOPE_OK|LAYOUTCALLS=*) continue ;;
      NO_PUREFN=*)
        w29msg "找不到 ${line29#NO_PUREFN=} —— 這一頁的判斷又回到繪製碼裡了,
     而 service/ 底下的東西在 Ubuntu 上編不起來(= 實際上沒有人驗得到)" ;;
      NO_REFRESH)  w29msg "找不到 RefreshNetworkPage —— 掃描範圍錯了" ;;
      SWITCH_NOT_FROM_GATE)
        w29msg "開關的狀態不是從 NetGate::Enabled() 讀的 —— 兩份真相會漂移,
     而漂移的樣子是「開關看起來開了,按下去卻說被擋下」" ;;
      SUMMARY_NOT_FROM_FN)
        w29msg "開關底下那一句話不是 NetSwitchSummary() 給的(預期
     SetText(hwnd_, IDC_NET_STATE, UiText(NetSwitchSummary(...))))——
     寫死一句的話,開與關會說同一句話" ;;
      LOG_NOT_FROM_GATE)
        w29msg "連網紀錄不是從 net_gate_.ReadLog() 經 BuildNetLogView() 來的 ——
     自己拼一份的話,那一份與紀錄檔會不一樣" ;;
      EMPTY_NOT_STORED)
        w29msg "讀完紀錄之後沒有把 view.empty 存回 net_log_empty_ ——
     版面那個分支永遠停在初值" ;;
      NO_PAGESTATE) w29msg "找不到 PageStateNow —— 掃描範圍錯了" ;;
      EMPTY_BRANCH_HARDCODED)
        w29msg "版面的「紀錄是空的」分支被寫死(預期 s.net_log_empty = net_log_empty_;)
     —— 一次都沒有連過的使用者會看到一個空表格加一顆清除鍵,
     而「開關從沒開過所以紀錄是空的」那句話就再也說不出口了" ;;
      SCHEMA_BRANCH_HARDCODED)
        w29msg "版面的「一種方式都沒有」分支被寫死(預期 s.schema_list_empty = order_.empty();)" ;;
      NO_LAYOUT_CALL) w29msg "settings_window.cc 裡找不到 LayoutSettingsPageDip 的呼叫 —— 掃描範圍錯了" ;;
      LAYOUT_STATE_BYPASSED=*)
        w29msg "有 LayoutSettingsPageDip 的呼叫沒有走 PageStateNow()(${line29#LAYOUT_STATE_BYPASSED=})
     —— 繞過去的那一個會用一份與畫面無關的狀態排版" ;;
      NO_TOGGLE) w29msg "找不到 OnNetSwitchToggled —— 掃描範圍錯了" ;;
      TOGGLE_NOT_THROUGH_GATE)
        w29msg "開關不是寫進 NetGate::SetEnabled() —— 出口那一側讀到的仍然是舊值" ;;
      TOGGLE_FAILS_SILENTLY)
        w29msg "開關寫不進去時沒有告訴使用者 —— 症狀會是「開關關了,重開又是開的」" ;;
      NO_STARTER=*) w29msg "找不到 ${line29#NO_STARTER=} —— 掃描範圍錯了" ;;
      UPDATE_SKIPS_THE_SWITCH=*)
        w29msg "${line29#UPDATE_SKIPS_THE_SWITCH=}() 沒有問過 net_gate_.Enabled() ——
     **開關關著也會連出去**。這是這一頁上最嚴重的一種寫壞法,而且
     畫面上看起來完全正常(只多一句「正在檢查更新…」)" ;;
      SWITCH_ASKED_TOO_LATE=*)
        w29msg "${line29#SWITCH_ASKED_TOO_LATE=}() 問了開關,但問在 CreateThread **之後** ——
     執行緒已經上路了,問了也沒有用。位置錯掉的守門與沒有守門是同一件事" ;;
      NO_SWITCHOFF_REASON=*)
        w29msg "${line29#NO_SWITCHOFF_REASON=}() 的擋下分支沒有 UpdateFailure::kSwitchOff ——
     使用者會拿到「連不上伺服器」,而真正的原因是他自己把開關關著" ;;
      UPDATE_IGNORES_THE_VERDICT=*)
        w29msg "${line29#UPDATE_IGNORES_THE_VERDICT=}() 問了開關卻沒有在那個分支裡 return ——
     算了判斷照跑,是「看起來有做」的典型" ;;
      UPDATE_NOT_ON_A_THREAD=*)
        w29msg "${line29#UPDATE_NOT_ON_A_THREAD=}() 沒有開背景執行緒 —— 同步阻塞跑在
     UI 執行緒上就是「打字打到一半整個沒反應」(候選窗與設定視窗共用那條執行緒)" ;;
      UPDATE_RUNS_ON_THE_UI_THREAD=*)
        w29msg "${line29#UPDATE_RUNS_ON_THE_UI_THREAD=}() 裡直接呼叫了 update_.Check() /
     update_.DownloadAndVerify() —— 那兩支是阻塞的,見上一條" ;;
      NO_UPDATE_THREAD) w29msg "找不到 UpdateWorkerEntry —— 掃描範圍錯了" ;;
      THREAD_DOES_NOT_CHECK)
        w29msg "背景執行緒沒有呼叫 update_.Check() —— 檢查那一條路是死的" ;;
      THREAD_DOES_NOT_DOWNLOAD)
        w29msg "背景執行緒沒有呼叫 update_.DownloadAndVerify() —— 下載那一條路是死的" ;;
      THREAD_DOES_NOT_POST_BACK)
        w29msg "背景執行緒沒有 PostMessageW(WM_RIME_UPDATE_DONE) 回 UI 執行緒 ——
     settings_window.h:200 明寫「所有 UI 狀態只在 UI 執行緒上動」,
     在工作執行緒上動畫面是「偶爾正常、偶爾畫一半」那一種" ;;
      NO_UPDATE_DONE) w29msg "找不到 OnUpdateWorkerDone —— 掃描範圍錯了" ;;
      NO_UPDATE_CARD) w29msg "找不到 RefreshNetworkAndUpdateCard —— 掃描範圍錯了" ;;
      RESULT_TEXT_HARDCODED)
        w29msg "更新卡片那一句話不是 DescribeUpdateCard() 算出來的(或者有人直接把
     字面餵給 IDC_UPDATE_STATUS)—— 五種結果會被壓成同一句,
     使用者拿到別人的錯誤訊息" ;;
      CARD_NOT_FROM_GATE)
        w29msg "卡片吃的不是 NetGate 的真值(預期 st.network_enabled = on;)——
     開關與卡片會漂移,症狀是「開關關著,按鈕卻是亮的」" ;;
      CARD_DOES_NOT_REFRESH_LOG)
        w29msg "RefreshNetworkAndUpdateCard 沒有先 RefreshNetworkPage() ——
     使用者按完更新,人就在這一頁上,卻看不到剛剛那幾筆連線,
     而那幾筆正是這一頁存在的理由" ;;
      RESULT_DOES_NOT_REFRESH)
        w29msg "OnUpdateWorkerDone 沒有呼叫 RefreshNetworkAndUpdateCard() ——
     工作回來了,畫面停在「正在檢查更新…」" ;;
      NO_CLEAR) w29msg "找不到 DoClearNetLog —— 掃描範圍錯了" ;;
      CLEAR_WITHOUT_CONFIRM)
        w29msg "清除連網紀錄沒有確認 —— 那份紀錄是使用者用來稽核我們的證據,
     清掉就找不回來了(§2-C2/§2-C3)" ;;
      CLEAR_DOES_NOTHING)
        w29msg "「清除連網紀錄」沒有呼叫 net_gate_.ClearLog()" ;;
      *) w29msg "未知的回報:${line29}" ;;
    esac
  done <<< "${w29out}"
  [ "${w29bad}" -eq 0 ] && ok "W29 連網那一頁:開關讀寫都走 NetGate、那幾句話與空狀態分支都從純函式來、${w29calls} 個版面呼叫點全部走 PageStateNow(),通往網路的兩道門都在 CreateThread **之前**問過開關並收手,而工作執行緒把結果 Post 回 UI 執行緒"

  # ── W34:「已送出」→「已套用 / 套用失敗」那一條線 ──────────────
  #
  # #79 的後半段:引擎的套用是**非同步**的,按下去的當下沒有人知道
  # 結果。舊版在呼叫點就直接寫「已套用」—— 那是在替一件還躺在佇列裡
  # 的工作背書,而工作真的失敗時畫面上仍然是那句成功。
  #
  # ⚠ 這一條之所以存在,是因為上一輪**修好了卻沒有守住**。覆核者實跑
  #   五個植入,把最關鍵的修正一個一個還原回去 ——
  #     A1 呼叫點改回無條件 SetTransientStatus(kStatusApplied)
  #     A2 Engine::ApplyVariantAll 拿掉「沒排進去也要說」
  #     A3 4 秒計時器改回無條件清空
  #     A4 心跳解除改回無條件清空
  #     A5 拿掉序號守衛
  #   ——三支守門**五個全綠**,因為整張檢核表裡連 BeginApply 這個字都
  #   沒有出現過。
  #
  # 四條判準,分母全部從程式碼數出來(下一個非同步套用點一接上去就
  # 自動被守住):
  #
  #   1. 每一個 engine_->ApplyVariantAll / SetOptionAll 呼叫點,都要先
  #      BeginApply() 拿序號、把 ApplyDoneNotifier(**那個**序號)傳進去,
  #      而且那一段之後**不准再說一次成功**(SetTransientStatus)。
  #   2. Engine 那兩支:PostAsync 沒排進去(!queued)也要 on_done(false)。
  #      工作根本不會發生,而畫面上那句「正在套用…」會永遠停在那裡。
  #   3. 每一張票(StatusLine::Ticket)都要有人讀;而「收回訊息」那兩處
  #      不得無條件清空 —— 4 秒的計時器會把使用者剛拿到的紅字一起收走,
  #      而且不留任何痕跡。
  #   4. 序號守衛:連按三下時,前兩次的結果不可以寫進那一行。
#
# ⚠ **以上四條只守了去程。** 覆核者第二輪實跑三個單行還原,三個全綠:
#     B1 ApplyDoneNotifier 的本體換成空的(名字還在,PostMessageW 沒了)
#     B4 WndProc 的 case WM_RIME_APPLY_DONE 不再呼叫 OnApplyDone
#     B5 OnApplyDone 成功那一支換成 (void)apply_ok_status_;
#   而 BeginApply() 是**故意** KillTimer 的,所以少了回程,那一行會
#   **永遠**停在「已送出,正在套用…」—— 不是 4 秒後消失,是不會消失。
#   B1/B4 連失敗那一句紅字也一起沒了。
#   更糟的是那一輪的守門把「4 個非同步套用點全部走 BeginApply() +
#   ApplyDoneNotifier(同一個序號)」原樣印出來 —— **綠燈斷言的正好是
#   當下已經壞掉的那個性質**。這與本條檔頭自己寫的存在理由(「上一輪
#   修好了卻沒有守住」)是同一個形狀,只是往下游移了一格。
#
# 所以下面第 5、6 條**不針對 WM_RIME_APPLY_DONE 寫**,而是讓判準跟著
# 「訊息從送出到畫面」這條鏈自己長:
#
#   5. settings_window.cc 裡**每一則** WM_RIME_xxx 都是一條鏈 ——
#        有人送出(PostMessageW / SendMessageW,或交給 OS 當回呼)
#      → WndProc 的 case 真的把它交出去(而不是一個空的 case)
#      → 收下的那支處理常式**走得到**一次真正的視窗寫入
#        (::SetWindowTextW / ::InvalidateRect / …,深度 5 以內)。
#      呼叫圖是從檔案本身建出來的,所以 SetStatus() 被掏空成
#      「記一筆 ticket 但不動控制項」也會紅 —— 那正是下游的下一格。
#      新增一則 WM_RIME_xxx,這三格自動跟著長出來。
#   6. **每一支回傳 std::function 的成員(通知工廠)**,回傳的那個
#      lambda 本體裡都要有 PostMessageW/SendMessageW —— 它存在的唯一
#      理由就是把結果搬回 UI 執行緒。B1 溜過去的方式是「名字在、本體
#      空」,而只 grep 一個函式名字的判準對這個形狀是瞎的。
#   7. OnApplyDone **兩支各自**都要寫畫面:失敗那一支要寫一句話,
#      成功那一支要寫的是 BeginApply() 當初挑好的 apply_ok_status_
#      (寫死成 kStatusApplied 的話,重設與「跟著輸入法語言」那兩個
#      呼叫點的措辭就丟了)。
#
# ⚠ **這一條守到哪一格為止**:靜態掃描能走到「處理常式 → … →
#   ::SetWindowTextW(控制項)」。**下一格**是「那個控制項在版面上看不
#   看得見」—— IDC_STATUS 有沒有被捲出可視範圍、有沒有被 ShowWindow
#   藏起來、字有沒有被截掉。那要版面計算或實跑才知道,不是這裡做得到
#   的;W25(捲動)與 W11/W24(版面)守的是它,但沒有人把兩邊接起來。
#   再下一格是「使用者看到的那句話是不是對的那句」—— UiText() 的內容
#   由 W7/W8/W23 守。
  check
  local w34bad=0
  local w34out; w34out="$("${PY}" - "${CODE_DIR}" <<'PYSCRIPT'
import os, re, sys
root = sys.argv[1]

def read(rel):
    try:
        return open(os.path.join(root, rel), encoding='utf-8',
                    errors='replace').read()
    except OSError:
        return None

sw = read('service/settings_window.cc')
hh = read('service/settings_window.h')
en = read('service/engine.cc')
if sw is None or hh is None or en is None:
    print('NOSRC=1')
    raise SystemExit(0)

def match_from(src, i):
    # 從 src[i] 這個 '{' 起做配對,回傳 (開, 閉+1)。
    depth = 0
    for j in range(i, len(src)):
        if src[j] == '{':
            depth += 1
        elif src[j] == '}':
            depth -= 1
            if depth == 0:
                return (i, j + 1)
    return (i, len(src))

def body_after(src, start):
    i = src.find('{', start)
    return match_from(src, i) if i >= 0 else (0, 0)

def enclosing_block(src, pos):
    # 往回找最內層還沒閉合的 '{'。
    depth = 0
    i = pos
    while i > 0:
        i -= 1
        c = src[i]
        if c == '}':
            depth += 1
        elif c == '{':
            if depth == 0:
                return match_from(src, i)
            depth -= 1
    return (0, len(src))

def args_at(src, lparen):
    depth = 0
    for j in range(lparen, len(src)):
        if src[j] == '(':
            depth += 1
        elif src[j] == ')':
            depth -= 1
            if depth == 0:
                return src[lparen + 1:j], j + 1
    return src[lparen + 1:], len(src)

def where(src, pos):
    # 最近的一個 SettingsWindow::xxx( —— 行號在去註解後的樹上沒有意義。
    last = 'settings_window.cc'
    for m in re.finditer(r'SettingsWindow::(\w+)\s*\(', src[:pos]):
        last = m.group(1) + '()'
    return last

# ── 1. 非同步套用的每一個呼叫點 ────────────────────────────────
sites = list(re.finditer(r'engine_->(ApplyVariantAll|SetOptionAll)\s*\(', sw))
print('NAPPLY=%d' % len(sites))
for m in sites:
    args, end = args_at(sw, m.end() - 1)
    w = where(sw, m.start())
    ob, cb = enclosing_block(sw, m.start())
    lead = sw[ob:m.start()]
    seqs = re.findall(r'(\w+)\s*=\s*BeginApply\s*\(', lead)
    if 'ApplyDoneNotifier(' not in args:
        print('NONOTIFY=%s' % w)
        continue
    if not seqs:
        print('NOBEGIN=%s' % w)
    elif not re.search(r'ApplyDoneNotifier\(\s*%s\s*\)' % re.escape(seqs[-1]),
                       args):
        print('WRONGSEQ=%s' % w)
    if 'SetTransientStatus(' in sw[end:cb]:
        print('SAYSOK=%s' % w)

# ── 2. Engine:沒排進去也要說 ──────────────────────────────────
for fn in ('SetOptionAll', 'ApplyVariantAll'):
    i = en.find('void Engine::%s(' % fn)
    if i < 0:
        print('NOENGFN=%s' % fn)
        continue
    ob, cb = body_after(en, i)
    b = en[ob:cb]
    if not re.search(r'bool\s+queued\s*=\s*PostAsync\s*\(', b):
        print('NOQUEUED=%s' % fn)
    if not re.search(r'if\s*\(\s*!\s*queued[^)]*\)\s*\{?\s*(?:if[^;]*\)\s*)?'
                     r'on_done\(false\)', b):
        print('NOTELL=%s' % fn)

# ── 3. 票要有人讀,收回訊息不得無條件 ──────────────────────────
tickets = re.findall(r'StatusLine::Ticket\s+(\w+)\s*=', hh)
print('NTICKET=%d' % len(tickets))
for t in tickets:
    if not re.search(r'StillShowing\(\s*(?:self->)?%s\s*\)' % re.escape(t), sw):
        print('UNREAD=%s' % t)
wipes = list(re.finditer(r'SetStatus\(\s*std::wstring\(\)\s*\)', sw))
print('NWIPE=%d' % len(wipes))
for m in wipes:
    lead = sw[max(0, m.start() - 400):m.start()]
    if '_ticket_' in lead and 'StillShowing(' not in lead:
        print('BAREWIPE=%s' % where(sw, m.start()))

# ── 4. 序號守衛 ───────────────────────────────────────────────
i = sw.find('void SettingsWindow::OnApplyDone(')
if i < 0:
    print('NOAPPLYDONE=1')
else:
    ob, cb = body_after(sw, i)
    b = sw[ob:cb]
    if not (re.search(r'if\s*\(\s*seq\s*!=\s*apply_seq_\s*\)\s*return\s*;', b) or
            re.search(r'if\s*\(\s*apply_seq_\s*!=\s*seq\s*\)\s*return\s*;', b)):
        print('NOSEQGUARD=1')
j = sw.find('unsigned SettingsWindow::BeginApply(')
if j < 0:
    print('NOBEGINDEF=1')
else:
    ob, cb = body_after(sw, j)
    if '++apply_seq_' not in sw[ob:cb]:
        print('NOBUMP=1')

# ── 5/6/7. 回程:訊息送出 → WndProc → 處理常式 → 畫面 ──────────

UIWRITE = re.compile(
    r'::(?:SetWindowTextW|SetDlgItemTextW|InvalidateRect|RedrawWindow|'
    r'ShowWindow|SetWindowPos|MoveWindow|DrawTextW|SendMessageW|'
    r'EnableWindow|TrackPopupMenu|SetForegroundWindow|SetActiveWindow|'
    r'DestroyWindow|Shell_NotifyIconW|SetFocus)\s*\(')

KEYWORDS = frozenset(('if', 'for', 'while', 'switch', 'catch', 'return',
                      'sizeof', 'do', 'else', 'case', 'and', 'or', 'not'))

def args_end(src, lparen, limit=4000):
    # 有界的配對:字串裡的孤兒括號不會讓它掃到檔尾。
    depth = 0
    stop = min(len(src), lparen + limit)
    for j in range(lparen, stop):
        c = src[j]
        if c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
            if depth == 0:
                return j + 1
    return -1

def index_functions(src):
    # 「名字( 引數 ) {」而且不是關鍵字 —— 自由函式與成員定義都收。
    # 呼叫點後面是 ';' 不是 '{',所以不會混進來;lambda 的 '(' 前面
    # 是 ']',連名字都取不到。
    idx = {}
    for m in re.finditer(r'\b(\w+)\s*\(', src):
        name = m.group(1)
        if name in KEYWORDS:
            continue
        after = args_end(src, m.end() - 1)
        if after < 0:
            continue
        mm = re.match(r'\s*(?:const\s*)?(?:noexcept\s*)?\{', src[after:after + 40])
        if not mm:
            continue
        b0, b1 = match_from(src, after + mm.end() - 1)
        idx.setdefault(name, []).append(src[b0:b1])
    return idx

FNS = index_functions(sw)

def reaches_screen(name, depth=0, seen=None):
    if seen is None:
        seen = set()
    if depth > 5 or name in seen or name not in FNS:
        return False
    seen.add(name)
    for b in FNS[name]:
        if UIWRITE.search(b):
            return True
        for c in re.findall(r'\b(\w+)\s*\(', b):
            if c not in KEYWORDS and reaches_screen(c, depth + 1, seen):
                return True
    return False

msgs = re.findall(r'constexpr UINT (WM_RIME_\w+)\s*=\s*WM_APP', sw)
print('NMSG=%d' % len(msgs))
wpb = max(FNS.get('WndProc', ['']), key=len)
for msg in msgs:
    posted = re.search(
        r'::(?:Post|Send)(?:Thread)?MessageW\s*\([^;]{0,240}\b%s\b' % msg, sw)
    handed = re.search(r'uCallbackMessage\s*=\s*%s\b' % msg, sw)
    if not posted and not handed:
        print('NOPOST=%s' % msg)
    cm = re.search(r'case\s+%s\s*:' % msg, wpb)
    if not cm:
        print('NOCASE=%s' % msg)
        continue
    nxt = re.search(r'(?m)^\s*(?:case\s|default\s*:)', wpb[cm.end():])
    arm = wpb[cm.end():cm.end() + (nxt.start() if nxt else len(wpb))]
    handlers = re.findall(r'self->(\w+)\s*\(', arm)
    if UIWRITE.search(arm):
        continue
    if not handlers:
        print('NODISPATCH=%s' % msg)
        continue
    if not any(reaches_screen(h) for h in handlers):
        print('NOSCREEN=%s|%s' % (msg, ','.join(handlers)))

notifiers = list(re.finditer(r'std::function<[^>]*>\s+SettingsWindow::(\w+)\s*\(',
                             sw))
print('NNOTIF=%d' % len(notifiers))
for m in notifiers:
    ob, cb = body_after(sw, m.start())
    b = sw[ob:cb]
    lam = re.search(r'return\s*\[[^\]]*\]\s*\([^)]*\)\s*(?:mutable\s*)?\{', b)
    if not lam:
        print('NOLAMBDA=%s' % m.group(1))
        continue
    lb0, lb1 = match_from(b, lam.end() - 1)
    if not re.search(r'::(?:Post|Send)(?:Thread)?MessageW\s*\(', b[lb0:lb1]):
        print('EMPTYNOTIFIER=%s' % m.group(1))

i = sw.find('void SettingsWindow::OnApplyDone(')
if i >= 0:
    ob, cb = body_after(sw, i)
    b = sw[ob:cb]
    fm = re.search(r'if\s*\(\s*!\s*ok\s*\)\s*\{', b)
    if not fm:
        print('NOFAILBRANCH=1')
    else:
        f0, f1 = match_from(b, fm.end() - 1)
        if not re.search(r'Set(?:Transient)?Status\s*\(', b[f0:f1]):
            print('FAILNOWRITE=1')
        if not re.search(r'Set(?:Transient)?Status\s*\(\s*apply_ok_status_\s*\)',
                         b[f1:]):
            print('OKNOWRITE=1')
PYSCRIPT
)"
  local napply; napply="$(num "$(printf '%s\n' "${w34out}" | grep '^NAPPLY=' | cut -d= -f2)")"
  local ntkt;   ntkt="$(num "$(printf '%s\n' "${w34out}" | grep '^NTICKET=' | cut -d= -f2)")"
  local nwipe;  nwipe="$(num "$(printf '%s\n' "${w34out}" | grep '^NWIPE=' | cut -d= -f2)")"
  # ⚠ 分母:四個套用點(變體、標點、跟著輸入法語言、重設)、兩張票、
  #   兩處收回。掃不到就是範圍寫錯了,而那必須是紅的。
  need_scope "W34 非同步套用呼叫點" "${napply}" 4 || w34bad=1
  need_scope "W34 狀態列的票" "${ntkt}" 2 || w34bad=1
  need_scope "W34 收回訊息的地方" "${nwipe}" 2 || w34bad=1
  local nmsg;   nmsg="$(num "$(printf '%s\n' "${w34out}" | grep '^NMSG=' | cut -d= -f2)")"
  local nnotif; nnotif="$(num "$(printf '%s\n' "${w34out}" | grep '^NNOTIF=' | cut -d= -f2)")"
  # ⚠ 分母:settings_window.cc 目前有 7 則自訂訊息、1 支通知工廠。
  #   掃不到就是**判準跟著程式碼一起被拆了**,而那必須是紅的。
  need_scope "W34 自訂訊息" "${nmsg}" 7 || w34bad=1
  need_scope "W34 通知工廠" "${nnotif}" 1 || w34bad=1
  local w34line
  while IFS= read -r w34line; do
    case "${w34line}" in
      NOSRC=*)
        red "W34:找不到 service/settings_window.{cc,h} 或 service/engine.cc —— 掃描範圍錯了"
        w34bad=1 ;;
      NONOTIFY=*)
        red "W34:${w34line#NONOTIFY=} 的非同步套用沒有把 ApplyDoneNotifier() 傳進去 —— 沒有人會回來換掉「正在套用…」,而呼叫點當下說的任何一句成功都是在替佇列裡的工作背書(#79)"
        w34bad=1 ;;
      NOBEGIN=*)
        red "W34:${w34line#NOBEGIN=} 沒有先 BeginApply() —— 那一句「已送出,正在套用…」是使用者唯一看得到的「我收到了」"
        w34bad=1 ;;
      WRONGSEQ=*)
        red "W34:${w34line#WRONGSEQ=} 傳給 ApplyDoneNotifier() 的不是這一次 BeginApply() 拿到的序號 —— 序號守衛會把它整個丟掉,結果是那一行永遠停在「正在套用…」"
        w34bad=1 ;;
      SAYSOK=*)
        red "W34:${w34line#SAYSOK=} 在送出之後又自己說了一次成功(SetTransientStatus)—— 工作還躺在佇列裡,那句話沒有根據"
        w34bad=1 ;;
      NOENGFN=*)
        red "W34:engine.cc 裡找不到 Engine::${w34line#NOENGFN=} —— 掃描範圍錯了"
        w34bad=1 ;;
      NOQUEUED=*)
        red "W34:Engine::${w34line#NOQUEUED=} 沒有接住 PostAsync 的回傳值 —— 「排不進去」與「排進去了」在呼叫端看起來會一模一樣"
        w34bad=1 ;;
      NOTELL=*)
        red "W34:Engine::${w34line#NOTELL=} 在工作沒排進佇列時不通知 on_done(false) —— 那件事永遠不會發生,而畫面上那句「正在套用…」會永遠停在那裡"
        w34bad=1 ;;
      UNREAD=*)
        red "W34:${w34line#UNREAD=} 寫了卻沒有任何人用 StillShowing() 讀它 —— 收回訊息就變成無條件清空,使用者剛拿到的紅字會被上一則的計時器一起收走"
        w34bad=1 ;;
      BAREWIPE=*)
        red "W34:${w34line#BAREWIPE=} 拿著一張票卻無條件把狀態列清空 —— 要先問過 StillShowing():畫面上那一則已經不是自己寫的了就不准動它"
        w34bad=1 ;;
      NOAPPLYDONE=*)
        red "W34:找不到 SettingsWindow::OnApplyDone —— 掃描範圍錯了"
        w34bad=1 ;;
      NOSEQGUARD=*)
        red "W34:OnApplyDone 少了序號守衛(seq != apply_seq_ 就 return)—— 連按三下時前兩次的結果會蓋掉最後那一下,而使用者關心的正好是最後那一下"
        w34bad=1 ;;
      NOBEGINDEF=*)
        red "W34:找不到 SettingsWindow::BeginApply —— 掃描範圍錯了"
        w34bad=1 ;;
      NOBUMP=*)
        red "W34:BeginApply 沒有把 apply_seq_ 往前推 —— 序號守衛擋不掉任何東西"
        w34bad=1 ;;
      NOPOST=*)
        red "W34:${w34line#NOPOST=} 沒有任何人送出它(PostMessageW/SendMessageW,或交給 OS 當回呼)—— 收訊那一端的 case 會**永遠不會被叫到**,而發訊那一支的名字還在,grep 一個函式名字看不出來"
        w34bad=1 ;;
      NOCASE=*)
        red "W34:WndProc 裡沒有 case ${w34line#NOCASE=} —— 送出去的那一則沒有人接,DefWindowProc 會安靜地吃掉它"
        w34bad=1 ;;
      NODISPATCH=*)
        red "W34:WndProc 的 case ${w34line#NODISPATCH=} 是空的 —— 沒有交給任何處理常式、也沒有自己動畫面。訊息照送、case 照在,而畫面上那一行永遠停在送出當下那句話(BeginApply() 是故意 KillTimer 的,所以它不會自己消失)"
        w34bad=1 ;;
      NOSCREEN=*)
        red "W34:${w34line%%|*} 交給了 ${w34line#*|},而那一支(往下 5 層)走不到任何一次真正的視窗寫入 —— 結果換回來了卻沒有換到畫面上"
        w34bad=1 ;;
      NOLAMBDA=*)
        red "W34:SettingsWindow::${w34line#NOLAMBDA=} 回傳 std::function 卻不是 return [捕捉](…){…} 的形狀 —— 掃描範圍錯了"
        w34bad=1 ;;
      EMPTYNOTIFIER=*)
        red "W34:SettingsWindow::${w34line#EMPTYNOTIFIER=} 回傳的 lambda 本體裡沒有 PostMessageW/SendMessageW —— **名字在、本體空**。引擎會乖乖呼叫它,而那一下什麼都不會發生"
        w34bad=1 ;;
      NOFAILBRANCH=*)
        red "W34:OnApplyDone 裡找不到 if (!ok) 那一支 —— 失敗與成功走同一條路,而使用者只會看到成功"
        w34bad=1 ;;
      FAILNOWRITE=*)
        red "W34:OnApplyDone 的失敗那一支沒有把任何一句話寫上去 —— 那一行會停在「已送出,正在套用…」,而工作其實已經失敗了"
        w34bad=1 ;;
      OKNOWRITE=*)
        red "W34:OnApplyDone 的成功那一支沒有 Set(Transient)Status(apply_ok_status_) —— 要嘛沒把「正在套用…」換掉(它不會自己消失),要嘛把 BeginApply() 當初挑好的措辭丟了(重設是「已重設」、跟著輸入法語言是另一句)"
        w34bad=1 ;;
    esac
  done <<< "${w34out}"
  [ "${w34bad}" -eq 0 ] && ok "W34 去程:${napply} 個非同步套用點全部走 BeginApply() + ApplyDoneNotifier(同一個序號),Engine 那兩支排不進佇列時也會說;回程:${nmsg} 則自訂訊息每一則都有人送出、WndProc 的 case 都真的交出去、收下的那支都走得到一次視窗寫入,${nnotif} 支通知工廠回傳的 lambda 本體裡都真的有 PostMessageW,OnApplyDone 擋得掉過期的回覆而且成功/失敗兩支各自都寫了畫面;${ntkt} 張票都有人讀、${nwipe} 處收回訊息都先問過 StillShowing()"

  # ── W35:失敗訊息不會自己消失 ──────────────────────────────────
  #
  # 設定視窗底部那一行有兩種寫法:SetStatus() 寫上去就留著,
  # SetTransientStatus() 4 秒之後自己清掉(§12.5.3:成功訊息不值得一個
  # 新表面,所以借用那一行,再自己收回去)。
  #
  # ⚠ **失敗不准借那條路。** 使用者沒有讀完的權利被 4 秒剝奪掉,而失敗
  #   訊息消失之後畫面上是一片空白 —— 那跟「成功了」長得一模一樣。
  #   這個檔案裡曾經只有一句失敗走 transient(kStatusApplyFailed),
  #   而其他每一句(kStatusSaveFailed ×10、kStatusRedeployFailed、
  #   kStatusOrderNotApplied)都是 SetStatus。偏偏那一句是最長的
  #   (英文兩行),又正好出現在使用者剛被告知「已送出,正在套用…」、
  #   最可能把視線移開的那一刻。
  #
  # 兩條路都要堵:直接傳給 SetTransientStatus() 的,以及從 BeginApply()
  # 那一頭經由 apply_ok_status_ 繞進來的。
  check
  local w35bad=0
  local w35out; w35out="$("${PY}" - "${CODE_DIR}" <<'PYSCRIPT'
import os, re, sys
root = sys.argv[1]
try:
    sw = open(os.path.join(root, 'service/settings_window.cc'),
              encoding='utf-8', errors='replace').read()
except OSError:
    print('NOSRC=1')
    raise SystemExit(0)

def args_at(src, lparen):
    depth = 0
    for j in range(lparen, len(src)):
        if src[j] == '(':
            depth += 1
        elif src[j] == ')':
            depth -= 1
            if depth == 0:
                return src[lparen + 1:j]
    return src[lparen + 1:]

FAILNAME = re.compile(r'\bk\w*(?:Failed|NotApplied)\w*\b')
ntrans = 0
# ⚠ (?<![:\w]):定義那一行是 SettingsWindow::SetTransientStatus(UiString s),
#   前面是冒號。把它算進去的話,分母會多一個永遠乾淨的假樣本。
for m in re.finditer(r'(?<![:\w])(SetTransientStatus|BeginApply)\s*\(', sw):
    if m.group(1) == 'SetTransientStatus':
        ntrans += 1
    for bad in FAILNAME.findall(args_at(sw, m.end() - 1)):
        print('TRANSIENTFAIL=%s@%s' % (bad, m.group(1)))
print('NTRANS=%d' % ntrans)
print('NPERM=%d' % len(re.findall(
    r'SetStatus\(\s*UiString::k\w*(?:Failed|NotApplied)\w*\s*\)', sw)))
PYSCRIPT
)"
  local ntrans; ntrans="$(num "$(printf '%s\n' "${w35out}" | grep '^NTRANS=' | cut -d= -f2)")"
  local nperm;  nperm="$(num "$(printf '%s\n' "${w35out}" | grep '^NPERM=' | cut -d= -f2)")"
  # ⚠ 兩個分母都要非空:一邊是零就代表這一條沒有在比較任何東西。
  need_scope "W35 會自己消失的訊息" "${ntrans}" 6 || w35bad=1
  need_scope "W35 留在畫面上的失敗訊息" "${nperm}" 8 || w35bad=1
  local w35line
  while IFS= read -r w35line; do
    case "${w35line}" in
      NOSRC=*)
        red "W35:找不到 service/settings_window.cc —— 掃描範圍錯了"
        w35bad=1 ;;
      TRANSIENTFAIL=*)
        local w35n="${w35line#TRANSIENTFAIL=}"
        case "${w35n}" in
          *@BeginApply)
            red "W35:${w35n%%@*} 被當成 BeginApply() 的成功文案 —— 那一句最後會走 SetTransientStatus(apply_ok_status_),失敗訊息從這一頭混進去,結果一樣是 4 秒後自己消失" ;;
          *)
            red "W35:${w35n%%@*} 走了 SetTransientStatus —— 失敗訊息 4 秒後自己消失,使用者回頭看到的是一片空白,而空白跟成功長得一模一樣" ;;
        esac
        w35bad=1 ;;
    esac
  done <<< "${w35out}"
  [ "${w35bad}" -eq 0 ] && ok "W35 ${ntrans} 句會自己消失的訊息裡沒有一句是失敗,${nperm} 句失敗訊息全部留在畫面上等使用者自己看完"

  # ── W36:詞庫檔被改寫的那一刻,一個 session 都不可以在 ────────────
  #
  # #90。librime 的 dict_compiler 重編時第一件事是刪掉 *.table.bin /
  # *.prism.bin,而 Windows 不允許刪除或 resize 一個還有 section mapping
  # 掛著的檔案 —— 於是那一發失敗,而**回傳值沒有人看**
  # (dict_compiler.cc:265 / :358)。接下來 MappedFile::Create() 落進
  # overwriting 分支,Resize 一樣失敗、一樣沒有人看,最後它用讀寫模式
  # 重新映射舊檔,把新表寫進一段還有 session 正在讀的記憶體。
  #
  # ⚠ **這種壞法在執行期看不出來。** librime 從頭到尾回報成功,使用者
  #   那一側是打字打到一半候選變成亂碼、或者服務直接消失。所以這條線
  #   沒有「跑一次看看」這個選項,只能靠呼叫點的形狀守住。
  #
  # ⚠ 而且它有一半只有 Windows 驗得到(mmap 的行為)。純函式那一半在
  #   common/redeploy_flow.{h,cc} + tests/test_redeploy_flow.cc;
  #   **這裡守的是接線**:那個階段機有沒有真的被接在 engine.cc 上。
  #
  # 六條判準,分母全部從程式碼數出來:
  #
  #   1. 每一個 rs_session_create() 呼叫點,所在的函式都要在呼叫**之前**
  #      問過 SessionCreationAllowed()。一個例外都沒有 —— 有例外就得
  #      維護一份名單,而名單會過期。
  #   2. BeginDeploy 不可以自己呼叫 rs_deploy()(它跑在設定視窗的 UI
  #      執行緒上),要把「收乾淨 + 開始部署」整包丟給引擎執行緒;
  #      而那一包裡收乾淨要在 rs_deploy() **之前**、拒絕啟動時要重建、
  #      而且要讓畫面知道這一場結束了。
  #   3. CloseAllSessionsOnEngineThread 要真的 destroy 並清空 sessions_。
  #   4. OnDeployTerminal(部署終局)要重建 —— 而且**不可以**掛在設定
  #      視窗的計時器上:那個視窗可以被關掉。
  #   5. RebuildSessionsOnEngineThread 要重套方案與選項(#85)。
  #   6. ProcessKey / ToggleAsciiMode 那道門要用 ShouldFailOpen(phase_...),
  #      而且在 ProcessKey 裡要排在 Find() **之前**;兩支都要排在
  #      Post() **之前** —— 那道門只讀兩個 atomic,要在呼叫端執行緒上答,
  #      不可以排在引擎那條唯一的 FIFO 後面(理由見下面那一條紅字)。
  check
  local w36bad=0
  local w36out; w36out="$("${PY}" - "${CODE_DIR}" <<'PYSCRIPT'
import os, re, sys
root = sys.argv[1]

def read(rel):
    try:
        return open(os.path.join(root, rel), encoding='utf-8',
                    errors='replace').read()
    except OSError:
        return None

en = read('service/engine.cc')
if en is None:
    print('NOSRC=1')
    raise SystemExit(0)

def match_from(src, i):
    depth = 0
    for j in range(i, len(src)):
        if src[j] == '{':
            depth += 1
        elif src[j] == '}':
            depth -= 1
            if depth == 0:
                return (i, j + 1)
    return (i, len(src))

def body_of(src, sig):
    i = src.find(sig)
    if i < 0:
        return None
    b = src.find('{', i)
    if b < 0:
        return None
    ob, cb = match_from(src, b)
    return src[ob + 1:cb - 1]

def where(src, pos):
    last = None
    for m in re.finditer(r'Engine::(\w+)\s*\(', src[:pos]):
        last = m.group(1)
    return last or '(檔案開頭)'

# ── 1. 每一個 rs_session_create() 都要先問過門 ────────────────────
creates = list(re.finditer(r'rs_session_create\s*\(', en))
print('NCREATE=%d' % len(creates))
for m in creates:
    fn = where(en, m.start())
    bd = body_of(en, 'Engine::%s(' % fn)
    if bd is None:
        print('NOFN=%s' % fn)
        continue
    i_gate = bd.find('SessionCreationAllowed(')
    i_new = bd.find('rs_session_create(')
    if i_gate < 0:
        print('UNGATED=%s' % fn)
    elif i_gate > i_new:
        print('GATEAFTERCREATE=%s' % fn)

# ── 2. BeginDeploy 只負責關門與丟工作 ────────────────────────────
bd = body_of(en, 'bool Engine::BeginDeploy(')
if bd is None:
    print('NOBEGINDEPLOY=1')
else:
    if 'kRequested' not in bd:
        print('NOREQUESTED=1')
    if 'rs_deploy(' in bd:
        print('DEPLOYONCALLERTHREAD=1')
    if 'PostAsync(' not in bd:
        print('NOASYNC=1')
    if 'kTeardownFailed' not in bd:
        print('NOOPENBACK=1')

# ── 2b. 收乾淨 + 開始部署那一包 ──────────────────────────────────
cd2 = body_of(en, 'void Engine::CloseThenDeployOnEngineThread(')
if cd2 is None:
    print('NOCLOSEDEPLOYFN=1')
else:
    i_close = cd2.find('CloseAllSessionsOnEngineThread()')
    i_deploy = cd2.find('rs_deploy(')
    if i_close < 0:
        print('NOCLOSESTEP=1')
    elif i_deploy < 0:
        print('NODEPLOYCALL=1')
    elif i_close > i_deploy:
        print('DEPLOYBEFORECLOSE=1')
    elif 'RebuildSessionsOnEngineThread()' not in cd2[i_deploy:]:
        print('NOREBUILD=CloseThenDeployOnEngineThread')
    if 'kSessionsClosed' not in cd2:
        print('NOCLOSEDEVENT=1')
    if 'deploy_seq_' not in cd2:
        print('NOTELLUI=1')

# ── 3. 收乾淨那一支 ──────────────────────────────────────────────
cb = body_of(en, 'void Engine::CloseAllSessionsOnEngineThread(')
if cb is None:
    print('NOCLOSEFN=1')
else:
    if 'rs_session_destroy(' not in cb:
        print('NODESTROY=1')
    if 'sessions_.clear()' not in cb:
        print('NOCLEAR=1')

# ── 4. 部署終局要重建 ────────────────────────────────────────────
ob = body_of(en, 'void Engine::OnDeployTerminal(')
if ob is None:
    print('NOTERMINALFN=1')
else:
    if 'kDeployFinished' not in ob:
        print('NOFINISHEVENT=1')
    if 'RebuildSessionsAsync()' not in ob:
        print('NOREBUILD=OnDeployTerminal')

# ── 5. 重建要重套設定(#85)──────────────────────────────────────
rb = body_of(en, 'void Engine::RebuildSessionsOnEngineThread(')
if rb is None:
    print('NOREBUILDFN=1')
else:
    if 'planner_(' not in rb:
        print('NOPLANNER=1')
    # ⚠ 這裡問的是 SelectAndApply,不是 rs_select_schema。
    #   winbar 併進來之後 engine.cc 裡**只准有一個**裸的 rs_select_schema,
    #   而那一個在 Engine::SelectAndApply 裡(audit_single_source.sh 規則 2)
    #   —— 換方案會把 switches 重設回方案宣告的值,所以「選方案」與
    #   「重套簡繁」被綁成一個不可分割的動作。
    #   照舊掃 rs_select_schema 的話,這一條與那條規則**互相矛盾**:
    #   一邊要求重建裡有裸呼叫,另一邊禁止它。兩邊要的性質其實是同一個
    #   ——「重建之後使用者的方案與選項都回得來」—— 所以判準跟著搬家。
    if 'SelectAndApply(' not in rb or 'rs_set_option(' not in rb:
        print('NOREAPPLY=1')

# ── 6. 按鍵那道門 ────────────────────────────────────────────────
gates = ('ProcessKey', 'ToggleAsciiMode')
print('NGATE=%d' % len(gates))
for fn in gates:
    gb = body_of(en, 'Engine::%s(' % fn)
    if gb is None:
        print('NOGATEFN=%s' % fn)
        continue
    i_gate = gb.find('ShouldFailOpen(')
    if i_gate < 0:
        print('NOGATE=%s' % fn)
        continue
    if not re.search(r'ShouldFailOpen\(\s*phase_\.load\(\)', gb):
        print('STALEPHASE=%s' % fn)
    i_find = gb.find('Find(id)')
    if i_find >= 0 and i_find < i_gate:
        print('GATEAFTERFIND=%s' % fn)
    i_post = gb.find('Post(')
    if i_post >= 0 and i_post < i_gate:
        print('GATEBEHINDQUEUE=%s' % fn)
    if re.search(r'deploy_state_\.load\(\)\s*!=\s*1', gb):
        print('OLDJUDGE=%s' % fn)
PYSCRIPT
)"
  local ncreate; ncreate="$(num "$(printf '%s\n' "${w36out}" | grep '^NCREATE=' | cut -d= -f2)")"
  local ngate;   ngate="$(num "$(printf '%s\n' "${w36out}" | grep '^NGATE=' | cut -d= -f2)")"
  # ⚠ 分母:三個建 session 的呼叫點(當場建、備用池、部署後重建)、
  #   兩道按鍵的門。掃不到就是範圍寫錯,而那必須是紅的。
  need_scope "W36 建 session 的呼叫點" "${ncreate}" 3 || w36bad=1
  need_scope "W36 按鍵那道門" "${ngate}" 2 || w36bad=1
  local w36line
  while IFS= read -r w36line; do
    case "${w36line}" in
      NOSRC=*)
        red "W36:找不到 service/engine.cc —— 掃描範圍錯了"; w36bad=1 ;;
      NOFN=*)
        red "W36:找不到 Engine::${w36line#NOFN=} 的本體 —— 掃描範圍錯了"; w36bad=1 ;;
      UNGATED=*)
        red "W36:Engine::${w36line#UNGATED=} 建 session 之前沒有問過 SessionCreationAllowed() —— 部署期間建起來的 session 會對正在被改寫的 *.table.bin 開一個 mmap,而 librime 刪不掉舊檔時**不會報錯**,它會把新表寫進那段記憶體"; w36bad=1 ;;
      GATEAFTERCREATE=*)
        red "W36:Engine::${w36line#GATEAFTERCREATE=} 問門問在 rs_session_create() **之後** —— session 已經建出來了,那一問擋不掉任何東西"; w36bad=1 ;;
      NOBEGINDEPLOY=*)
        red "W36:找不到 Engine::BeginDeploy —— 掃描範圍錯了"; w36bad=1 ;;
      NOCLOSESTEP=*)
        red "W36:BeginDeploy 裡沒有「部署前收乾淨 session」那一步 —— 部署會在活著的 session 腳下抽換詞庫檔(#90 的本體)"; w36bad=1 ;;
      NODEPLOYCALL=*)
        red "W36:BeginDeploy 裡找不到 rs_deploy() —— 掃描範圍錯了"; w36bad=1 ;;
      DEPLOYBEFORECLOSE=*)
        red "W36:BeginDeploy 先呼叫 rs_deploy() 才去收 session —— 順序反了就等於沒收"; w36bad=1 ;;
      DEPLOYONCALLERTHREAD=*)
        red "W36:BeginDeploy 自己呼叫了 rs_deploy() —— 那一支跑在設定視窗的 UI 執行緒上,而收 session(把使用者剛學到的詞寫回去)必須排在它前面。在那條執行緒上同步等就是 #79 的形狀"; w36bad=1 ;;
      NOASYNC=*)
        red "W36:BeginDeploy 沒有把「收乾淨 + 開始部署」丟給引擎執行緒(PostAsync)—— 見上一條"; w36bad=1 ;;
      NOOPENBACK=*)
        red "W36:BeginDeploy 在工作沒排進佇列時沒有把門打開(kTeardownFailed)—— session 一個都沒收掉,而那道門會一直關著,引擎再起來也沒有人去開它"; w36bad=1 ;;
      NOCLOSEDEPLOYFN=*)
        red "W36:找不到 Engine::CloseThenDeployOnEngineThread —— 掃描範圍錯了"; w36bad=1 ;;
      NOCLOSEDEVENT=*)
        red "W36:CloseThenDeployOnEngineThread 沒有走 AdvanceRedeploy(kSessionsClosed) —— 階段沒有進到 kDeploying,而那是「詞庫檔現在可以被改寫」唯一的說法"; w36bad=1 ;;
      NOTELLUI=*)
        red "W36:CloseThenDeployOnEngineThread 在 rs_deploy() 拒絕啟動時沒有推 deploy_seq_ —— 設定視窗會永遠停在「正在整理字詞…已耗時 N 秒」,而那個數字一直往上跳"; w36bad=1 ;;
      NOREQUESTED=*)
        red "W36:BeginDeploy 沒有走 AdvanceRedeploy(kRequested) —— 那道門沒有關上,收乾淨與 rs_deploy() 之間新開的程式會拿到一個活過整場部署的 session"; w36bad=1 ;;
      NOREBUILD=*)
        red "W36:Engine::${w36line#NOREBUILD=} 沒有把 session 建回來(RebuildSessionsAsync)—— 使用者手上會是一個一個 session 都沒有的引擎,從此打不出中文,而且重開機也沒用"; w36bad=1 ;;
      NOCLOSEFN=*)
        red "W36:找不到 Engine::CloseAllSessionsOnEngineThread —— 掃描範圍錯了"; w36bad=1 ;;
      NODESTROY=*)
        red "W36:CloseAllSessionsOnEngineThread 沒有 rs_session_destroy —— mmap 沒有放掉,而且使用者剛學到的詞不會落地(destroy_session 才是它落地的時機)"; w36bad=1 ;;
      NOCLEAR=*)
        red "W36:CloseAllSessionsOnEngineThread 沒有清空 sessions_ —— 裡面留著的是已經失效的指標"; w36bad=1 ;;
      NOTERMINALFN=*)
        red "W36:找不到 Engine::OnDeployTerminal —— 掃描範圍錯了"; w36bad=1 ;;
      NOFINISHEVENT=*)
        red "W36:OnDeployTerminal 沒有走 AdvanceRedeploy(kDeployFinished) —— 階段會永遠停在 kDeploying,而那道門是關的"; w36bad=1 ;;
      NOREBUILDFN=*)
        red "W36:找不到 Engine::RebuildSessionsOnEngineThread —— 掃描範圍錯了"; w36bad=1 ;;
      NOPLANNER=*)
        red "W36:重建沒有問過 planner_ —— 建 session 時套什麼與重建時套什麼會變成兩份,而漂移是靜默的(#85)"; w36bad=1 ;;
      NOREAPPLY=*)
        red "W36:重建沒有重套方案與選項(SelectAndApply / rs_set_option)—— 使用者按一次「重新整理字詞」,他釘的方案、選的簡繁、剛切到的英數全部回到預設,而畫面上寫的是「完成」(#85)。⚠ 走 SelectAndApply 而不是裸的 rs_select_schema:engine.cc 裡只准有一個裸呼叫點,見 audit_single_source.sh 規則 2"; w36bad=1 ;;
      NOGATEFN=*)
        red "W36:找不到 Engine::${w36line#NOGATEFN=} —— 掃描範圍錯了"; w36bad=1 ;;
      NOGATE=*)
        red "W36:Engine::${w36line#NOGATE=} 沒有走 ShouldFailOpen() —— 按鍵會被交給一個正在被抽換的詞庫"; w36bad=1 ;;
      STALEPHASE=*)
        red "W36:Engine::${w36line#STALEPHASE=} 傳給 ShouldFailOpen() 的不是 phase_.load() —— 那道門讀不到重新部署走到哪一格,等於永遠開著"; w36bad=1 ;;
      GATEAFTERFIND=*)
        red "W36:Engine::${w36line#GATEAFTERFIND=} 的門排在 Find() 之後 —— 重新部署期間 session 是真的不見了,先 Find() 就會走進「找不到」那條路,回給宿主一份 status_flags 全 0 的快照,而狀態列會把它當成「一切正常」"; w36bad=1 ;;
      GATEBEHINDQUEUE=*)
        red "W36:Engine::${w36line#GATEBEHINDQUEUE=} 的門排在 Post() 之後 —— 那道門會在**引擎執行緒**上答,而且排在「收乾淨 session 再開始部署」那一包後面(每一個 rs_session_destroy 都要把使用者詞典寫回去,是這條路上最慢的一步);Engine::Post 是 queue_.Call(...,0) = **永遠等**。而 DLL 那一側每顆鍵的預算是 50 毫秒(tsf/ipc_client.cc 的 kKeyTimeoutMs)—— 逾時就 Fail(kTimeout) → Close()、session_ = 0,**整條連線被丟掉**,於是部署後「保住原 id、重套方案 / 簡繁 / 英數」那一套(#85)對那個宿主不生效"; w36bad=1 ;;
      OLDJUDGE=*)
        red "W36:Engine::${w36line#OLDJUDGE=} 還在用 deploy_state_ != 1 判斷 —— 那個值首次部署成功之後**永遠**是 1,重新部署期間這道門是開的(#90 的原始形狀)"; w36bad=1 ;;
    esac
  done <<< "${w36out}"
  [ "${w36bad}" -eq 0 ] && ok "W36 ${ncreate} 個建 session 的呼叫點全部先問過 SessionCreationAllowed(),BeginDeploy 不在 UI 執行緒上部署而是整包丟給引擎執行緒,那一包收乾淨才 rs_deploy()、拒絕啟動時會建回來也會讓畫面知道,部署終局那一條不靠任何視窗,重建會重套方案與選項,${ngate} 道按鍵的門都讀 phase_、排在 Find() 之前、而且排在 Post() 之前(在呼叫端執行緒上答,不排在收 session 那一包後面)"
  # ── W37:部署回呼碰的那個 Engine,活多久 ─────────────────────────
  #
  # OnDeploy 跑在 **librime 的部署執行緒**上,而 Engine 是 main 那條執行緒
  # 上的物件。舊版是一個裸指標配一個 null 檢查,而 check 與 use 之間不是
  # 原子的:讀到非空之後,那個物件仍然可能在回呼正在用它的時候被
  # ~Engine() 拆掉。
  #
  # ⚠ **換成 std::atomic<Engine*> 不算修好。** 那只讓「讀到的是不是
  #   nullptr」變成定義良好,對「讀出來之後」一個字都沒說。要的是生命
  #   週期保證:關門的人要等到裡面沒有人 —— common/callback_gate.h,
  #   由 tests/test_callback_gate.cc 在 Ubuntu 上驗(--asan 之下,天真的
  #   atomic 版在那裡是 heap-use-after-free)。**這裡守的是接線。**
  check
  local w37bad=0
  local w37out; w37out="$("${PY}" - "${CODE_DIR}" <<'PYSCRIPT'
import os, re, sys
root = sys.argv[1]

try:
    en = open(os.path.join(root, 'service/engine.cc'), encoding='utf-8',
              errors='replace').read()
except OSError:
    print('NOSRC=1')
    raise SystemExit(0)

def match_from(src, i):
    depth = 0
    for j in range(i, len(src)):
        if src[j] == '{':
            depth += 1
        elif src[j] == '}':
            depth -= 1
            if depth == 0:
                return (i, j + 1)
    return (i, len(src))

def body_of(src, sig):
    i = src.find(sig)
    if i < 0:
        return None
    b = src.find('{', i)
    if b < 0:
        return None
    ob, cb = match_from(src, b)
    return src[ob + 1:cb - 1]

# 分母:部署回呼的註冊點。掃空 = 範圍寫錯,那必須是紅的。
print('NREG=%d' % len(re.findall(r'\.on_deploy\s*=', en)))

if 'CallbackGate<Engine>' not in en:
    print('NOGATEOBJ=1')
if re.search(r'std::atomic\s*<\s*Engine\s*\*\s*>', en):
    print('ATOMICONLY=1')
if re.search(r'^\s*Engine\s*\*\s*g_\w+', en, re.M):
    print('RAWGLOBAL=1')

ob = body_of(en, 'void OnDeploy(')
if ob is None:
    print('NOCBFN=1')
else:
    if 'g_deploy_gate.Run(' not in ob:
        print('NOGATERUN=1')
    if re.search(r'\brs_[a-z_]+\s*\(', ob):
        print('RSINCALLBACK=1')

sb = body_of(en, 'bool Engine::Start(')
if sb is None:
    print('NOSTARTFN=1')
elif 'g_deploy_gate.Open(this)' not in sb:
    print('NOOPEN=1')

tb = body_of(en, 'void Engine::Stop(')
if tb is None:
    print('NOSTOPFN=1')
else:
    i_close = tb.find('g_deploy_gate.Close()')
    if i_close < 0:
        print('NOCLOSE=1')
    else:
        i_started = tb.find('if (!started_)')
        i_queue = tb.find('queue_.Stop()')
        i_final = tb.find('rs_finalize(')
        if 0 <= i_started < i_close:
            print('CLOSEAFTERSTARTEDCHECK=1')
        if 0 <= i_queue < i_close:
            print('CLOSEAFTERQUEUE=1')
        if 0 <= i_final < i_close:
            print('CLOSEAFTERFINALIZE=1')
PYSCRIPT
)"
  local nreg; nreg="$(num "$(printf '%s\n' "${w37out}" | grep '^NREG=' | cut -d= -f2)")"
  need_scope "W37 部署回呼的註冊點" "${nreg}" 1 || w37bad=1
  local w37line
  while IFS= read -r w37line; do
    case "${w37line}" in
      NOSRC=*)
        red "W37:找不到 service/engine.cc —— 掃描範圍錯了"; w37bad=1 ;;
      NOCBFN=*)
        red "W37:找不到 OnDeploy() 的本體 —— 掃描範圍錯了"; w37bad=1 ;;
      NOSTARTFN=*)
        red "W37:找不到 Engine::Start 的本體 —— 掃描範圍錯了"; w37bad=1 ;;
      NOSTOPFN=*)
        red "W37:找不到 Engine::Stop 的本體 —— 掃描範圍錯了"; w37bad=1 ;;
      NOGATEOBJ=*)
        red "W37:engine.cc 裡沒有 CallbackGate<Engine> —— 部署回呼手上那個 Engine 沒有任何生命週期保證,~Engine() 可以在它正在用的時候把它拆掉"; w37bad=1 ;;
      ATOMICONLY=*)
        red "W37:那個指標是 std::atomic<Engine*> —— **換型別不夠**。它只讓「是不是 nullptr」這一問變成定義良好,而回呼在 load() 與 -> 之間可以被排掉任意久,那段時間足夠 main 跑完 Stop() 與 ~Engine()(見 common/callback_gate.h)"; w37bad=1 ;;
      RAWGLOBAL=*)
        red "W37:engine.cc 裡有一個裸的 Engine* 全域指標 —— check 與 use 之間不是原子的,而且連「讀得乾淨」都沒有"; w37bad=1 ;;
      NOGATERUN=*)
        red "W37:OnDeploy 沒有走 g_deploy_gate.Run() —— 它拿到的 Engine 沒有人保證還活著"; w37bad=1 ;;
      RSINCALLBACK=*)
        red "W37:OnDeploy 裡呼叫了 rs_* —— rime_shell 呼叫這個回呼時**持有** g_global_mutex(#91),那是一個真的死鎖;而且它是在閘的鎖裡面跑的,做得久就是讓 Stop() 陪著等"; w37bad=1 ;;
      NOOPEN=*)
        red "W37:Engine::Start 沒有開閘(g_deploy_gate.Open(this))—— 部署的終局到不了引擎,session 不會被建回來,使用者從此打不出中文"; w37bad=1 ;;
      NOCLOSE=*)
        red "W37:Engine::Stop 沒有關閘 —— ~Engine() 之後,librime 的部署執行緒還拿著這個物件在寫"; w37bad=1 ;;
      CLOSEAFTERSTARTEDCHECK=*)
        red "W37:Stop() 的關閘排在 if (!started_) 之後 —— Start() 在 rs_init 失敗時 started_ 仍然是 false 而閘已經開了(它必須開在 rs_init 之前),那一問會先 return,閘上就留著一個指向即將被解構的 Engine 的指標"; w37bad=1 ;;
      CLOSEAFTERQUEUE=*)
        red "W37:Stop() 的關閘排在 queue_.Stop() 之後 —— 佇列排乾的那一段裡,部署終局的回呼會走 RebuildSessionsAsync() 排不進去的那條路去清 parked_,而工作者可能正在 RebuildSessionsOnEngineThread() 裡讀它"; w37bad=1 ;;
      CLOSEAFTERFINALIZE=*)
        red "W37:Stop() 的關閘排在 rs_finalize() 之後 —— 那之後才關等於沒關"; w37bad=1 ;;
    esac
  done <<< "${w37out}"
  [ "${w37bad}" -eq 0 ] && ok "W37 部署回呼走 CallbackGate:OnDeploy 只在閘裡動 atomic 與排一件工作(沒有 rs_*),Start 開閘、Stop 的**第一句**就關閘(排在 started_ 那一問與 queue_.Stop() 之前)—— Stop() 返回時「沒有回呼還在用這個 Engine」是被鎖保證的"

  # ── W38:候選窗的滾輪翻頁,整條鏈都要接著 ─────────────────────
  #
  # ⚠ 這一條守的**不是點,是鏈**。G71 之前的狀態正是這個專案最會出的那種:
  #   `Op::kChangePage` 在 protocol.h 上、pipe_server 解得出來、
  #   Engine::ChangePage 會呼叫 rs_change_page、ipc_client 也有
  #   SendChangePage —— **整個 windows/ 底下一個呼叫點都沒有**。
  #   每一段都寫好了,只是沒有人按得到它,而只有使用者的手指找得到這件事。
  #
  #   所以這裡逐段問:訊息收得到嗎 → 收到之後走純函式判斷嗎 →
  #   判斷完交得出去嗎 → 有人接嗎 → 接的人真的翻頁嗎 →
  #   翻完兩個表面都更新嗎 → 有人記得「這一頁是誰的」嗎。
  #   少任何一段,畫面上都看不出來(滾輪就是沒反應),而那不會有人回報。
  check
  local w38bad=0
  local w38out; w38out="$("${PY}" - "${CODE_DIR}" <<'PYSCRIPT'
import os, re, sys
root = sys.argv[1]

def read(rel):
    try:
        return open(os.path.join(root, rel), encoding='utf-8',
                    errors='replace').read()
    except OSError:
        return None

cw = read('service/cand_window.cc')
ps = read('service/pipe_server.cc')
cl = read('common/cand_layout.cc')
if cw is None or ps is None or cl is None:
    print('NOSRC=1')
    raise SystemExit(0)

def match_from(src, i):
    depth = 0
    for j in range(i, len(src)):
        if src[j] == '{':
            depth += 1
        elif src[j] == '}':
            depth -= 1
            if depth == 0:
                return (i, j + 1)
    return (i, len(src))

def body_of(src, sig):
    i = src.find(sig)
    if i < 0:
        return None
    b = src.find('{', i)
    if b < 0:
        return None
    ob, cb = match_from(src, b)
    return src[ob + 1:cb - 1]

# 分母:候選窗自己處理的視窗訊息。掃空 = 範圍寫錯,那必須紅。
print('NCASE=%d' % len(re.findall(r'case\s+WM_[A-Z]+', cw)))

if 'case WM_MOUSEWHEEL' not in cw:
    print('NOWHEELCASE=1')
elif 'OnWheel(' not in cw:
    print('WHEELCASENOCALL=1')

ow = body_of(cw, 'void CandidateWindow::OnWheel(')
if ow is None:
    print('NOWHEELFN=1')
else:
    if 'WheelPageSteps(' not in ow:
        print('WHEELNOPUREFN=1')
    # ⚠ 「有讀到 page_fn_」不等於「有叫它」。這一格是覆核時實測出來的:
    #    植入 W38c(把 `if (fn) fn(steps);` 換成 `(void)steps;`)之後
    #    整支守門仍然是綠的 —— 那條反向測試本來等於沒做。
    if 'page_fn_' not in ow or 'fn(steps)' not in ow:
        print('WHEELNOHANDOFF=1')
    if 'shown_.items.empty()' not in ow:
        print('WHEELNOEMPTYGUARD=1')

if 'WheelPageSteps' not in cl:
    print('NOPUREIMPL=1')

if not re.search(r'SetPageHandler\(\s*\[this\]', ps):
    print('NOINSTALL=1')
if 'SetPageHandler(nullptr)' not in ps:
    print('NOUNINSTALL=1')

oc = body_of(ps, 'void PipeServer::OnCandidateWheel(')
if oc is None:
    print('NOHANDLERFN=1')
else:
    if 'ChangePage(' not in oc:
        print('HANDLERNOCHANGEPAGE=1')
    if 'ui_->Update(' not in oc:
        print('HANDLERNOREPAINT=1')
    if 'bar_->OnSnapshot(' not in oc:
        print('HANDLERNOBAR=1')
    if 'ui_session_' not in oc:
        print('HANDLERNOSESSION=1')

if not re.search(r'ui_session_\s*=\s*snap\.items\.empty\(\)', ps):
    print('NOSESSIONRECORD=1')
PYSCRIPT
)"
  local ncase; ncase="$(num "$(printf '%s\n' "${w38out}" | grep '^NCASE=' | cut -d= -f2)")"
  need_scope "W38 候選窗的視窗訊息分支" "${ncase}" 4 || w38bad=1
  local w38line
  while IFS= read -r w38line; do
    case "${w38line}" in
      NOSRC=*)
        red "W38:找不到 cand_window.cc / pipe_server.cc / cand_layout.cc —— 掃描範圍錯了"; w38bad=1 ;;
      NOWHEELCASE=*)
        red "W38:候選窗沒有 WM_MOUSEWHEEL 分支 —— 滾輪翻頁整條鏈的第一段就斷了(而畫面上看起來只是「滾輪沒反應」)"; w38bad=1 ;;
      WHEELCASENOCALL=*)
        red "W38:有 WM_MOUSEWHEEL 分支但沒有呼叫 OnWheel() —— 收到了不做事"; w38bad=1 ;;
      NOWHEELFN=*)
        red "W38:找不到 CandidateWindow::OnWheel() 的本體"; w38bad=1 ;;
      WHEELNOPUREFN=*)
        red "W38:OnWheel 沒有走 WheelPageSteps() —— 直接看 delta 的正負在滑鼠上正常,而精密觸控板一次輕撥會送出一連串小增量,使用者一撥就翻掉十幾頁。判斷要留在純函式裡才驗得到"; w38bad=1 ;;
      WHEELNOHANDOFF=*)
        red "W38:OnWheel 算完之後沒有交給 page_fn_ —— 又一個「算對了但沒有人收」"; w38bad=1 ;;
      WHEELNOEMPTYGUARD=*)
        red "W38:OnWheel 沒有擋「現在沒有候選」—— 隱藏的視窗仍然收得到 hover 捲動,那時翻頁是在使用者背後動他的組字狀態"; w38bad=1 ;;
      NOPUREIMPL=*)
        red "W38:common/cand_layout.cc 裡沒有 WheelPageSteps —— 純函式那一半不見了,Ubuntu 上就驗不到"; w38bad=1 ;;
      NOINSTALL=*)
        red "W38:pipe_server 沒有掛上 SetPageHandler —— 候選窗手上那個回呼永遠是空的,這正是 kChangePage 原本的處境(線路接好了沒有人叫它)"; w38bad=1 ;;
      NOUNINSTALL=*)
        red "W38:pipe_server 的解構子沒有 SetPageHandler(nullptr) —— 候選窗比它晚死,那個 lambda 捕捉的是一個已經不在的 this"; w38bad=1 ;;
      NOHANDLERFN=*)
        red "W38:找不到 PipeServer::OnCandidateWheel() 的本體"; w38bad=1 ;;
      HANDLERNOCHANGEPAGE=*)
        red "W38:OnCandidateWheel 沒有呼叫 Engine::ChangePage —— 收到滾輪卻沒有翻頁"; w38bad=1 ;;
      HANDLERNOREPAINT=*)
        red "W38:OnCandidateWheel 翻完頁沒有把新快照推回候選窗 —— 頁翻了、畫面沒動,而使用者按的數字鍵會選到看不見的字"; w38bad=1 ;;
      HANDLERNOBAR=*)
        red "W38:OnCandidateWheel 只更新候選窗、沒有餵給那一橫 —— ui-design §12.10.1 規範性:兩個表面必須讀同一份快照"; w38bad=1 ;;
      HANDLERNOSESSION=*)
        red "W38:OnCandidateWheel 沒有讀 ui_session_ —— 它不知道要翻誰的頁"; w38bad=1 ;;
      NOSESSIONRECORD=*)
        red "W38:push_ui 沒有把「沒有候選 → ui_session_ 清成 0」寫下來 —— 候選窗收起來之後,滾輪仍然會去翻一個看不見的 session"; w38bad=1 ;;
    esac
  done <<< "${w38out}"
  [ "${w38bad}" -eq 0 ] && ok "W38 滾輪翻頁整條鏈都接著:WM_MOUSEWHEEL → OnWheel(擋空清單、走純函式 WheelPageSteps)→ page_fn_ → PipeServer::OnCandidateWheel → Engine::ChangePage → 候選窗與那一橫**同一份**快照,而且 push_ui 記著「這一頁是誰的」、解構子把回呼收回來"

  # ── W39:頁碼從快照到畫面,整條鏈都要接著 ─────────────────────
  #
  # ⚠ 同 W38,守的是鏈。G72 之前:`page_no` / `is_last_page` 一路從
  #   `rs_menu` 進到 `Snapshot`(protocol.h:193–195),而整個 `windows/`
  #   底下**沒有任何地方讀它們**。使用者翻得動頁,卻不知道自己在第幾頁、
  #   後面還有沒有 —— 於是「下一頁大概就沒了」這個猜測會讓他在第一頁
  #   就停下來。
  #
  # ⚠ 字面本身是**規範性**的(theme-format §8.12),所以這裡也守
  #   「判斷留在純函式裡」:自己在繪製碼裡拼一個 "1/3" 出來,在 Ubuntu 上
  #   一行都驗不到,而它同時是違反規範的(librime 不提供總頁數)。
  check
  local w39bad=0
  local w39out; w39out="$("${PY}" - "${CODE_DIR}" <<'PYSCRIPT'
import os, re, sys
root = sys.argv[1]

def read(rel):
    try:
        return open(os.path.join(root, rel), encoding='utf-8',
                    errors='replace').read()
    except OSError:
        return None

cw = read('service/cand_window.cc')
cl = read('common/cand_layout.cc')
ch = read('common/cand_layout.h')
if cw is None or cl is None or ch is None:
    print('NOSRC=1')
    raise SystemExit(0)

def match_from(src, i):
    depth = 0
    for j in range(i, len(src)):
        if src[j] == '{':
            depth += 1
        elif src[j] == '}':
            depth -= 1
            if depth == 0:
                return (i, j + 1)
    return (i, len(src))

def body_of(src, sig):
    i = src.find(sig)
    if i < 0:
        return None
    b = src.find('{', i)
    if b < 0:
        return None
    ob, cb = match_from(src, b)
    return src[ob + 1:cb - 1]

# 分母:cand_layout.h 上的欄位。掃空 = 範圍寫錯,必須紅。
print('NFIELD=%d' % len(re.findall(r'^\s+double\s+\w+', ch, re.M)))

# ⚠ 用詞界,不是子字串:把它改名成 PageIndicatorTextRemoved 的植入
#   在子字串比對底下**完全沒有變紅**(實跑確認過)。
if not re.search(r'\bPageIndicatorText\s*\(', cl):
    print('NOPUREIMPL=1')

rl = body_of(cw, 'void CandidateWindow::Relayout()')
if rl is None:
    print('NORELAYOUT=1')
else:
    if not re.search(r'page\.page_no\s*=\s*shown_\.page_no', rl):
        print('NOPAGEFROMSNAP=1')
    if not re.search(r'page\.is_last_page\s*=\s*shown_\.is_last_page', rl):
        print('NOLASTFROMSNAP=1')
    # ⚠ 不能用 `[^;]*`:那一次呼叫的第三個引數是一個 lambda,而 lambda
    #   本體裡有分號。分兩問:有沒有呼叫、最後一個引數是不是 page。
    if not re.search(r'ComputeLayout\(', rl) or \
       not re.search(r',\s*page\s*\)\s*;', rl):
        print('NOPAGEARG=1')

pt = body_of(cw, 'void CandidateWindow::Paint(')
if pt is None:
    print('NOPAINT=1')
else:
    # ⚠ 「本體裡出現 layout_.page_text」擋不住把守衛改成 if (false) ——
    #   那個植入實跑之後守門仍然全綠。要問的是**那個判斷本身還在不在**。
    if 'if (!layout_.page_text.empty())' not in pt:
        print('NOPAGEDRAW=1')
    elif 'layout_.page_x' not in pt or 'layout_.page_y' not in pt:
        print('NOPAGEPOS=1')
    elif 'TextOutW' not in pt:
        print('NOPAGETEXTOUT=1')

# 繪製碼不得自己拼頁碼字面(§8.12 是規範性的,而且沒有總頁數)。
if re.search(r'L"\s*%d\s*/\s*%d', cw) or re.search(r'"/%d"', cw):
    print('HANDROLLED=1')
PYSCRIPT
)"
  local nfield; nfield="$(num "$(printf '%s\n' "${w39out}" | grep '^NFIELD=' | cut -d= -f2)")"
  need_scope "W39 cand_layout.h 的欄位" "${nfield}" 20 || w39bad=1
  local w39line
  while IFS= read -r w39line; do
    case "${w39line}" in
      NOSRC=*)
        red "W39:找不到 cand_window.cc / cand_layout.{h,cc} —— 掃描範圍錯了"; w39bad=1 ;;
      NOPUREIMPL=*)
        red "W39:common/cand_layout.cc 裡沒有 PageIndicatorText —— §8.12 的字面規則沒有純函式版本,Ubuntu 上驗不到"; w39bad=1 ;;
      NORELAYOUT=*)
        red "W39:找不到 CandidateWindow::Relayout() 的本體"; w39bad=1 ;;
      NOPAGEFROMSNAP=*)
        red "W39:Relayout 沒有從**這一份快照**取 page_no —— 頁碼要嘛不動、要嘛是別人的"; w39bad=1 ;;
      NOLASTFROMSNAP=*)
        red "W39:Relayout 沒有從快照取 is_last_page —— 「後面還有沒有」是這一格唯一的內容"; w39bad=1 ;;
      NOPAGEARG=*)
        red "W39:ComputeLayout 沒有收到 page —— 版面不知道要留位置,頁碼會壓在候選上面(或根本不畫)"; w39bad=1 ;;
      NOPAINT=*)
        red "W39:找不到 CandidateWindow::Paint() 的本體"; w39bad=1 ;;
      NOPAGEDRAW=*)
        red "W39:Paint 沒有畫 layout_.page_text —— 又一個「算出來了但沒有人畫」,而那正是 G72 本身"; w39bad=1 ;;
      NOPAGEPOS=*)
        red "W39:Paint 沒有用 layout_.page_x / page_y —— 版面算了位置卻不照它畫,頁碼會壓在候選上面"; w39bad=1 ;;
      NOPAGETEXTOUT=*)
        red "W39:Paint 讀了 page_text 卻沒有任何輸出呼叫"; w39bad=1 ;;
      HANDROLLED=*)
        red "W39:繪製碼自己拼了一個 n/m 形式的頁碼 —— §8.12 明文規定後綴是 `+` 而不是分數,因為 librime **不提供總頁數**,寫成 1/3 就得靠猜"; w39bad=1 ;;
    esac
  done <<< "${w39out}"
  [ "${w39bad}" -eq 0 ] && ok "W39 頁碼整條鏈都接著:快照的 page_no / is_last_page → PageHint → ComputeLayout(留位置)→ PageIndicatorText(§8.12 的字面,純函式)→ Paint 真的畫出來,而且繪製碼沒有自己拼分數"
}

# ────────────────────────────────────────────────────────────────
# 反向測試:每一條都真的植入一次違規,要求它變紅。
# ────────────────────────────────────────────────────────────────
self_check() {
  info "反向測試:逐條植入違規,要求它變紅"
  local base; base="$(mktemp -d)"
  cp -r "${ROOT}/windows" "${base}/windows"

  # ⚠ **恆假防護**(#84)。這一段以前不在,而少了它整張表什麼都不證明:
  #   判準是「植入之後 rc != 0」,而 W29 的五條常駐紅讓 rc **本來就是 1**。
  #   於是 59 條「會紅」全部是因為 W29 一直紅,不是因為植入的違規被抓到
  #   —— 覆核者實測植入 J(把 #80 的修正整個還原掉)之後,守門的輸出
  #   一個位元都沒變,而報告上仍然是一整排 ok。
  #   照 verify_update_gates.sh:52–62 的寫法:沒植入時不綠就直接退出。
  local base_out base_rc
  base_out="$(RIMEWIN_ROOT="${base}" bash "${SCRIPT_DIR}/check_ui_spec.sh" 2>&1)"
  base_rc=$?
  if [ "${base_rc}" -ne 0 ]; then
    printf '\033[1;31m[FAIL]\033[0m 沒有植入任何東西,check_ui_spec.sh 卻已經是紅的 —— **底下的反向測試全部不算數**\n' >&2
    printf '%s\n' "${base_out}" | grep -a 'FAIL' | head -12 >&2
    return 1
  fi
  printf '  \033[1;32mok\033[0m   基準:沒有植入時是綠的\n'

  local pass=0 fail=0
  # 名稱|要改的檔案|python 片段(對 s 做替換)
  local muts=(
"W1|service/settings_window.cc|s=s.replace('void SettingsWindow::LayoutUi() {','void SettingsWindow::LayoutUi() { double dpi_scale_ = 1.0; (void)dpi_scale_;',1)"
"W2|service/status_bar.cc|s=s.replace('case WM_DPICHANGED: {','case WM_NULL + 12345: {',1)"
"W3|common/ui_layout.h|s=s.replace('constexpr int s7 = 20;','constexpr int s7 = 22;',1)"
"W4|common/ui_layout.h|s=s.replace('constexpr int t4 = 12;','constexpr int t4 = 14;',1)"
"W5|service/cand_window.cc|s=s.replace('namespace {','namespace {\\nstatic int probe = SPI_GETNONCLIENTMETRICS;',1)"
"W6|service/settings_window.cc|s=s.replace('void SettingsWindow::ApplyFonts() {','void SettingsWindow::ApplyFonts() { LOGFONTW lf{}; HFONT bad = ::CreateFontIndirectW(&lf); (void)bad;',1)"
"W7|service/settings_window.cc|s=s.replace('constexpr UiString kNoText = UiString::kUiStringCount;', 'constexpr UiString kNoText = UiString::kUiStringCount;' + chr(10) + 'constexpr wchar_t kOops[] = L' + chr(34) + chr(28204) + chr(34) + ';', 1)"
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
"W24a 版面回到 service|service/settings_window.cc|s=s.replace('void SettingsWindow::LayoutUi() {','void SettingsWindow::LayoutUi() { Stack sneaky(0, 0, 100); (void)sneaky.Push(10, 2);',1)"
"W24b 版面上少三顆|common/ui_layout.cc|s=s.replace('      radios({IDC_THEME_0, IDC_THEME_1, IDC_THEME_2}, \"appearance_radio\");','',1)"
"W24c 某頁多塞三顆|common/ui_layout.cc|s=s.replace('radios({IDC_THEME_0, IDC_THEME_1, IDC_THEME_2}, \"appearance_radio\");','radios({IDC_THEME_0, IDC_THEME_1, IDC_THEME_2, IDC_LANG_1, IDC_LANG_2, IDC_LANG_3}, \"appearance_radio\");',1)"
"W24e 頁名回到 service|service/settings_window.cc|s=s.replace('  wc.lpszClassName = kClass;','  const UiString sneaky[] = {UiString::kNavSchemas}; (void)sneaky;' + chr(10) + '  wc.lpszClassName = kClass;',1)"
"W24d 表上少三顆|service/settings_window.cc|s=s.replace('    {IDC_THEME_0, L\"BUTTON\", RADIO1, UiString::kThemeFollowSystem},','',1).replace('    {IDC_THEME_1, L\"BUTTON\", RADIO, UiString::kThemeLight},','',1).replace('    {IDC_THEME_2, L\"BUTTON\", RADIO, UiString::kThemeDark},','',1)"
"W25 拿掉滾輪|service/settings_window.cc|s=s.replace('case WM_MOUSEWHEEL:','case WM_NULL + 4242:',1)"
"W25b 又把高度丟掉|common/ui_layout.cc|s=s.replace('int ScrollMaxDip(int page, int window_w_dip, int window_h_dip,','int ScrollMaxDipRemoved(int page, int window_w_dip, int window_h_dip,',1)"
"W26 狀態列不重擺|service/status_bar.cc|s=s.replace('  ApplyPlacement(MulDivRound(total_w, 96, static_cast<int>(dpi_)));','',1)"
"W26b 简繁點完不回讀|service/status_bar.cc|s=s.replace('      RefreshFromEngine();\n      return;\n    }\n    case kCellSchema:','      return;\n    }\n    case kCellSchema:',1)"
"W26c 回讀不問引擎|service/status_bar.cc|s=s.replace('  const Engine::StatusReadback rb = engine_->ReadBackStatus();','  Engine::StatusReadback rb;',1)"
"W26d 中英又樂觀寫入|service/status_bar.cc|s=s.replace('      engine_->SetAsciiModeAll(!engine_->AsciiMode());','      { std::lock_guard<std::mutex> lk(mu_); ascii_mode_ = !ascii_mode_; }\n      engine_->SetAsciiModeAll(!engine_->AsciiMode());',1)"
"W26f 空的那一格又佔位置|service/status_bar.cc|s=s.replace('    if (c.text.empty()) {','    if (false) {',1)"
"W26g 零寬的那一格又點得到|service/status_bar.cc|s=s.replace('    if (r.right <= r.left) continue;','',1)"
"W26e 回讀之後不重畫|service/status_bar.cc|s=s.replace('  if (changed) {\n    Relayout();\n    ::InvalidateRect(hwnd_, nullptr, TRUE);\n','  if (changed) {\n',1)"
"W27a Relayout 不再問狀態|service/status_bar.cc|s=s.replace('  service_state_ = CurrentServiceState();','  service_state_ = ServiceState::kReady;',1)"
"W27b 那一橫的字寫死一句|service/status_bar.cc|s=s.replace('    c.text = UiText(StatusTextFor(service_state_));','    c.text = UiText(UiString::kBarNotRunning);',1)"
"W27c 不讀線路上的旗標|service/status_bar.cc|s=s.replace('SnapshotSaysNotReady(snap.status_flags)','false',1)"
"W27d 事實少餵一格|service/status_bar.cc|s=s.replace('  facts.engine_says_not_ready =' + chr(10) + '      engine_not_ready_.load() ||','  facts.engine_says_not_ready = false ||',1)"
"W28 自繪直接用 nmcd.rc|service/settings_window.cc|s=s.replace('LRESULT SettingsWindow::DrawSchemaList(NMLVCUSTOMDRAW* cd) {','LRESULT SettingsWindow::DrawSchemaList(NMLVCUSTOMDRAW* cd) { RECT sneaky = cd->nmcd.rc; (void)sneaky;',1)"
"W32 那一橫又擋 UI 執行緒 1.5 秒|service/status_bar.cc|s=s.replace('  popup_loading_ = !engine_->SchemaListFromCache(&popup_items_);','  popup_loading_ = !engine_->SchemaListForUi(1500, &popup_items_);',1)"
"W32b 每按一次就多排一件|service/status_bar.cc|s=s.replace('if (popup_loading_ && !schema_query_inflight_) {','if (popup_loading_) {',1)"
"W31j 側欄反白改回從 CDIS_SELECTED 畫(覆核者實測的拆法 J)|service/settings_window.cc|s=s.replace('      const bool selected = (i == page_);','      const bool selected = (cd->nmcd.uItemState & CDIS_SELECTED) != 0;',1)"
"W31k SelectOnlyRow 拿掉先全清(覆核者實測的拆法 K)|service/ui_listview.cc|s=s.replace('  ::SendMessageW(list, LVM_SETITEMSTATE, static_cast<WPARAM>(-1),' + chr(10) + '                 reinterpret_cast<LPARAM>(&clear));','',1)"
"W31s 方案清單又自己下 LVM_SETITEMSTATE|service/settings_window.cc|s=s.replace('void SettingsWindow::SelectSchemaRow(int row) {','void SettingsWindow::SelectSchemaRow(int row) { LVITEMW sneaky{}; ::SendMessageW(schema_list_, LVM_SETITEMSTATE, 0, reinterpret_cast<LPARAM>(&sneaky));',1)"
"W31d 方案清單的反白改回從 CDIS_SELECTED 畫|service/settings_window.cc|s=s.replace('      const bool selected = (i == schema_sel_);','      const bool selected = (cd->nmcd.uItemState & CDIS_SELECTED) != 0;',1)"
"W31w 連網紀錄的反白改回讀 uItemState(run #171 實測:它 5 列裡說 5 列被選)|service/settings_window.cc|s=s.replace('      const bool selected = RowIsSelected(net_log_list_, static_cast<int>(i));','      const bool selected = (cd->nmcd.uItemState & CDIS_SELECTED) != 0;',1)"
"W30 頁碼又寫死成數字|service/status_bar.cc|s=s.replace('settings_->OpenAt(StateIsFailure(service_state_) ? kPageAdvanced','settings_->OpenAt(StateIsFailure(service_state_) ? 3',1).replace(': kPageSchemas);',': 0);',1)"
"W30b 只有一邊改回數字(跨行,而且引數裡自己有括號)|service/status_bar.cc|s=s.replace(': kPageSchemas);',': 0);',1)"
"W9 少一條單元測試|tests/test_status_cells.cc|s=s.replace('TEST(status_cells_input_mode_shows_exactly_one_label)','TEST(status_cells_renamed_away)',1)"
"W38a 沒有人收滾輪|service/cand_window.cc|s=s.replace('case WM_MOUSEWHEEL:','case WM_NULL + 4243:',1)"
"W38b 滾輪不走純函式(觸控板一撥翻十幾頁)|service/cand_window.cc|s=s.replace('const int32_t steps = WheelPageSteps(&wheel_accum_, delta);','const int32_t steps = delta > 0 ? -1 : 1;',1)"
"W38c 算完了不交出去|service/cand_window.cc|s=s.replace('  if (fn) fn(steps);','  (void)steps;',1)"
"W38d 回呼沒有人裝(kChangePage 原本的處境)|service/pipe_server.cc|s=s.replace('  if (ui_) ui_->SetPageHandler([this](int32_t steps) {' + chr(10) + '    OnCandidateWheel(steps);' + chr(10) + '  });','',1)"
"W38e 翻完頁那一橫沒跟上|service/pipe_server.cc|s=s.replace('  if (bar_) bar_->OnSnapshot(r.snap);','',1)"
"W38f 沒有人記下這一頁是誰的|service/pipe_server.cc|s=s.replace('ui_session_ = snap.items.empty() ? 0 : session;','ui_session_ = session;',1)"
"W38g 解構子不把回呼收回來|service/pipe_server.cc|s=s.replace('  if (ui_) ui_->SetPageHandler(nullptr);','',1)"
"W39a 頁碼算出來了沒有人畫(G72 本身)|service/cand_window.cc|s=s.replace('  if (!layout_.page_text.empty()) {','  if (false) {',1)"
"W39b 版面不知道有頁碼|service/cand_window.cc|s=s.replace('  }, page);','  }, PageHint());',1)"
"W39c 頁碼不從這一份快照來|service/cand_window.cc|s=s.replace('  page.page_no = shown_.page_no;','  page.page_no = 0;',1)"
"W39d 「後面還有沒有」不從快照來|service/cand_window.cc|s=s.replace('  page.is_last_page = shown_.is_last_page;','  page.is_last_page = true;',1)"
"W39e 純函式那一半不見了|common/cand_layout.cc|s=s.replace('std::string PageIndicatorText(','std::string PageIndicatorTextRemoved(',1).replace('  out.page_text = PageIndicatorText(page.page_no, page.is_last_page);','  out.page_text = PageIndicatorTextRemoved(page.page_no, page.is_last_page);',1)"
"W27e 拿掉那一橫自己更新的計時器|service/status_bar.cc|s=s.replace('  ::SetTimer(hwnd_, kStateTimer, kStatePollMs, nullptr);','',1)"
"W27f 計時器還在但不再比對狀態|service/status_bar.cc|s=s.replace('      if (now != self->service_state_) {','      if (false) {',1)"
"W27g 側欄又變回兩句|service/settings_window.cc|s=s.replace('  ::DrawTextW(hdc, UiText(SidebarStatusTextFor(state)),','  ::DrawTextW(hdc, UiText(UiString::kNavStatusNotRunning),',1)"
"W27h 側欄不再自己更新|service/settings_window.cc|s=s.replace('      if (self && w == kServiceStateTimer) self->OnServiceStateTick();','',1)"
"W27i 那一橫三種合併回同一句|common/service_state.cc|s=s.replace('      return UiString::kBarPreparing;','      return UiString::kBarNotRunning;',1)"
"W27j 側欄三種合併回同一句|common/service_state.cc|s=s.replace('      return UiString::kNavStatusPreparing;','      return UiString::kNavStatusNotRunning;',1)"
"W12a 設定視窗不跟著系統換深淺(整檔 grep 抓不到)|service/settings_window.cc|s=s.replace('Theme::IsColorSetChange(l) ||','false ||',1)"
"W12b 比對的字面值被換掉|service/ui_theme.cc|s=s.replace('L'+chr(34)+'ImmersiveColorSet'+chr(34),'L'+chr(34)+'SomethingElse'+chr(34),1)"
"W25a 捲動量沒套到控制項(覆核者實測的拆法)|service/settings_window.cc|s=s.replace('place(id, RectI{p->rect.x, sp.y_dip, p->rect.w, p->rect.h});','place(id, RectI{p->rect.x, p->rect.y, p->rect.w, p->rect.h});',1)"
"W25b 捲動量沒送進純函式|service/settings_window.cc|s=s.replace('ScrollPlaceControlDip(p->rect, scroll_, viewport_h)','ScrollPlaceControlDip(p->rect, 0, viewport_h)',1)"
"W25c 捲出可視範圍就藏起來(覆核者實測的拆法)|service/settings_window.cc|s=s.replace('::ShowWindow(c, sp.visible ? SW_SHOW : SW_HIDE);','::ShowWindow(c, SW_HIDE);',1)"
"W25d 主視窗的捲軸拿掉(覆核者實測的拆法)|service/settings_window.cc|s=s.replace('WS_THICKFRAME | WS_VSCROLL | WS_CLIPCHILDREN','WS_THICKFRAME | WS_CLIPCHILDREN',1)"
"W25e 訊息迴圈裡的焦點呼叫刪掉、定義留著(覆核者實測的拆法)|service/settings_window.cc|s=s.replace('        EnsureFocusVisible();','        (void)0;',1)"
"W25f 滾輪分支在但什麼都不做|service/settings_window.cc|s=s.replace('      if (self) self->OnMouseWheel(GET_WHEEL_DELTA_WPARAM(w));','',1)"
"W25g 裁切高度不從純函式來|service/settings_window.cc|s=s.replace('ClipToViewport(i, c, p->rect.w, sp.clip_h_dip);','ClipToViewport(i, c, p->rect.w, -1);',1)"
"W25h 純函式從 common/ 消失|common/ui_layout.cc|s=s.replace('ScrolledPlacement ScrollPlaceControlDip(','ScrolledPlacement ScrollPlaceControlDipGone(',1)"
"W29a 開關的狀態讀錯地方|service/settings_window.cc|s=s.replace('const bool on = net_gate_.Enabled();','const bool on = settings_.NetworkEnabled();',1)"
"W29b 開關底下那句話寫死一條|service/settings_window.cc|s=s.replace('SetText(hwnd_, IDC_NET_STATE, UiText(NetSwitchSummary(on)));','SetText(hwnd_, IDC_NET_STATE, UiText(UiString::kNetworkOffSummary));',1)"
"W29c 檢查更新那道門整個拿掉(開關關著也連出去)|service/settings_window.cc|blk='  if (!net_gate_.Enabled()) {' + chr(10) + '    update_failure_ = UpdateFailure::kSwitchOff;' + chr(10) + '    update_stage_ = UpdateStage::kIdle;' + chr(10) + '    RefreshNetworkAndUpdateCard();' + chr(10) + '    return;' + chr(10) + '  }' + chr(10); s=s.replace(blk,'',1)"
"W29d 下載那道門沒人看(舊判準只看 StartUpdateCheck)|service/settings_window.cc|blk='  if (!net_gate_.Enabled()) {' + chr(10) + '    update_failure_ = UpdateFailure::kSwitchOff;' + chr(10) + '    RefreshNetworkAndUpdateCard();' + chr(10) + '    return;' + chr(10) + '  }' + chr(10); s=s.replace(blk,'',1)"
"W29e 問了開關但問在 CreateThread 之後(位置式判準)|service/settings_window.cc|blk='  if (!net_gate_.Enabled()) {' + chr(10) + '    update_failure_ = UpdateFailure::kSwitchOff;' + chr(10) + '    update_stage_ = UpdateStage::kIdle;' + chr(10) + '    RefreshNetworkAndUpdateCard();' + chr(10) + '    return;' + chr(10) + '  }' + chr(10); ct='  update_thread_ = ::CreateThread(nullptr, 0, &UpdateWorkerEntry, this, 0,' + chr(10) + '                                  nullptr);' + chr(10); s=s.replace(blk,'',1); s=s.replace(ct,ct+blk,1)"
"W29m 問了開關卻不收手(那個分支沒有 return)|service/settings_window.cc|s=s.replace('    update_stage_ = UpdateStage::kIdle;' + chr(10) + '    RefreshNetworkAndUpdateCard();' + chr(10) + '    return;' + chr(10) + '  }','    update_stage_ = UpdateStage::kIdle;' + chr(10) + '    RefreshNetworkAndUpdateCard();' + chr(10) + '  }',1)"
"W29n 擋下的理由說成別的(不是 kSwitchOff)|service/settings_window.cc|s=s.replace('    update_failure_ = UpdateFailure::kSwitchOff;' + chr(10) + '    update_stage_ = UpdateStage::kIdle;','    update_failure_ = UpdateFailure::kUnreachable;' + chr(10) + '    update_stage_ = UpdateStage::kIdle;',1)"
"W29o 檢查在 UI 執行緒上直接跑|service/settings_window.cc|s=s.replace('  update_failure_ = UpdateFailure::kNone;' + chr(10) + '  update_stage_ = UpdateStage::kChecking;','  UpdateFailure why0 = UpdateFailure::kNone; update_.Check(&why0);' + chr(10) + '  update_stage_ = UpdateStage::kChecking;',1)"
"W29p 工作執行緒直接動畫面,不回 UI 執行緒|service/settings_window.cc|s=s.replace('  ::PostMessageW(self->hwnd_, WM_RIME_UPDATE_DONE, 0, 0);','  self->OnUpdateWorkerDone();',1)"
"W29q 工作執行緒的下載那一條路是死的|service/settings_window.cc|s=s.replace('    self->update_.DownloadAndVerify(&why);','    (void)0;',1)"
"W29r 卡片吃的不是 NetGate 的真值|service/settings_window.cc|s=s.replace('  st.network_enabled = on;','  st.network_enabled = true;',1)"
"W29s 更新完不重讀連網紀錄|service/settings_window.cc|s=s.replace('  RefreshNetworkPage();' + chr(10) + '  const bool on = net_gate_.Enabled();','  const bool on = net_gate_.Enabled();',1)"
"W29t 工作回來了卻不重畫|service/settings_window.cc|s=s.replace('  RefreshNetworkAndUpdateCard();' + chr(10) + '}' + chr(10) + chr(10) + 'void SettingsWindow::DoUpdateHandOff() {','}' + chr(10) + chr(10) + 'void SettingsWindow::DoUpdateHandOff() {',1)"
"W29f 空狀態的版面分支被寫死|service/settings_window.cc|s=s.replace('  s.net_log_empty = net_log_empty_;','  s.net_log_empty = false;',1)"
"W29g 紀錄不是從出口讀來的|service/settings_window.cc|s=s.replace('BuildNetLogView(net_gate_.ReadLog(), ui_lang_, LocalTzOffsetMinutes())','BuildNetLogView({}, ui_lang_, LocalTzOffsetMinutes())',1)"
"W29h 更新卡片那一句話寫死一條|service/settings_window.cc|s=s.replace('  SetText(hwnd_, IDC_UPDATE_STATUS, text.c_str());','  SetText(hwnd_, IDC_UPDATE_STATUS, UiText(UiString::kUpdateStatusUpToDate));',1)"
"W29i 清除紀錄不問一聲|service/settings_window.cc|s=s.replace('  if (!ConfirmDialog(hwnd_, &theme_, script(),' + chr(10) + '                     UiText(UiString::kNetLogClearHeading),' + chr(10) + '                     UiText(UiString::kNetLogClearBlurb),' + chr(10) + '                     UiText(UiString::kNetLogClearButton),' + chr(10) + '                     UiText(UiString::kCancel)))' + chr(10) + '    return;','',1)"
"W29j 開關寫不進去卻不說|service/settings_window.cc|s=s.replace('    SetStatus(UiString::kStatusSaveFailed);' + chr(10) + '    RefreshNetworkPage();' + chr(10) + '    return;','    RefreshNetworkPage();' + chr(10) + '    return;',1)"
"W29k 更新卡片的純函式從 common/ 消失|common/update_flow.cc|s=s.replace('UpdateCard DescribeUpdateCard(','UpdateCard DescribeUpdateCardGone(',1)"
"W29k2 那一句失敗文案的純函式從 common/ 消失|common/update_flow.cc|s=s.replace('UiString UpdateFailureText(','UiString UpdateFailureTextGone(',1)"
"W29k3 「要不要提開關」的純函式從 common/ 消失|common/update_flow.cc|s=s.replace('bool UpdateFailureNeedsSwitch(','bool UpdateFailureNeedsSwitchGone(',1)"
"W29u 背景執行緒不再去查(winbar 帶進來的:win-next 那一側沒有任何植入碰得到 THREAD_DOES_NOT_CHECK)|service/settings_window.cc|s=s.replace('    self->update_.Check(&why);','    (void)why;',1)"
"W29l 版面呼叫點繞過真實狀態|service/settings_window.cc|s=s.replace('  const PageLayout pl = LayoutSettingsPageDip(page_, W, PageStateNow());','  const PageLayout pl = LayoutSettingsPageDip(page_, W, PageState{});',1)"
"W34a 套用的呼叫點改回無條件說「已套用」(覆核者實測的拆法 A1)|service/settings_window.cc|old='  const unsigned seq = BeginApply(UiString::kStatusApplied);' + chr(10) + '  engine_->ApplyVariantAll(settings_.SchemaPref(), ApplyDoneNotifier(seq));' + chr(10) + '  int vsel = 0;'; new='  engine_->ApplyVariantAll(settings_.SchemaPref(), [](bool) {});' + chr(10) + '  SetTransientStatus(UiString::kStatusApplied);' + chr(10) + '  int vsel = 0;'; s=s.replace(old,new,1)"
"W34b Engine::ApplyVariantAll 不再說「根本沒排進去」(覆核者實測的拆法 A2)|service/engine.cc|k='  if (!queued && on_done) on_done(false);' + chr(10); i=s.index('void Engine::ApplyVariantAll'); j=s.index(k,i); s=s[:j]+s[j+len(k):]"
"W34c 4 秒的計時器改回無條件清空(覆核者實測的拆法 A3)|service/settings_window.cc|old='        if (self->status_line_.StillShowing(self->transient_ticket_)) {' + chr(10) + '          self->transient_ticket_ = StatusLine::kNone;' + chr(10) + '          self->SetStatus(std::wstring());' + chr(10) + '        }'; new='        self->transient_ticket_ = StatusLine::kNone;' + chr(10) + '        self->SetStatus(std::wstring());'; s=s.replace(old,new,1)"
"W34d 心跳解除改回無條件清空(覆核者實測的拆法 A4)|service/settings_window.cc|s=s.replace('      } else if (status_line_.StillShowing(engine_busy_ticket_)) {','      } else if (true) {',1)"
"W34e 拿掉序號守衛(覆核者實測的拆法 A5)|service/settings_window.cc|s=s.replace('  if (seq != apply_seq_) return;' + chr(10),'',1)"
"W34f 送出之後呼叫點又自己說了一次成功|service/settings_window.cc|s=s.replace('  engine_->ApplyVariantAll(settings_.SchemaPref(), ApplyDoneNotifier(seq));' + chr(10) + '  int vsel = 0;','  engine_->ApplyVariantAll(settings_.SchemaPref(), ApplyDoneNotifier(seq));' + chr(10) + '  SetTransientStatus(UiString::kStatusApplied);' + chr(10) + '  int vsel = 0;',1)"
"W34h ApplyDoneNotifier 的本體被掏空,名字還在(覆核者實測的拆法 B1)|service/settings_window.cc|old='    if (h) ::PostMessageW(h, WM_RIME_APPLY_DONE, static_cast<WPARAM>(seq),' + chr(10) + '                          ok ? 1 : 0);'; new='    (void)h; (void)seq; (void)ok;'; s=s.replace(old,new,1)"
"W34i WndProc 不再把 WM_RIME_APPLY_DONE 交給 OnApplyDone(覆核者實測的拆法 B4)|service/settings_window.cc|s=s.replace('      if (self) self->OnApplyDone(static_cast<unsigned>(w), l != 0);' + chr(10),'',1)"
"W34j OnApplyDone 成功那一支什麼都不寫(覆核者實測的拆法 B5)|service/settings_window.cc|s=s.replace('  SetTransientStatus(apply_ok_status_);','  (void)apply_ok_status_;',1)"
"W34k 成功那一句改回寫死,BeginApply 挑的措辭丟掉|service/settings_window.cc|s=s.replace('  SetTransientStatus(apply_ok_status_);','  SetTransientStatus(UiString::kStatusApplied);',1)"
"W34l SetStatus 記了 ticket 卻不動控制項(回程的下一格)|service/settings_window.cc|s=s.replace('  SetText(hwnd_, IDC_STATUS, text.c_str());','  (void)text;',1)"
"W34g 傳給完成通知的不是這一次拿到的序號|service/settings_window.cc|s=s.replace('  engine_->ApplyVariantAll(settings_.SchemaPref(), ApplyDoneNotifier(seq));' + chr(10) + '  int vsel = 0;','  engine_->ApplyVariantAll(settings_.SchemaPref(), ApplyDoneNotifier(seq - 1));' + chr(10) + '  int vsel = 0;',1)"
"W31n5 全清那一發改成一個位元都不動(覆核者實測的拆法 N5)|service/ui_listview.cc|s=s.replace('  clear.stateMask = LVIS_SELECTED ' + chr(124) + ' LVIS_FOCUSED;','  clear.stateMask = 0;',1)"
"W31n1 SelectOnlyRow 只清不設(覆核者實測的拆法 N1)|service/ui_listview.cc|k='  ::SendMessageW(list, LVM_SETITEMSTATE, static_cast<WPARAM>(row),' + chr(10) + '                 reinterpret_cast<LPARAM>(&set));' + chr(10); s=s.replace(k,'',1)"
"W31n2 方案清單悄悄退出受管(覆核者實測的拆法 N2)|service/settings_window.cc|s=s.replace('    SelectOnlyRow(schema_list_, row);' + chr(10),'',1)"
"W31n6 全清那一發改成把每一列都選起來|service/ui_listview.cc|s=s.replace('  clear.state = 0;','  clear.state = LVIS_SELECTED;',1)"
"W35a 套用失敗那一句改回會自己消失|service/settings_window.cc|s=s.replace('    SetStatus(UiString::kStatusApplyFailed);','    SetTransientStatus(UiString::kStatusApplyFailed);',1)"
"W35b 存檔失敗那一句也改成會自己消失|service/settings_window.cc|s=s.replace('    SetStatus(UiString::kStatusSaveFailed);','    SetTransientStatus(UiString::kStatusSaveFailed);',1)"
"W35c 失敗訊息從 BeginApply 那一頭混進 transient|service/settings_window.cc|s=s.replace('  const unsigned seq = BeginApply(UiString::kStatusResetDone);','  const unsigned seq = BeginApply(UiString::kStatusRedeployFailed);',1)"
"W27k 那一橫的 EngineFacts 不再讀重新部署的階段|service/status_bar.cc|s=s.replace('      engine_not_ready_.load() ||' + chr(10) + '      (engine_ && PhaseSaysPreparing(engine_->redeploy_phase()));','      engine_not_ready_.load();',1)"
"W27l 側欄的 EngineFacts 不再讀重新部署的階段|service/settings_window.cc|s=s.replace('      deploying_ ||' + chr(10) + '      (engine_ && PhaseSaysPreparing(engine_->redeploy_phase()));','      deploying_;',1)"
"W36a 部署前不再收 session|service/engine.cc|s=s.replace('  CloseAllSessionsOnEngineThread();' + chr(10),'',1)"
"W36j BeginDeploy 自己在 UI 執行緒上部署|service/engine.cc|s=s.replace('  if (PostAsync(' + chr(34) + '收乾淨 session 再開始部署' + chr(34) + ',','  if (rs_deploy() && PostAsync(' + chr(34) + '收乾淨 session 再開始部署' + chr(34) + ',',1)"
"W36k 拒絕啟動時不讓畫面知道|service/engine.cc|s=s.replace('  redeploy_start_failed_.store(true);' + chr(10) + '  deploy_seq_.fetch_add(1);' + chr(10),'',1)"
"W36l 收乾淨與開始部署之間沒有進到 kDeploying|service/engine.cc|s=s.replace('RedeployEvent::kSessionsClosed','RedeployEvent::kRebuilt',1)"
"W36b 當場建 session 那一處的門被拿掉|service/engine.cc|s=s.replace('    if (!SessionCreationAllowed(phase_.load())) return;' + chr(10) + '    const rs_session s = rs_session_create();','    const rs_session s = rs_session_create();',1)"
"W36c 備用 session 那一處的門被拿掉|service/engine.cc|s=s.replace('  if (!SessionCreationAllowed(phase_.load())) return;' + chr(10) + '  const rs_session s = rs_session_create();','  const rs_session s = rs_session_create();',1)"
"W36d 部署終局不再把 session 建回來|service/engine.cc|s=s.replace('  RebuildSessionsAsync();' + chr(10) + '}','}',1)"
"W36e 按鍵那道門排到 Find() 後面|service/engine.cc|s=s.replace('  if (ShouldFailOpen(phase_.load(), deploy_state_.load() == 1,' + chr(10) + '                     &r.snap.status_flags)) {','  (void)Find(id);' + chr(10) + '  if (ShouldFailOpen(phase_.load(), deploy_state_.load() == 1,' + chr(10) + '                     &r.snap.status_flags)) {',1)"
"W36f 重建不再重套方案與選項(#85)|service/engine.cc|s=s.replace('      SelectAndApply(ps.id, s, plan.schema_id);' + chr(10) + '      for (const OptionAssign& a : plan.options)' + chr(10) + '        rs_set_option(s, a.option, a.value);','      (void)plan;',1)"
"W36n 重建繞過 SelectAndApply,自己裸呼叫一次(winbar 規則 2 的形狀)|service/engine.cc|s=s.replace('      SelectAndApply(ps.id, s, plan.schema_id);','      if (!plan.schema_id.empty()) rs_select_schema(s, plan.schema_id.c_str());',1)"
"W36g 那道門讀不到階段|service/engine.cc|s=s.replace('ShouldFailOpen(phase_.load(), deploy_state_.load() == 1,','ShouldFailOpen(RedeployPhase::kIdle, deploy_state_.load() == 1,',1)"
"W36h BeginDeploy 沒有先把門關上|service/engine.cc|s=s.replace('RedeployEvent::kRequested','RedeployEvent::kRebuilt',1)"
"W36i 收乾淨那一支不再銷毀 session|service/engine.cc|s=s.replace('    rs_session_destroy(kv.second);' + chr(10) + '  }' + chr(10) + '  const int total','  }' + chr(10) + '  const int total',1)"
"W36m 按鍵那道門又排到佇列後面|service/engine.cc|s=s.replace('Result Engine::ProcessKey(uint64_t id, int32_t keysym, uint32_t mods) {' + chr(10) + '  Result r;','Result Engine::ProcessKey(uint64_t id, int32_t keysym, uint32_t mods) {' + chr(10) + '  Result r;' + chr(10) + '  Post(' + chr(34) + '先擋一下' + chr(34) + ', [] {});',1)"
"W37a 部署回呼改回裸指標|service/engine.cc|s=s.replace('  g_deploy_gate.Run([ok](Engine* e) { e->OnDeployTerminal(ok); });','  if (g_deploy_engine) g_deploy_engine->OnDeployTerminal(ok);',1)"
"W37b 閘換成 std::atomic<Engine*>(只換型別)|service/engine.cc|s=s.replace('CallbackGate<Engine> g_deploy_gate;','std::atomic<Engine*> g_deploy_gate{nullptr};',1)"
"W37c Stop 的關閘排到佇列後面|service/engine.cc|s=s.replace('  g_deploy_gate.Close();' + chr(10) + '  if (!started_) return;','  if (!started_) return;',1).replace('  started_ = false;','  started_ = false;' + chr(10) + '  g_deploy_gate.Close();',1)"
"W37d Start 不開閘|service/engine.cc|s=s.replace('  g_deploy_gate.Open(this);' + chr(10),'',1)"
"W37e 部署回呼裡直接呼叫 rs_*|service/engine.cc|s=s.replace('  const bool ok = (status == RS_DEPLOY_SUCCESS);','  const bool ok = (status == RS_DEPLOY_SUCCESS);' + chr(10) + '  (void)rs_last_error();',1)"
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
      # ⚠ 植入本身失敗(錨點對不上、跳脫寫錯)時,樹是**沒改過的**,
      #   於是守門當然是綠的 —— 而舊版把那報成「那一條守門不算數」,
      #   讀起來像是守門有問題。兩種情況要分開講,而且都要紅。
      if ! "${PY}" - "${tmp}/windows/${relfile}" <<PYMUT
import io,sys
p=sys.argv[1]
s=open(p,encoding='utf-8').read()
before=s
${pysnip}
if s==before:
    sys.stderr.write('NOCHANGE\n')
    raise SystemExit(3)
open(p,'w',encoding='utf-8').write(s)
PYMUT
      then
        printf '  \033[1;31m%s 的違規**根本沒有植入成功**(錨點對不上或跳脫寫錯) —— 這一條反向測試等於沒做\033[0m\n' "${name}" >&2
        fail=$((fail+1))
        rm -rf "${tmp}"
        continue
      fi
    fi

    local out
    out="$(RIMEWIN_ROOT="${tmp}" bash "${SCRIPT_DIR}/check_ui_spec.sh" 2>&1)"
    local rc=$?
    if [ "${rc}" -eq 0 ]; then
      printf '  \033[1;31m植入 %s 的違規之後腳本仍然是綠的 —— 那一條不算數\033[0m\n' "${name}" >&2
      fail=$((fail+1))
      rm -rf "${tmp}"
      continue
    fi
    # ⚠ **紅在該紅的地方**(#84)。只斷言結束碼是不夠的:植入 W29 的違規
    #   而紅了 W24,一樣算通過 —— 而那什麼都沒有證明。條號從名稱前綴取
    #   (「W31j 側欄反白…」→ W31),要求那一條自己吐出 [FAIL]。
    #   ⚠ 只認 `[FAIL] W31…` 這種行首:ok 那一行也含有「W31 」。
    local wnum; wnum="$(printf '%s' "${name}" | sed -n 's/^\(W[0-9]\{1,3\}\).*/\1/p')"
    if [ -n "${wnum}" ]; then
      local plain; plain="$(printf '%s\n' "${out}" | sed 's/\x1b\[[0-9;]*m//g')"
      if ! printf '%s\n' "${plain}" | grep -q "^\[FAIL\] ${wnum}[:/ ]"; then
        printf '  \033[1;31m植入 %s 的違規之後紅了,但**不是紅在 %s** —— 換了個地方壞掉不算守住\033[0m\n' \
               "${name}" "${wnum}" >&2
        printf '%s\n' "${plain}" | grep '^\[FAIL\]' | head -4 >&2
        fail=$((fail+1))
        rm -rf "${tmp}"
        continue
      fi
    fi
    printf '  \033[1;32mok\033[0m   植入 %s 的違規 → 變紅\n' "${name}"
    pass=$((pass+1))
    rm -rf "${tmp}"
  done
  rm -rf "${base}"

  info "反向測試:${pass} 條會紅,${fail} 條不會"
  # ⚠ 「baseline 紅的時候上面那個數字沒有意義」那一條**還在**,但它搬到
  #   這一支的開頭了(見 base_rc 那一段):winbar 的版本是跑完整張表再
  #   回 1,win-next 的版本是**一發現基準不綠就直接 return 1**,連跑都
  #   不跑。後者嚴格得多 —— 基準紅的時候整張表的每一個 ok 都是假的,
  #   印出來只會讓人以為守門有在做事。所以這裡不再讀 baseline_red。
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
if [ "${CHECKED}" -lt 20 ]; then
  # ⚠ 「一條都沒跑卻報通過」是這張檢核表自己最可能的失效方式(§2-G)。
  printf '\033[1;31m只跑了 %d 組檢查 —— 少於下界,當作失敗\033[0m\n' "${CHECKED}" >&2
  exit 1
fi
printf '\033[1;32m%d 組檢查全部通過\033[0m\n' "${CHECKED}"
