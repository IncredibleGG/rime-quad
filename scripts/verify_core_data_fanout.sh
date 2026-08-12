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
    -h|--help)   sed -n '2,45p' "$0"; exit 0 ;;
    *) die "未知參數: $1" ;;
  esac
done

[ -f "$GATE" ] || die "找不到 $GATE"

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

CHANGED="$(git -C "$ROOT" diff --name-only "$BASE" HEAD -- core/data 2>/dev/null)"
# 還沒 commit 的改動也算 —— 本機跑這一支時通常就是為了問「我現在改的東西」。
CHANGED="$CHANGED
$(git -C "$ROOT" status --porcelain -- core/data 2>/dev/null | sed 's/^...//')"
CHANGED="$(printf '%s\n' "$CHANGED" | sed '/^$/d' | sort -u)"

if [ -z "$CHANGED" ]; then
  echo "這一次沒有動到 core/data/（比對 $BASE）—— 這一關沒有話要說。"
  exit 0
fi

log "動到 core/data/ 的檔案（比對 $BASE）"
printf '%s\n' "$CHANGED" | sed 's/^/    /'
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
   乙、這次不要動 core/data/。它是四端共用的執行期資料，改它就是同時
       改四個產品 —— 在一端驗過只是四分之一。
──────────────────────────────────────────────────────────────────────────
MSG
fi
echo "═══ core/data 擴散守門：$PASS 過、$FAIL 失敗 ═══"
[ "$FAIL" -eq 0 ] || exit 1
