#!/usr/bin/env bash
#
# verify_core_data_fanout.sh — 動到 core/data/ 就要四條車道都跑過
#
# ═══════════════════════════════════════════════════════════════════════════
#  這一關是 2026-08-12 那次撤回換來的
# ═══════════════════════════════════════════════════════════════════════════
#
# 那一批改的是 `core/data/`（opencc 補充表 + 一支 lua filter）。它在
# emulator-5558 上驗過、綠的，於是併進 main —— 然後 **macOS 車道紅、
# Android 車道紅**，整批被 `git revert -m 1` 撤回。
#
# `core/data/` 是四端**共用**的執行期資料：Android、macOS、Windows 都會
# `scripts/collect_data.sh` 把它裝進自己的產物，四端載的是同一份 librime 與
# 同一份 lua。改它 = 同時改四個產品，而在一端驗過只證明了四分之一。
#
# 而支線預設**只跑得到自己接了線的那幾條車道**：每一份 workflow 的
# `on: push: branches:` 各自一份清單，慢車道的 job 還有第二道 `if:`。
# 沒接的車道不會紅 —— 它**根本不會跑**，而 checks 上被跳過的 job 是
# 灰色的勾，和跑過而且通過長得一模一樣。「我在 CI 上看過了」因此可以是
# 真心話，卻仍然只驗了一端。
#
# 所以這一關問的是一個機械問題：
#
#     這次的改動動到 core/data/ 了嗎？動到的話，**這條分支**在每一條
#     讀 core/data/ 的車道上，那個 job 真的會跑嗎？
#
# ═══════════════════════════════════════════════════════════════════════════
#  2026-08-13：它守著輸出，而沒有人守輸入
# ═══════════════════════════════════════════════════════════════════════════
#
# cand 那一批把 `menu/page_size` 從 5 改成 9 —— 它自己的 commit 訊息寫著
# 「四端共用，桌面候選窗也會從 5 變 9」。而這一關對它**整關沉默**：
#
#     $ bash scripts/verify_core_data_fanout.sh --base main
#     這一次沒有動到 core/data/（比對 main）—— 這一關沒有話要說。
#
# 成因是一行 pathspec 加一行 .gitignore：
#
#     CHANGED=$(git diff --name-only ... -- core/data)
#     .gitignore:  /core/data/shared
#                  /core/data/user
#
# 那兩個目錄是 **產物**，而且不入版控 —— `git diff` 對它們永遠是空的。
# 真正的改動在 `scripts/collect_data.sh`（產生器）第 98–104 行的那個
# `sed -i 's/page_size: 5/page_size: 9/'`，而當時**沒有任何一關在看它**。
#
# 對照組成立：同一批裡的 b1 改的是版控裡的 `core/data/schemas/…`，同一支
# 腳本 EXIT=1 並逐條點名三條車道跑不到。**這道關本身是好的，它只是守著
# 輸出、而沒有人守輸入。**
#
# 所以現在「動到 core/data/」= 產物 ∪ **產生器**。產生器的清單是
# scripts/lib/shared_data_writers.py **掃**出來的，不是寫死的 ——
# 寫死的清單會在下一支產生器出現時安靜地漏掉它，那正是這一段要消滅的
# 失敗模式。掃描器自己有反向測試（分不分得出「寫」與「讀」），而這裡
# 再釘一條下界：已知的產生器一個都不許掃不到。
#
# 判斷「會不會跑」的是 scripts/ci_branch_gate.py（它看得懂兩道閘門）。
# 這裡只負責：找出哪些車道讀 core/data/、把分支名餵給它、把答案講清楚。
#
# ⚠ 車道是**掃出來的**，不是寫死的。新加一條讀 core/data/ 的 workflow 而
#   沒有在下面的表裡登記，這支腳本會紅並指名是哪一份 —— 因為那時它已經
#   不知道要拿什麼當「那個 job 真的會跑」的證據了。
#
# 用法:
#   scripts/verify_core_data_fanout.sh                  # 對 origin/main 比對
#   scripts/verify_core_data_fanout.sh --base <ref>
#   scripts/verify_core_data_fanout.sh --self-test      # 證明它分得出紅綠
#
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GATE="$ROOT/scripts/ci_branch_gate.py"
SCANNER="$ROOT/scripts/lib/shared_data_writers.py"
WF_DIR="$ROOT/.github/workflows"

log() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[error]\033[0m %s\n' "$*" >&2; exit 1; }
PASS=0; FAIL=0
ok()  { echo "  [PASS] $*"; PASS=$((PASS+1)); }
bad() { echo "  [FAIL] $*" >&2; FAIL=$((FAIL+1)); }

BASE=""
SELF_TEST=0
while [ $# -gt 0 ]; do
  case "$1" in
    --base)      BASE="$2"; shift 2 ;;
    --self-test) SELF_TEST=1; shift ;;
    # ⚠ 不要寫死行號:檔頭補過一節之後,'2,45p' 會停在半句話上。
    -h|--help)   sed -n '2,/^set -uo pipefail$/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) die "未知參數: $1" ;;
  esac
done

[ -f "$GATE" ] || die "找不到 $GATE"
[ -f "$SCANNER" ] || die "找不到 $SCANNER"

# ── 產生器 ────────────────────────────────────────────────────────────────
#
# 「會寫進 core/data/shared 或 core/data/user 的版控腳本」。掃出來的，見
# scripts/lib/shared_data_writers.py。下面兩張表是它的**下界**與**例外**，
# 兩者的失敗方向刻意不同：
#
#   · MIN  掃不到就死。掃描器認的是形狀不是語意，改壞了會安靜地少掃幾支 ——
#          而「少掃」的長相正是這一關 2026-08-13 之前的樣子。
#   · SKIP 掃得到但不算。每一項都要寫**為什麼它不決定出貨內容**，
#          而且它必須真的被掃到（掃不到 = 這一行過期了，見自我測試）。
#
# ⚠ SKIP 的方向是安全的：忘了加只會多亮一次紅燈，不會變成安靜的綠燈。
GEN_MIN=(
  scripts/collect_data.sh
  scripts/collect_charset_guard.sh
)
GEN_SKIP=(
  # 它把測試詞庫寫進 core/data/user 再於 trap 裡還原（檔頭第 35 行自己寫了）。
  # 借用那個目錄，不產生出貨內容。
  "apple/scripts/verify_user_dict.sh|借 core/data/user 跑測試，跑完還原"
  # 只有 --plant stale-schema 那條植入分支會 mkdir 一次；真正的產物寫在
  # build/ 底下。它是這一關的使用者，不是它的輸入。
  "scripts/verify_syllables.sh|只在 --plant 分支建一次目錄，產物寫在 build/"
)

gen_skipped() {
  local f="$1" e
  for e in "${GEN_SKIP[@]}"; do [ "${e%%|*}" = "$f" ] && return 0; done
  return 1
}

# 掃出來、扣掉 SKIP 之後的產生器清單。
generators() {
  local f
  while IFS= read -r f; do
    [ -n "$f" ] || continue
    gen_skipped "$f" || printf '%s\n' "$f"
  done < <(python3 "$SCANNER" --root "$ROOT")
}

# ── 車道登記表 ─────────────────────────────────────────────────────────────
# 一行一條：<workflow 檔名>|<針>|<針>…
#
# 「針」是那條車道上**真的會碰到 core/data/ 的那一步**的字串。
# ci_branch_gate.py 會確認:(1) 這條分支會觸發這份 workflow、
# (2) 針所在的那個 job 的 if: 也認這條分支。兩道都過才算「會跑」。
#
# 為什麼慢車道要單獨給一根針:build.yml 的快車道與慢車道是兩個 job，
# 各有各的 if:。只驗快車道會漏掉「模擬器那一段整個被跳過」的情形 ——
# 那正是這一輪踩過的坑。
LANES=(
  "build.yml|scripts/collect_data.sh|release_check.sh --emu-only"
  "macos.yml|scripts/collect_data.sh|apple/scripts/verify_console.sh"
  "windows.yml|scripts/collect_data.sh|windows/verify_console.sh"
)

lane_file()  { printf '%s' "${1%%|*}"; }
lane_needles() { printf '%s' "${1#*|}"; }

