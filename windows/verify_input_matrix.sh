#!/usr/bin/env bash
#
# windows/verify_input_matrix.sh — 逐鍵、逐狀態驗證「這顆鍵真的做了它宣稱的事」
#
# ══ 為什麼要有這一支 ═══════════════════════════════════════════════
#
# 使用者回報:「居然無法刪除字,可以打字,不能刪除。」
#
# 而在那一刻,CI 是**全綠**的。原因很簡單:唯一一條真的經過 TSF 的測試
# (verify_installer.sh §6c)送的是 `nihao1` —— 七顆字母與數字,
# **一顆功能鍵都沒有**。退格、Delete、方向鍵、Esc、Enter、Tab 從來沒有被按過。
#
# 更麻煩的是,就算送了退格,舊的假宿主也**驗不出來**:它在輸入法說
# 「我沒吃這顆鍵」之後什麼都不做。於是
#
#     「輸入法放行了,宿主把字刪掉了」   → 文件沒變(因為宿主什麼都沒做)
#     「輸入法吃掉了,但什麼都沒做」     → 文件沒變
#
# 兩者長得一模一樣。所以這一輪 rime_tsf_host.exe 多了一段**宿主的預設處理**
# (見它的 HostDefaultAction):沒被吃掉的鍵,它會像一個真的編輯框那樣
# 插入字元、退格刪一個、方向鍵移游標。這樣「放行」才有可觀察的後果。
#
# ⚠ 因此本腳本的每一格斷言的是**文件內容**,不是「有沒有被吃掉」。
#   「吃掉了」正是這個缺陷的表現形式 —— 拿它當成功標準等於把 bug 寫成規格。
#
# ══ 矩陣 ═══════════════════════════════════════════════════════════
#
# 每一顆會走到 pfEaten=TRUE 的鍵,在**兩種狀態**下各驗一次:
#
#   組字中     退格要刪組字串的最後一個字元、Esc 要取消、數字要選字
#   沒有組字   退格、Delete、方向鍵、Esc、Enter、Tab **一律放行給宿主**
#
# 第二列是最容易錯的那一列,也正是使用者撞到的那一列:一律吃掉的話,
# 使用者在任何程式裡都刪不掉任何東西 —— 那比打不出字嚴重,
# 因為打不出字他還能切回英數,刪不掉字他只能重開程式。
#
# 判準的實作在 windows/common/key_eat_policy.cc,那一層另有一張窮舉的
# 真值表在 Ubuntu 上跑(tests/test_key_eat_policy.cc)。這一支驗的是
# **那張表真的接到了 TSF 上**。兩層都要,少一層就會出現
# 「政策是對的但沒接上」或「接上了但政策是錯的」。
#
# 用法(Git Bash;需要服務已經在跑而且詞庫部署完了):
#   windows/verify_input_matrix.sh --host <rime_tsf_host.exe> [--langid 0x0404]
#                                  [--out <目錄>] [--only <名字>]
#
# 離開碼:0 = 每一格都過。
#
set -uo pipefail

log() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[error]\033[0m %s\n' "$*" >&2; exit 1; }

HOST=""
LANGID="0x0404"
OUT=""
ONLY=""
while [ $# -gt 0 ]; do
  case "$1" in
    --host)   HOST="$2"; shift 2 ;;
    --langid) LANGID="$2"; shift 2 ;;
    --out)    OUT="$2"; shift 2 ;;
    --only)   ONLY="$2"; shift 2 ;;
    *) die "未知參數: $1" ;;
  esac
done
[ -n "${HOST}" ] || die "要 --host <rime_tsf_host.exe>"
[ -f "${HOST}" ] || die "找不到 ${HOST}"
command -v cygpath >/dev/null 2>&1 || die "必須在 Git Bash / MSYS2 下執行"
[ -n "${OUT}" ] || OUT="$(dirname "${HOST}")/inputmatrix"
mkdir -p "${OUT}"

fail=0
note_fail() { printf '\033[1;31m  !! %s\033[0m\n' "$*" >&2; fail=1; }
ok()        { printf '  ✓ %s\n' "$*"; }

