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
  # ── 按下去畫面要立刻動:兩格都要「樂觀寫入 + 重畫」 ──────────
  #
  # ⚠ 上一版只看 kCellVariant 分支裡有沒有 `simplified_ = `。覆核者
  #   保留樂觀寫入、只拿掉 `Relayout()` + `::InvalidateRect()`,
  #   那一格照樣不會變 —— 而 W26 仍然印 ok。狀態寫了沒有重畫,
  #   使用者看到的與「根本沒寫」一模一樣。
  #
  #   所以現在對**兩格**(中/En 與 简/繁)都要求同一個形狀,而且要求
  #   順序:先寫本地狀態 → 再送出去 → 再重畫。少一步就紅。
  local w26c; w26c="$("${PY}" - "${bar}" <<'PYSCRIPT'
import sys as _s
# ⚠ windows-latest 的 runner 上 python 的 print 吐的是 CRLF。bash 的 $(...)
#   只剥掉末尾的 \n,於是每一行都帶著 \r —— `case "${line}" in SCOPE_OK)`
#   就對不上,而症狀是「未知的回報:SCOPE_OK」這種看不懂的紅字。
#   (實際發生過:CI run 31331902667 的 W12。)
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

# (格名, 樂觀寫入的形態, 送出去的呼叫)
cells = (
    ('kCellMode', 'ascii_mode_ = !now;', 'SetAsciiModeAll('),
    ('kCellVariant', 'simplified_ = !now;', 'SetVariantPref('),
)
for name, write, send in cells:
    c = re.search(r'case ' + name + r': \{(.*?)\n    \}', body, re.S)
    if not c:
        out.append('NOCELL=' + name)
        continue
    b = c.group(1)
    iw = b.find(write)
    if iw < 0:
        out.append('NOOPTIMISTIC=' + name)
        continue
    if send not in b:
        out.append('NOSEND=' + name)
    ir = b.find('Relayout();')
    ii = b.find('::InvalidateRect(hwnd_, nullptr, TRUE);')
    if ir < 0 or ii < 0:
        out.append('NOREPAINT=' + name)
    elif ir < iw or ii < iw:
        out.append('REPAINT_BEFORE_WRITE=' + name)
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
      NOOPTIMISTIC=*)
        w26msg "${l26#NOOPTIMISTIC=} 那一格沒有樂觀寫入本地狀態 —— 指示器要等使用者
     打一個字才會動,而且再按一次送的是同一個值(它是拿本地狀態反推的)" ;;
      NOSEND=*)
        w26msg "${l26#NOSEND=} 那一格只改了本地狀態,沒有把新值送出去 ——
     指示器動了而引擎沒動,那比不動更糟" ;;
      NOREPAINT=*)
        w26msg "${l26#NOREPAINT=} 那一格寫了狀態卻沒有 Relayout() + InvalidateRect() ——
     **那一格照樣不會變**。使用者看到的與根本沒寫一模一樣,
     而只看「有沒有寫入」的檢查會是綠的(上一輪就是這樣被拆掉的)。" ;;
      REPAINT_BEFORE_WRITE=*)
        w26msg "${l26#REPAINT_BEFORE_WRITE=} 那一格先重畫才寫狀態 —— 畫出來的是舊值" ;;
      *) w26msg "未知的回報:${l26}" ;;
    esac
  done <<< "${w26c}"
  [ "${w26bad}" -eq 0 ] && ok "W26 狀態列寬度一變就重走 PlaceStatusBar,而且 简/繁 那一格按下去立刻改變"

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
      'facts.engine_says_not_ready = engine_not_ready_.load();',
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
      'facts.engine_says_not_ready = deploying_;',
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
  #   common/net_ui.cc(純函式,有單元測試);這裡驗的是那三條接線
  #   真的接上了,而且沒有第二條路繞過去。
  #
  # ⚠ 這一段的 python 失敗時**必須是紅**(見 W25 的教訓),所以它先印
  #   SCOPE_OK;沒有那一行就當作沒跑過。
  check
  local w29bad=0
  local w29out; w29out="$("${PY}" - "${sw}" "${CODE_DIR}/common/net_ui.cc" <<'PYSCRIPT'