# ── 掃出「讀 core/data/ 的 workflow」──────────────────────────────────────
# 只看非註解行:註解裡提到 core/data/ 的地方不少(它們是在解釋，不是在讀)。
discover_lanes() {
  local f
  for f in "$WF_DIR"/*.yml; do
    [ -f "$f" ] || continue
    if sed 's/^[[:space:]]*#.*$//' "$f" \
       | grep -qE 'core/data|collect_data\.sh'; then
      basename "$f"
    fi
  done
}

check_lane() {  # check_lane <workflow 檔名> <分支> <針…>
  local wf="$1" branch="$2"; shift 2
  local args=(--workflow "$WF_DIR/$wf" --branch "$branch")
  local n
  for n in "$@"; do args+=(--needle "$n"); done
  python3 "$GATE" "${args[@]}" > /tmp/.fanout.$$ 2>&1
  local rc=$?
  if [ "$rc" -eq 0 ]; then
    rm -f /tmp/.fanout.$$
    return 0
  fi
  sed 's/^/         /' /tmp/.fanout.$$ >&2
  rm -f /tmp/.fanout.$$
  # 2 = 工具自己判斷不了。那不可以當成綠,但也要與「確定不會跑」分開講。
  return "$rc"
}

# ── 產生器「動到了沒有」:按**內容**判,不按檔名 ───────────────────────────
#
# ⛔ 2026-08-13 的 4a726e3 給 `scripts/collect_data.sh` 與
#   `scripts/collect_charset_guard.sh` 各加了 9 行 `-h|--help` 的**唯讀出口**
#   (`verify_script_readonly.sh` 要求每一支腳本的 `--help` 都 RC=0 且無副作用)。
#   兩支都是產生器,於是這一關由綠轉紅、逐條點名三條車道跑不到 ——
#   而那 18 行**一個位元組的產物都沒有改變**:`$1` 是輸出目錄,
#   永遠不會等於 `-h`/`--help`,那個 case 對真正的呼叫恆為落空。
#
#   「把 --help 撤回」不是解:那會讓 verify_script_readonly.sh 轉紅,
#   等於把紅燈從一支守門搬到另一支。所以改成問**產物相關的內容**變了沒有。
#
# 判準(刻意笨,而且 fail-closed):把兩版的原始碼各自正規化 ——
#   1. 去掉整行註解與空行(註解不會被執行);
#   2. 去掉**第一個**唯讀說明出口 `case "${1:-}" in … esac`,
#      而且只有在它的每一行都是「印字或 exit 0」時才去掉。
#      認不出形狀 → **不去掉**(於是算成動到了)。
# 正規化之後兩版一樣 → 這一支沒有動到產物。
#
# ⚠ 這一條擋不住「改了註解裡的資料」這種事,但註解本來就不會被執行;
#   而它**不會**放過任何一行真的程式碼 —— cand 那一批的
#   `sed -i 's/page_size: 5/page_size: 9/'` 照樣算動到(自我測試釘住了)。
gen_data_changed() {  # gen_data_changed <base-ref> <rel-path…>
  python3 - "$ROOT" "$@" <<'PY'
import re, subprocess, sys

root, base, rels = sys.argv[1], sys.argv[2], sys.argv[3:]

_CASE_HEAD = re.compile(r'^[ \t]*case[ \t]+"?\$\{1:-\}"?[ \t]+in[ \t]*$')
_ESAC = re.compile(r'^[ \t]*esac[ \t]*$')
# 區塊裡允許出現的形狀:說明的樣式分支、印字、以 0 結束。其餘一律不認。
_OK_IN_BLOCK = re.compile(
    r'^[ \t]*(?:'
    r'(?:-h\|--help|--help\|-h|-h|--help)\)'      # 樣式分支
    r'|(?:sed|echo|cat|printf|head|tail)\b'        # 印字
    r'|exit[ \t]+0(?:[ \t]*;;)?[ \t]*$'            # 以 0 結束
    r'|;;[ \t]*$'
    r')'
)


def strip_help_block(text):
    """去掉第一個「印完說明就 exit 0」的 case 區塊。認不出形狀就原樣回傳。"""
    lines = text.splitlines()
    for i, line in enumerate(lines):
        if not _CASE_HEAD.match(line):
            continue
        for j in range(i + 1, len(lines)):
            if _ESAC.match(lines[j]):
                body = lines[i + 1:j]
                clean = [b for b in body if b.strip() and not b.strip().startswith('#')]
                if clean and all(_OK_IN_BLOCK.match(b) for b in clean):
                    return '\n'.join(lines[:i] + lines[j + 1:])
                return text          # 形狀不對 → 不去掉(fail-closed)
            if _CASE_HEAD.match(lines[j]):
                break                # 巢狀 case → 不碰
        return text
    return text


def normalize(text):
    out = []
    for line in strip_help_block(text).splitlines():
        s = line.strip()
        if not s or s.startswith('#'):
            continue
        out.append(s)
    return '\n'.join(out)


# `--pair a b`:直接比兩個檔案(自我測試用的入口,走的是同一份 normalize)。
if base == '--pair':
    a, b = rels[0], rels[1]
    with open(a, encoding='utf-8') as fa, open(b, encoding='utf-8') as fb:
        sys.exit(0 if normalize(fa.read()) == normalize(fb.read()) else 1)


def at_base(rel):
    p = subprocess.run(['git', '-C', root, 'show', '%s:%s' % (base, rel)],
                       stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, check=False)
    if p.returncode != 0:
        return None              # base 上還沒有這一支 = 新增的產生器 → 算動到
    return p.stdout.decode('utf-8', 'replace')


for rel in rels:
    old = at_base(rel)
    try:
        with open('%s/%s' % (root, rel), 'r', encoding='utf-8', errors='replace') as fh:
            new = fh.read()
    except OSError:
        print(rel)               # 讀不到 = 刪掉了 → 算動到
        continue
    if old is None or normalize(old) != normalize(new):
        print(rel)
PY
}

# ═══════════════════════════════════════════════════════════════════════════
if [ "$SELF_TEST" -eq 1 ]; then
  log "自我測試:這支腳本分得出「接了線」與「沒接線」嗎"
  # main 一定接得到每一條(所有 workflow 的 branches 都列了 main)。
  for lane in "${LANES[@]}"; do
    wf="$(lane_file "$lane")"
    IFS='|' read -r -a needles <<< "$(lane_needles "$lane")"
    if check_lane "$wf" main "${needles[@]}"; then
      ok "$wf 在 main 上會跑"
    else
      bad "$wf 在 main 上被判成不會跑 —— 這支腳本壞了,不是 workflow 壞了"
    fi
  done
  # 一條不存在的分支一定接不到(它不在任何一份 branches: 清單裡)。
  for lane in "${LANES[@]}"; do
    wf="$(lane_file "$lane")"
    IFS='|' read -r -a needles <<< "$(lane_needles "$lane")"
    if check_lane "$wf" zzz-not-a-real-branch "${needles[@]}" 2>/dev/null; then
      bad "$wf 對一條根本不存在的分支也說會跑 —— 這一關是裝飾品"
    else
      ok "$wf 對沒接線的分支會紅"
    fi
  done
  echo
  log "自我測試:產生器掃得出來嗎"
  # 掃描器自己的反向測試（分不分得出「寫進去」與「只是讀」）。
  if python3 "$SCANNER" --self-test > /tmp/.gen.$$ 2>&1; then
    ok "shared_data_writers.py 的反向測試全過"
  else
    sed 's/^/         /' /tmp/.gen.$$ >&2
    bad "shared_data_writers.py 的反向測試沒過 —— 掃描器壞了,下面的清單不可信"
  fi
  rm -f /tmp/.gen.$$
  RAW="$(python3 "$SCANNER" --root "$ROOT")"
  # 下界:已知的產生器一個都不許掃不到。
  for g in "${GEN_MIN[@]}"; do
    if printf '%s\n' "$RAW" | grep -qxF "$g"; then
      ok "掃得到產生器 $g"
    else
      bad "$g 是產生器,掃描器卻沒掃到 —— 這一關又回到只守輸出的那一版了"
    fi
  done
  # 例外清單不可以過期:寫在 SKIP 裡卻根本掃不到,代表那一行在騙人。
  for e in "${GEN_SKIP[@]}"; do
    f="${e%%|*}"
    if printf '%s\n' "$RAW" | grep -qxF "$f"; then
      ok "SKIP 的 $f 確實掃得到(${e#*|})"
    else
      bad "SKIP 裡的 $f 根本掃不到 —— 那一行過期了,拿掉它"
    fi
  done
  n_gen="$(generators | grep -c . || true)"
  [ "$n_gen" -ge "${#GEN_MIN[@]}" ] \
    && ok "扣掉 SKIP 之後還有 $n_gen 支產生器" \
    || bad "扣掉 SKIP 之後只剩 $n_gen 支 —— SKIP 把該守的也扣掉了"

  echo
  log "自我測試:產生器「動到了沒有」按**內容**判(不按檔名)"
  # 四份原始碼跑同一份 normalize:只加唯讀說明出口的不算動到,改到真的
  # 程式碼的一定算,而認不出形狀的 case 區塊**寧可算動到**(fail-closed)。
  _FT="$(mktemp -d)"
  cat > "$_FT/base.sh" <<'FANOUT_ST'
#!/usr/bin/env bash
set -euo pipefail
OUT="$ROOT/build/fanout-selftest/out"
mkdir -p "$OUT"
sed -i 's/page_size: 5/page_size: 9/' "$OUT/default.yaml"
FANOUT_ST
  cat > "$_FT/plus-help.sh" <<'FANOUT_ST'
#!/usr/bin/env bash
set -euo pipefail

# ⛔ 唯讀出口。
case "${1:-}" in
  -h|--help)
    sed -n '2,/^set -[eu]/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 0 ;;
esac

OUT="$ROOT/build/fanout-selftest/out"
mkdir -p "$OUT"
sed -i 's/page_size: 5/page_size: 9/' "$OUT/default.yaml"
FANOUT_ST
  cat > "$_FT/real-change.sh" <<'FANOUT_ST'
#!/usr/bin/env bash
set -euo pipefail
OUT="$ROOT/build/fanout-selftest/out"
mkdir -p "$OUT"
sed -i 's/page_size: 5/page_size: 6/' "$OUT/default.yaml"
FANOUT_ST
  cat > "$_FT/sneaky-case.sh" <<'FANOUT_ST'
#!/usr/bin/env bash
set -euo pipefail

case "${1:-}" in
  -h|--help)
    rm -rf "$ROOT/build/fanout-selftest/out"
    exit 0 ;;
esac

OUT="$ROOT/build/fanout-selftest/out"
mkdir -p "$OUT"
sed -i 's/page_size: 5/page_size: 9/' "$OUT/default.yaml"
FANOUT_ST
  if gen_data_changed --pair "$_FT/base.sh" "$_FT/plus-help.sh"; then
    ok "只加一段唯讀「--help」出口 → **不算**動到產物(4a726e3 的那 18 行)"
  else
    bad "只加唯讀「--help」出口卻被判成動到產物 —— 這一關又會為了說明文字亮紅燈"
  fi
  if gen_data_changed --pair "$_FT/base.sh" "$_FT/real-change.sh"; then
    bad "改了 page_size 卻被判成沒動到 —— 這一關變成裝飾品(cand 那一批的形狀)"
  else
    ok "改一行 page_size → 算動到產物"
  fi
  if gen_data_changed --pair "$_FT/base.sh" "$_FT/sneaky-case.sh"; then
    bad "「--help」分支裡藏了 rm -rf 卻被當成唯讀出口略過 —— 判準不是 fail-closed"
  else
    ok "認不出形狀的 case 區塊(分支裡有 rm -rf)→ 算動到(fail-closed)"
  fi
  rm -rf "$_FT"
  echo
  echo "═══ core/data 擴散守門(自我測試):$PASS 過、$FAIL 失敗 ═══"
  [ "$FAIL" -eq 0 ] || exit 1
  exit 0
fi

# ── 這次有沒有動到 core/data/ ──────────────────────────────────────────────
if [ "${GITHUB_EVENT_NAME:-}" = "pull_request" ]; then
  echo "這是 pull_request:四條車道的 pull_request 觸發都列了 main,一定都會跑。"
  exit 0
fi

BRANCH="${GITHUB_REF_NAME:-$(git -C "$ROOT" rev-parse --abbrev-ref HEAD 2>/dev/null)}"
[ -n "$BRANCH" ] || die "問不出目前的分支名"
if [ "$BRANCH" = "main" ]; then
  echo "分支是 main:四條車道都會跑。"
  exit 0
fi

if [ -z "$BASE" ]; then
  if git -C "$ROOT" rev-parse --verify -q origin/main >/dev/null; then
    BASE=origin/main
  elif git -C "$ROOT" rev-parse --verify -q main >/dev/null; then
    BASE=main
  else
    # CI 上 checkout 是淺的,把 main 那一個 commit 抓下來就夠比對 tree 了。
    git -C "$ROOT" fetch --no-tags --depth=1 origin main >/dev/null 2>&1 \
      && BASE=FETCH_HEAD
  fi
fi
[ -n "$BASE" ] || die "找不到可以比對的 main（用 --base 指定）"

# ⚠ 兩組 pathspec，不是一組：
#   · core/data       —— **產物**。但 shared/ 與 user/ 在 .gitignore 裡，
#                        diff 得出來的只有 schemas/、lua/、opencc/ 那些。
#   · 產生器          —— 掃出來的（見檔頭 2026-08-13 那一節）。改了它們，
#                        四端裝進產物裡的東西就變了，而 git 一個字都看不到。
mapfile -t GEN < <(generators)
[ "${#GEN[@]}" -gt 0 ] || die "一支產生器都掃不到 —— 掃描器壞了(跑 --self-test 看)"
# 下界也要在正式跑的路徑上釘住,不能只在自我測試裡。
for g in "${GEN_MIN[@]}"; do
  printf '%s\n' "${GEN[@]}" | grep -qxF "$g" \
    || die "$g 應該被當成產生器,卻不在掃描結果裡 —— 跑 --self-test 看是哪裡壞了"
done

diff_paths() {  # diff_paths <pathspec…>
  git -C "$ROOT" diff --name-only "$BASE" HEAD -- "$@" 2>/dev/null
  # 還沒 commit 的改動也算 —— 本機跑這一支時通常就是為了問「我現在改的東西」。
  git -C "$ROOT" status --porcelain -- "$@" 2>/dev/null | sed 's/^...//'
}

CHANGED_DATA="$(diff_paths core/data | sed '/^$/d' | sort -u)"
# ⚠ 產生器**不按檔名判**,按內容 —— 見 gen_data_changed 的檔頭理由。
CHANGED_GEN="$(gen_data_changed "$BASE" "${GEN[@]}" | sed '/^$/d' | sort -u)"
CHANGED="$(printf '%s\n%s\n' "$CHANGED_DATA" "$CHANGED_GEN" | sed '/^$/d' | sort -u)"

if [ -z "$CHANGED" ]; then
  echo "這一次沒有動到 core/data/,也沒有動到產生它的 ${#GEN[@]} 支腳本（比對 $BASE）—— 這一關沒有話要說。"
  exit 0
fi

if [ -n "$CHANGED_DATA" ]; then
  log "動到 core/data/ 的檔案（比對 $BASE）"
  printf '%s\n' "$CHANGED_DATA" | sed 's/^/    /'
fi
if [ -n "$CHANGED_GEN" ]; then
  log "動到**產生器**的檔案（比對 $BASE）—— 產物在 .gitignore 裡,git diff 看不到它們變了什麼"
  printf '%s\n' "$CHANGED_GEN" | sed 's/^/    /'
fi
echo
log "那麼每一條讀 core/data/ 的車道,在分支「$BRANCH」上都跑得到嗎"

# 掃出來的車道必須全部在登記表裡,否則這支腳本就有一條看不見的路。
for wf in $(discover_lanes); do
  known=0
  for lane in "${LANES[@]}"; do
    [ "$(lane_file "$lane")" = "$wf" ] && known=1
  done
  if [ "$known" -eq 0 ]; then
    bad "$wf 讀 core/data/,但它不在 verify_core_data_fanout.sh 的登記表裡 —— 補一行針進去"
  fi
done

for lane in "${LANES[@]}"; do
  wf="$(lane_file "$lane")"
  [ -f "$WF_DIR/$wf" ] || { bad "登記表裡的 $wf 不存在了"; continue; }
  IFS='|' read -r -a needles <<< "$(lane_needles "$lane")"
  check_lane "$wf" "$BRANCH" "${needles[@]}"
  case $? in
    0) ok "$wf 在「$BRANCH」上會跑到 core/data/ 那幾步" ;;
    2) bad "$wf 判斷不了（見上面的訊息）—— 判斷不了不可以當成驗過" ;;
    *) bad "$wf 在「$BRANCH」上跑不到 —— core/data/ 改了卻沒有這條車道的證據" ;;
  esac
done

echo
if [ "$FAIL" -ne 0 ]; then
  cat >&2 <<'MSG'
──────────────────────────────────────────────────────────────────────────
 怎麼修（兩條路，挑一條）：
   甲、把這條分支加進紅掉那幾份 workflow 的 `on: push: branches:`，
       慢車道的話**連那個 job 的 if: 也要加**（兩道閘門各自都足以讓它
       整條不跑）。合併回 main 之前記得把分支名拿掉。
   乙、這次不要動 core/data/，**也不要動產生它的那幾支腳本**。它是四端
       共用的執行期資料，改它就是同時改四個產品 —— 在一端驗過只是四分之
       一。產生器（scripts/collect_data.sh 之類）改一行 `page_size`，
       四端的候選窗一起變，而 `git diff -- core/data` 一個字都看不到。
──────────────────────────────────────────────────────────────────────────
MSG
fi
echo "═══ core/data 擴散守門：$PASS 過、$FAIL 失敗 ═══"
[ "$FAIL" -eq 0 ] || exit 1