# ══════════════════════════════════════════════════════════════════
#  矩陣
# ══════════════════════════════════════════════════════════════════
#
# 每一格四個欄位,用 `|` 分隔:
#   名字 | 文件裡先有的字 | 按鍵序列 | 跑完之後文件必須是什麼
#
# ⚠ 「文件裡先有的字」不是裝飾。沒有組字時按退格,要刪的是**已經在文件裡的
#   字**;文件是空的話,刪成功與刪失敗都是空字串,那一格等於沒有在測。
#
# ⚠ 預期值裡的中文假設預設方案是拼音(與 §6c 的 nihao1 → 你好 同一份)。
#   哪天預設方案換成注音,要改的是這裡的預期值 —— 而不是把這一格拿掉。
MATRIX=$(cat <<'CASES'
idle_backspace_deletes_host_text|abc|{BS}|ab
idle_backspace_twice|abc|{BS}{BS}|a
idle_home_then_delete|abc|{HOME}{DEL}|bc
idle_left_left_backspace|abc|{LEFT}{LEFT}{BS}|bc
idle_end_then_backspace|abc|{HOME}{END}{BS}|ab
idle_esc_enter_tab_change_nothing|abc|{ESC}{ENTER}{TAB}|abc
idle_updown_change_nothing|abc|{UP}{DOWN}{PGUP}{PGDN}|abc
idle_f1_and_insert_change_nothing|abc|{F1}{INS}|abc
idle_space_inserts_a_space|ab|{SPACE}c|ab c
idle_digit_inserts_a_digit||5|5
compose_backspace_edits_the_preedit||nihaoo{BS}1|你好
compose_escape_cancels_everything||nihao{ESC}|
compose_then_backspace_deletes_committed_text||nihao1{BS}|你
CASES
)

run_case() {
  local name="$1" pretext="$2" seq="$3" expect="$4"
  local logf="${OUT}/${name}.log"
  local traf="${OUT}/${name}-trace.log"
  set +e
  "${HOST}" --langid "${LANGID}" \
            --seq "${seq}" \
            --pretext "${pretext}" \
            --expect-doc "${expect}" \
            --require-activate \
            --trace "$(cygpath -w "${traf}")" \
            --wait-ms 3000 > "${logf}" 2>&1
  local rc=$?
  set -e

  local out doc mism
  out="$(tr -d '\r' < "${logf}")"
  doc="$(printf '%s\n' "${out}" | sed -n 's/^  DOC="\(.*\)"$/\1/p' | tail -1)"
  mism="$(printf '%s\n' "${out}" | sed -n 's/^  MISMATCH=\([0-9]*\)$/\1/p' | tail -1)"

  if [ "${rc}" -eq 0 ]; then
    ok "${name}:文件 = \"${doc}\""
  else
    note_fail "${name}:結束碼 ${rc}
     預期文件 = \"${expect}\"
     實際文件 = \"${doc}\"
     完整輸出:${logf}"
    # 逐鍵那幾行是這一格唯一有用的線索 —— 它會指出是哪一顆鍵開始走偏。
    printf '%s\n' "${out}" | grep -a '^  KEY ' | sed 's/^/       /'
  fi

  # ── 黑洞計數 ────────────────────────────────────────────────
  #
  # TestKeyDown 說「吃」而 KeyDown 說「不吃」= 那顆鍵在真的宿主裡會消失。
  # 這支假宿主有預設處理所以看不出來,但真的程式沒有 —— 所以這裡必須是 0。
  if [ -z "${mism}" ]; then
    note_fail "${name}:輸出裡沒有 MISMATCH= 那一行 —— 這一格的一致性沒有被量到"
  elif [ "${mism}" -ne 0 ]; then
    note_fail "${name}:有 ${mism} 顆鍵是「測試說吃、真的處理時說不吃」。
     那幾顆鍵在真的程式裡會直接消失(使用者回報的『可以打字,不能刪除』)。
     修在 windows/common/key_eat_policy.cc:沒有實作的鍵就不要宣告吃掉。"
  fi
}

log "按鍵矩陣(langid=${LANGID},宿主=${HOST})"
ran=0
while IFS='|' read -r name pretext seq expect; do
  [ -n "${name}" ] || continue
  case "${name}" in \#*) continue ;; esac
  if [ -n "${ONLY}" ] && [ "${name}" != "${ONLY}" ]; then continue; fi
  ran=$((ran + 1))
  run_case "${name}" "${pretext}" "${seq}" "${expect}"
done <<< "${MATRIX}"

# 一格都沒跑而回報「全過」是騙人的 —— 這個專案抓過那種測試。
if [ "${ran}" -eq 0 ]; then
  die "一格都沒有跑(--only 打錯了?)"
fi

echo
if [ "${fail}" -eq 0 ]; then
  log "按鍵矩陣:${ran} 格全過"
else
  printf '\033[1;31m==> 按鍵矩陣有格子沒過(共 %d 格)\033[0m\n' "${ran}" >&2
fi
exit "${fail}"