import sys as _s
_s.stdout.reconfigure(encoding='utf-8', newline='')
import re, sys
sw = open(sys.argv[1], encoding='utf-8', errors='replace').read()
try:
    net = open(sys.argv[2], encoding='utf-8', errors='replace').read()
except OSError:
    net = ''

def body_of(src, head, endpat='\n}\n'):
    i = src.find(head)
    if i < 0:
        return None
    j = src.find(endpat, i)
    return src[i:j] if j > 0 else src[i:]

out = []

# -- 1. 純函式那一側還在 common/,而且是這三支 --
for name in ('UpdateAction DecideUpdateAction(',
             'UiString NetSwitchSummary(',
             'NetLogView BuildNetLogView('):
    if name not in net:
        out.append('NO_PUREFN=' + name.split(' ')[-1].rstrip('('))

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

# -- 5. 檢查更新:開關先問,而且不在 UI 執行緒上跑 --
up = body_of(sw, 'void SettingsWindow::StartUpdateCheck() {')
if up is None:
    out.append('NO_STARTUPDATE')
else:
    if not re.search(r'DecideUpdateAction\(\s*net_gate_\.Enabled\(\),\s*update_running_\s*\)', up):
        out.append('UPDATE_SKIPS_THE_SWITCH')
    if 'if (action != UpdateAction::kStart) return;' not in up:
        out.append('UPDATE_IGNORES_THE_VERDICT')
    if 'CreateThread' not in up:
        out.append('UPDATE_NOT_ON_A_THREAD')
    if 'RunUpdateCheck' in up:
        out.append('UPDATE_RUNS_ON_THE_UI_THREAD')
th = body_of(sw, 'DWORD WINAPI SettingsWindow::UpdateThreadEntry(LPVOID param) {')
if th is None:
    out.append('NO_UPDATE_THREAD')
elif 'RunUpdateCheck(job->gate)' not in th:
    out.append('THREAD_DOES_NOT_CHECK')
done = body_of(sw, 'void SettingsWindow::OnUpdateCheckDone(')
if done is None:
    out.append('NO_UPDATE_DONE')
