#!/usr/bin/env bash
#
# verify_script_readonly.sh — **每一支腳本自己跑不跑得起來**
#
# ── 為什麼需要這一支 ────────────────────────────────────────────────────
# 「腳本本身壞掉」這一類缺陷在三輪裡出現了三次,每一次都是**人眼**發現的:
#
#   第一輪  `verify_ime.sh` 的 `SERIAL` 是空字串 → `adb -s "" shell`
#           在 `set -e` 之下靜默 RC=1,一個字都沒印。
#   第二輪  `emu.sh` 的 port 閘擋在子命令派發**之前** → 連 `--help` 都 exit 2,
#           而 `build_schema_store.sh:118` 因此必死。
#   第三輪  `--serial` 在七支腳本上一律 RC=2;
#           `verify_no_sigpipe_probe.sh` 在 HEAD 上**本來就是紅的**
#           (它把另一支腳本裡的一段字串字面判成違規),而沒有人跑它。
#
# 共同點:**沒有人把這些腳本的最短路徑跑過一次。** 一支跑不起來的守門,
# 與一支通過的守門,在「今天沒有紅字」這個觀察下一模一樣。
#
# 這一支就是那一次:把 `scripts/` 全樹每一支 `.sh` 的**唯讀路徑**跑一遍。
#
# ── 判準 ────────────────────────────────────────────────────────────────
#   可執行的腳本   `--help` 必須 RC=0,而且要印得出東西
#                  (印不出東西的 `--help` 等於沒有)
#   lib/ 的函式庫  不當腳本跑(它們是被 source 的)。改成:`bash -n` 過,
#                  而且 **source 進來不得有副作用、不得非零**
#   兩者共同       唯讀路徑**不准碰外部工具**。`adb` / `git` / `gradle` /
#                  `cmake` / `curl` … 被換成會留下痕跡的 shim;碰到就紅。
#                  (實測:`verify_lua_deferral.sh --help` 從前會開始**編譯**。)
#
# 用法:
#   ./verify_script_readonly.sh
#   ./verify_script_readonly.sh --self-test   # 植入一支壞的,證明這一關會紅
#
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

SELF_TEST=0
TIMEOUT="${RS_HELP_TIMEOUT:-90}"
case "${1:-}" in
  -h|--help)
    sed -n '2,/^set -uo pipefail$/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 0 ;;
  --self-test) SELF_TEST=1 ;;
  "") ;;
  *) echo "未知參數:$1" >&2; exit 2 ;;
esac

# ── 絆線:唯讀路徑碰了外部工具就留下痕跡 ────────────────────────────────
SHIM_DIR="$(mktemp -d)"
TRIPWIRE="$SHIM_DIR/tripped"
trap 'rm -rf "$SHIM_DIR"' EXIT
for tool in adb git gradle gradlew cmake ninja make curl wget rclone zip unzip \
            emulator avdmanager sdkmanager javac java clang clang++ gcc g++ \
            aapt aapt2 apksigner tesseract; do
  cat > "$SHIM_DIR/$tool" <<SHIM
#!/usr/bin/env bash
echo "$tool \$*" >> "$TRIPWIRE"
echo "[readonly-guard] 唯讀路徑不該呼叫 $tool" >&2
exit 127
SHIM
  chmod +x "$SHIM_DIR/$tool"
done

PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); printf '  [PASS] %s\n' "$*"; }
bad() { FAIL=$((FAIL+1)); printf '  [FAIL] %s\n' "$*" >&2; }

# 一支可執行腳本的唯讀路徑。
check_help() {
  local f="$1" rel="${1#$ROOT/}" out rc tripped
  : > "$TRIPWIRE"
  out="$(cd "$ROOT" && PATH="$SHIM_DIR:$PATH" timeout "$TIMEOUT" bash "$f" --help 2>&1)"
  rc=$?
  tripped="$(cat "$TRIPWIRE" 2>/dev/null || true)"
  if [ "$rc" -ne 0 ]; then
    bad "$rel --help → RC=$rc"
    printf '%s\n' "$out" | head -4 | sed 's/^/         /' >&2
    return
  fi
  if [ -z "$(printf '%s' "$out" | tr -d '[:space:]')" ]; then
    bad "$rel --help → RC=0 但**一個字都沒印** —— 那不是說明"
    return
  fi
  if [ -n "$tripped" ]; then
    bad "$rel --help 碰了外部工具(唯讀路徑不該有副作用):$(printf '%s' "$tripped" | head -1)"
    return
  fi
  ok "$rel --help"
}

