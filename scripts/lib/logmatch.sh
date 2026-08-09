# logmatch.sh — 在 `set -o pipefail` 之下安全地問「這批輸出裡有沒有這一段」
#
# ⚠ `cmd | grep -q P` 在 pipefail 之下是**壞的**:
#   grep -q 一命中就立刻結束,上游還在寫 → SIGPIPE → 上游的退出碼是 141 →
#   整條 pipeline 非 0 → `if` 判成「**沒有**命中」。
#   實測(200000 行的產生器):pipeline rc=141、PIPESTATUS=141 0,
#   `if` 走進 else,而那批輸出裡明明每一行都命中。
#
#   命中得**愈早**愈容易踩到,所以它是機率性的 —— 在小輸出上永遠正常
#   (整批塞得進 64KB 的管線緩衝區),在 `adb logcat -d`、`dumpsys` 這種
#   動輒幾百 KB 到幾 MB 的輸出上才會發作。這使它特別難查:同一支腳本
#   在本機好好的,在 CI 上偶爾「等不到就緒訊號」,看起來像產品沒起來。
#
#   這個專案已經在四支腳本的註解裡各自寫過一次「不可以這樣寫」
#   (audit_offline.sh:487、build_native.sh:402、publish_desktop.sh:110、
#    release_check.sh:214)—— 四次都是撞到之後才寫的。所以收成一份共用的。
#
# 解法:先把輸出收進變數(讀端是 tr/命令替換,會讀完,不會 SIGPIPE),
#       再用 **bash 內建**的比對。內建比對沒有管線,就沒有 SIGPIPE。
#
# 用法:
#   log_has     "phase → READY"  adbs logcat -d -s RimeRuntime:I
#   log_matches "READY|FAILED"   adbs logcat -d
#   兩者都吃「指令 + 參數」,不是吃一段字串 —— 這樣呼叫端不必自己接管線。

# log_has <固定字串> <指令...>
log_has() {
  local needle="$1"; shift
  local out
  out="$("$@" 2>/dev/null | tr -d '\r')" || true
  case "$out" in
    *"$needle"*) return 0 ;;
  esac
  return 1
}

# log_matches <ERE> <指令...>
log_matches() {
  local re="$1"; shift
  local out
  out="$("$@" 2>/dev/null | tr -d '\r')" || true
  # ⚠ $re 不可加引號 —— 加了就變成字面比對,而那會讓所有 ERE 的關卡
  #   安靜地永遠不命中(又一個「該紅的時候不紅」)。
  [[ "$out" =~ $re ]]
}
