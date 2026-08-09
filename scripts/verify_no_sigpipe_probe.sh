#!/usr/bin/env bash
#
# verify_no_sigpipe_probe.sh — 禁止 `logcat|dumpsys ... | grep -q`
#
# ── 為什麼需要這一關 ────────────────────────────────────────────────────
#
#   `cmd | grep -q P` 在 `set -o pipefail` 之下,命中時的退出碼可能是 **141**:
#   grep -q 一命中就結束,上游還在寫 → SIGPIPE。於是 `if` 判成「沒有命中」。
#   實測(200000 行的產生器):pipeline rc=141、PIPESTATUS=141 0。
#
#   它是**機率性**的:輸出小的時候整批塞得進 64KB 的管線緩衝區,永遠正常;
#   大到會阻塞、而且命中得早的時候才發作。所以它在本機好好的,在 CI 上
#   偶爾「等不到就緒訊號」—— 看起來像產品沒起來,而不是像關卡自己壞了。
#
#   這個專案已經在四支腳本的註解裡各自寫過一次「不可以這樣寫」
#   (audit_offline.sh、build_native.sh、publish_desktop.sh、release_check.sh),
#   四次都是撞到之後才補的註解。註解攔不住第五次,所以改成一條關卡。
#
# ── 範圍(刻意收窄,而且說出來)──────────────────────────────────────────
#   只擋 `logcat` 與 `dumpsys` 這兩種**大輸出**的產生者。
#   `adb devices` / `pm list packages` / `ime list` 這些輸出只有幾行,
#   整批寫得進管線緩衝區,上游不會被阻塞 → 不會 SIGPIPE,所以不擋。
#   **這是一條有意識的上限,不是漏掉的**:擋得太寬會逼人加一堆例外,
#   而例外清單正是這一類關卡失效的起點。
#
#   正確寫法:source scripts/lib/logmatch.sh,改用 log_has / log_matches。
#
# 用法:
#   ./verify_no_sigpipe_probe.sh
#   ./verify_no_sigpipe_probe.sh --self-test   # 植入一條,證明這一關會紅
#
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SELF_TEST=0
[ "${1:-}" = "--self-test" ] && SELF_TEST=1

# 產生者:輸出大到會讓上游阻塞的那幾種。
PRODUCERS='logcat|dumpsys'
# grep -q / -qF / -qE / -Eq / -Fq … 一律算。
BADPAT="(${PRODUCERS})[^|]*\|([^|]*\|)*[[:space:]]*grep[[:space:]]+-[A-Za-z]*q"

scan_dir() {
  local dir="$1"
  # 掃描範圍不能是空的 —— 「沒有檔案可掃」與「掃過都乾淨」在輸出上一模一樣。
  local files n
  files="$(find "$dir" -maxdepth 1 -name '*.sh' -type f 2>/dev/null | sort)"
  n="$(printf '%s\n' "$files" | grep -c . )"
  if [ "${n:-0}" -lt 5 ]; then
    echo "!! 掃描範圍只有 ${n:-0} 個 .sh($dir)—— 這條斷言在空轉。" >&2
    exit 2
  fi
  local f
  while IFS= read -r f; do
    [ -n "$f" ] || continue
    # 這一支自己不算:它的自我測試會**故意**印出那個錯誤寫法,
    # 而「守門腳本被自己的錯誤示範判紅」是一種很蠢的永遠紅。
    [ "$(basename "$f")" = "$(basename "${BASH_SOURCE[0]}")" ] && continue
    # 只看真的開了 pipefail 的檔案:沒開的話這個坑不存在。
    grep -q 'pipefail' "$f" || continue
    # 註解行不算 —— 這個專案好幾處**刻意**在註解裡寫出錯誤寫法當警告。
    grep -nE "$BADPAT" "$f" | grep -vE '^[0-9]+:[[:space:]]*#' \
      | sed "s|^|${f#$ROOT/}:|"
  done <<< "$files"
}

report() {
  local hits="$1"
  if [ -z "$hits" ]; then return 0; fi
  echo "!! 下面這幾行把 logcat/dumpsys 的輸出接進 grep -q —— pipefail 之下命中會變成 141:" >&2
  printf '%s\n' "$hits" | sed 's/^/    /' >&2
  echo >&2
  echo "   改法:. \"\$SCRIPT_DIR/lib/logmatch.sh\";log_has <字串> <指令...>" >&2
  echo "         或 log_matches <ERE> <指令...>(它們不接管線,所以沒有 SIGPIPE)" >&2
  return 1
}

if [ "$SELF_TEST" -eq 1 ]; then
  TMP="$(mktemp -d)"
  trap 'rm -rf "$TMP"' EXIT
  mkdir -p "$TMP/scripts"
  cp "$ROOT"/scripts/*.sh "$TMP/scripts/" 2>/dev/null
  VICTIM="$TMP/scripts/.sigpipe_plant.sh"
  {
    echo '#!/usr/bin/env bash'
    echo 'set -uo pipefail'
    echo 'if adb logcat -d 2>/dev/null | grep -q "phase → READY"; then :; fi'
  } > "$VICTIM"
  HITS="$(scan_dir "$TMP/scripts")"
  # ⚠ 「有抓到東西」不夠。樹上本來就可能有別的命中,那樣的話這個自我測試
  #   在掃描器壞掉之後照樣會綠。要問的是「**植入的那一行**有沒有被抓到」。
  PLANTED="$(printf '%s\n' "$HITS" | grep -F "$(basename "$VICTIM")" || true)"
  if [ -z "$PLANTED" ]; then
    echo "!! 自我測試失敗:植入了一行 logcat | grep -q,掃描卻沒有抓到它。" >&2
    echo "   這支腳本本身壞了,它的「通過」沒有意義。" >&2
    printf '%s\n' "$HITS" | sed 's/^/    (其他命中)/' >&2
    exit 1
  fi
  echo "自我測試通過:植入的那一行被抓到了 ✓"
  printf '%s\n' "$PLANTED" | sed 's/^/    /'
  exit 0
fi

HITS="$(scan_dir "$ROOT/scripts")"
if ! report "$HITS"; then
  exit 1
fi
echo "沒有 logcat/dumpsys 接 grep -q ✓(就緒判斷不會被 SIGPIPE 判成「沒命中」)"