else:
    if 'UpdateStateText(' not in done:
        out.append('RESULT_TEXT_HARDCODED')
    if 'RefreshNetworkPage()' not in done:
        out.append('RESULT_DOES_NOT_REFRESH')

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
        w29msg "common/net_ui.cc 裡沒有 ${line29#NO_PUREFN=} —— 那三件事的判斷又回到
     繪製碼裡了,而那個檔案在 Ubuntu 上編不起來(= 沒有人驗得到)" ;;
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
      NO_STARTUPDATE) w29msg "找不到 StartUpdateCheck —— 掃描範圍錯了" ;;
      UPDATE_SKIPS_THE_SWITCH)
        w29msg "「檢查更新」沒有把 net_gate_.Enabled() 送進 DecideUpdateAction() ——
     **開關關著也會連出去**。這是這一頁上最嚴重的一種寫壞法,而且
     畫面上看起來完全正常(只多一句「正在檢查更新…」)" ;;
      UPDATE_IGNORES_THE_VERDICT)
        w29msg "算出了判斷卻沒有據此收手(預期 if (action != UpdateAction::kStart) return;)" ;;
      UPDATE_NOT_ON_A_THREAD)
        w29msg "「檢查更新」沒有開背景執行緒 —— 同步阻塞跑在 UI 執行緒上就是
     「打字打到一半整個沒反應」(候選窗與設定視窗共用那條執行緒)" ;;
      UPDATE_RUNS_ON_THE_UI_THREAD)
        w29msg "StartUpdateCheck 裡直接呼叫了 RunUpdateCheck —— 見上一條" ;;
      NO_UPDATE_THREAD) w29msg "找不到 UpdateThreadEntry —— 掃描範圍錯了" ;;
      THREAD_DOES_NOT_CHECK)
        w29msg "背景執行緒沒有呼叫 RunUpdateCheck(job->gate) —— 那條路是死的" ;;
      NO_UPDATE_DONE) w29msg "找不到 OnUpdateCheckDone —— 掃描範圍錯了" ;;
      RESULT_TEXT_HARDCODED)
        w29msg "檢查完的那一句話不是 UpdateStateText() 給的 —— 五種結果會被壓成同一句" ;;
      RESULT_DOES_NOT_REFRESH)
        w29msg "檢查完沒有重讀連網紀錄 —— 使用者按完更新,就在這一頁上,
     卻看不到剛剛那幾筆連線" ;;
      NO_CLEAR) w29msg "找不到 DoClearNetLog —— 掃描範圍錯了" ;;
      CLEAR_WITHOUT_CONFIRM)
        w29msg "清除連網紀錄沒有確認 —— 那份紀錄是使用者用來稽核我們的證據,
     清掉就找不回來了(§2-C2/§2-C3)" ;;
      CLEAR_DOES_NOTHING)
        w29msg "「清除連網紀錄」沒有呼叫 net_gate_.ClearLog()" ;;
      *) w29msg "未知的回報:${line29}" ;;
    esac
  done <<< "${w29out}"
  [ "${w29bad}" -eq 0 ] && ok "W29 連網那一頁:開關讀寫都走 NetGate、那兩句話與空狀態分支都從純函式來、${w29calls} 個版面呼叫點全部走 PageStateNow(),而「檢查更新」在開關關著時走不到連線那一條路"
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
"W24d 表上少三顆|service/settings_window.cc|s=s.replace('    {IDC_THEME_0, L\"BUTTON\", RADIO1, UiString::kThemeFollowSystem},','',1).replace('    {IDC_THEME_1, L\"BUTTON\", RADIO, UiString::kThemeLight},','',1).replace('    {IDC_THEME_2, L\"BUTTON\", RADIO, UiString::kThemeDark},','',1)"
"W25 拿掉滾輪|service/settings_window.cc|s=s.replace('case WM_MOUSEWHEEL:','case WM_NULL + 4242:',1)"
"W25b 又把高度丟掉|common/ui_layout.cc|s=s.replace('int ScrollMaxDip(int page, int window_w_dip, int window_h_dip,','int ScrollMaxDipRemoved(int page, int window_w_dip, int window_h_dip,',1)"
"W26 狀態列不重擺|service/status_bar.cc|s=s.replace('  ApplyPlacement(MulDivRound(total_w, 96, static_cast<int>(dpi_)));','',1)"
"W26b 简繁不樂觀寫入|service/status_bar.cc|s=s.replace('        now = simplified_;\n        simplified_ = !now;\n      }\n      // 走設定視窗那一支','        now = simplified_;\n      }\n      // 走設定視窗那一支',1)"
"W27a Relayout 不再問狀態|service/status_bar.cc|s=s.replace('  service_state_ = CurrentServiceState();','  service_state_ = ServiceState::kReady;',1)"
"W27b 那一橫的字寫死一句|service/status_bar.cc|s=s.replace('    c.text = UiText(StatusTextFor(service_state_));','    c.text = UiText(UiString::kBarNotRunning);',1)"
"W27c 不讀線路上的旗標|service/status_bar.cc|s=s.replace('SnapshotSaysNotReady(snap.status_flags)','false',1)"
"W27d 事實少餵一格|service/status_bar.cc|s=s.replace('  facts.engine_says_not_ready = engine_not_ready_.load();','  facts.engine_says_not_ready = false;',1)"
"W28 自繪直接用 nmcd.rc|service/settings_window.cc|s=s.replace('LRESULT SettingsWindow::DrawSchemaList(NMLVCUSTOMDRAW* cd) {','LRESULT SettingsWindow::DrawSchemaList(NMLVCUSTOMDRAW* cd) { RECT sneaky = cd->nmcd.rc; (void)sneaky;',1)"
"W9 少一條單元測試|tests/test_status_cells.cc|s=s.replace('TEST(status_cells_input_mode_shows_exactly_one_label)','TEST(status_cells_renamed_away)',1)"
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
"W26c 简/繁 寫了狀態但不重畫(覆核者實測的拆法)|service/status_bar.cc|s=s.replace('                                      : VariantPref::kSimplified);\\n      Relayout();\\n      ::InvalidateRect(hwnd_, nullptr, TRUE);','                                      : VariantPref::kSimplified);',1)"
"W26d 中/En 寫了狀態但不重畫|service/status_bar.cc|s=s.replace('      if (engine_) engine_->SetAsciiModeAll(!now);\\n      Relayout();\\n      ::InvalidateRect(hwnd_, nullptr, TRUE);','      if (engine_) engine_->SetAsciiModeAll(!now);',1)"
"W29a 開關的狀態讀錯地方|service/settings_window.cc|s=s.replace('const bool on = net_gate_.Enabled();','const bool on = settings_.NetworkEnabled();',1)"
"W29b 開關底下那句話寫死一條|service/settings_window.cc|s=s.replace('SetText(hwnd_, IDC_NET_STATE, UiText(NetSwitchSummary(on)));','SetText(hwnd_, IDC_NET_STATE, UiText(UiString::kNetworkOffSummary));',1)"
"W29c 檢查更新不問開關(開關關著也連出去)|service/settings_window.cc|s=s.replace('DecideUpdateAction(net_gate_.Enabled(), update_running_)','DecideUpdateAction(true, update_running_)',1)"
"W29d 算了判斷卻不收手|service/settings_window.cc|s=s.replace('  if (action != UpdateAction::kStart) return;','',1)"
"W29e 檢查更新在 UI 執行緒上直接跑|service/settings_window.cc|s=s.replace('  update_running_ = true;','  (void)RunUpdateCheck(&net_gate_);' + chr(10) + '  update_running_ = true;',1)"
"W29f 空狀態的版面分支被寫死|service/settings_window.cc|s=s.replace('  s.net_log_empty = net_log_empty_;','  s.net_log_empty = false;',1)"
"W29g 紀錄不是從出口讀來的|service/settings_window.cc|s=s.replace('BuildNetLogView(net_gate_.ReadLog(), ui_lang_, LocalTzOffsetMinutes())','BuildNetLogView({}, ui_lang_, LocalTzOffsetMinutes())',1)"
"W29h 檢查完的那一句話寫死|service/settings_window.cc|s=s.replace('  SetStatus(' + chr(10) + '      UpdateStateText(result ? result->state : UpdateCheckState::kFailed));','  SetStatus(UiString::kUpdateUpToDate);',1)"
"W29i 清除紀錄不問一聲|service/settings_window.cc|s=s.replace('  if (!ConfirmDialog(hwnd_, &theme_, script(),' + chr(10) + '                     UiText(UiString::kNetLogClearHeading),' + chr(10) + '                     UiText(UiString::kNetLogClearBlurb),' + chr(10) + '                     UiText(UiString::kNetLogClearButton),' + chr(10) + '                     UiText(UiString::kCancel)))' + chr(10) + '    return;','',1)"
"W29j 開關寫不進去卻不說|service/settings_window.cc|s=s.replace('    SetStatus(UiString::kStatusSaveFailed);' + chr(10) + '    RefreshNetworkPage();' + chr(10) + '    return;','    RefreshNetworkPage();' + chr(10) + '    return;',1)"
"W29k 純函式從 common/ 消失|common/net_ui.cc|s=s.replace('UpdateAction DecideUpdateAction(','UpdateActionGone DecideUpdateActionGone(',1)"
"W29l 版面呼叫點繞過真實狀態|service/settings_window.cc|s=s.replace('  const PageLayout pl = LayoutSettingsPageDip(page_, W, PageStateNow());','  const PageLayout pl = LayoutSettingsPageDip(page_, W, PageState{});',1)"
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
if [ "${CHECKED}" -lt 20 ]; then
  # ⚠ 「一條都沒跑卻報通過」是這張檢核表自己最可能的失效方式(§2-G)。
  printf '\033[1;31m只跑了 %d 組檢查 —— 少於下界,當作失敗\033[0m\n' "${CHECKED}" >&2
  exit 1
fi
printf '\033[1;32m%d 組檢查全部通過\033[0m\n' "${CHECKED}"