# 一支被 source 的函式庫。
check_lib() {
  local f="$1" rel="${1#$ROOT/}" out rc tripped
  if ! bash -n "$f" 2>/dev/null; then
    bad "$rel 語法就不過(bash -n)"
    return
  fi
  : > "$TRIPWIRE"
  out="$(cd "$ROOT" && PATH="$SHIM_DIR:$PATH" timeout "$TIMEOUT" \
         bash -c 'set -uo pipefail; . "$1"' _ "$f" 2>&1)"
  rc=$?
  tripped="$(cat "$TRIPWIRE" 2>/dev/null || true)"
  if [ "$rc" -ne 0 ]; then
    bad "$rel source 進來就 RC=$rc(函式庫不得有副作用)"
    printf '%s\n' "$out" | head -4 | sed 's/^/         /' >&2
    return
  fi
  if [ -n "$tripped" ]; then
    bad "$rel source 進來就碰了外部工具:$(printf '%s' "$tripped" | head -1)"
    return
  fi
  ok "$rel(函式庫:語法 ＋ source 無副作用)"
}

scan() {
  local base="$1"
  local f
  while IFS= read -r f; do
    [ -n "$f" ] || continue
    case "${f#$base/}" in
      scripts/lib/*) check_lib "$f" ;;
      *)             check_help "$f" ;;
    esac
  done < <(find "$base/scripts" -name '*.sh' -type f | sort)
}

echo "=== 每一支 scripts/**/*.sh 的唯讀路徑 ==="
if [ "$SELF_TEST" -eq 0 ]; then
  n="$(find "$ROOT/scripts" -name '*.sh' -type f | wc -l)"
  # 掃描範圍不得是空的 —— 「沒有檔案可跑」與「全部跑過」在輸出上一模一樣。
  [ "$n" -ge 20 ] || { echo "!! 只找到 $n 支 .sh —— 這條斷言在空轉。" >&2; exit 2; }
  echo "(共 $n 支)"
  scan "$ROOT"
  echo
  echo "═══ 唯讀路徑:$PASS 通過、$FAIL 失敗 ═══"
  [ "$FAIL" -eq 0 ] || exit 1
  echo "✓ 每一支腳本的說明路徑都跑得起來,而且沒有副作用"
  exit 0
fi

# ── 反向:植入三種壞法,每一種都必須被抓到 ──────────────────────────────
echo "=== --self-test:把三種「腳本自己壞掉」逐一植進暫存副本 ==="
bad_count=0
PLANTS=(
  "help-nonzero|#!/usr/bin/env bash
set -euo pipefail
echo hi
exit 2
"
  "help-silent|#!/usr/bin/env bash
set -euo pipefail
exit 0
"
  "help-side-effect|#!/usr/bin/env bash
set -euo pipefail
adb devices
echo 說明
"
)
for plant in "${PLANTS[@]}"; do
  name="${plant%%|*}"; body="${plant#*|}"
  tmp="$(mktemp -d)"
  mkdir -p "$tmp/scripts"
  printf '%s' "$body" > "$tmp/scripts/_plant.sh"
  PASS=0; FAIL=0
  check_help "$tmp/scripts/_plant.sh" >/dev/null 2>&1
  if [ "$FAIL" -gt 0 ]; then
    echo "  [PASS] 抓到了「$name」"
  else
    echo "  [FAIL] **沒有**抓到「$name」—— 那一條什麼都沒在守" >&2
    bad_count=$((bad_count+1))
  fi
  rm -rf "$tmp"
done
echo
echo "═══ 反向驗證:$bad_count 項失敗 ═══"
exit $([ "$bad_count" -eq 0 ] && echo 0 || echo 1)
