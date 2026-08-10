#!/usr/bin/env bash
#
# windows/syntax_check_mingw.sh — 在 Linux 上對 Windows 專屬程式碼做語法檢查
#
# ⚠ **這不是建置。** 產物是 MSVC 編出來的,這裡只做 `-fsyntax-only`。
#   mingw-w64 的標頭與 Windows SDK 不完全一樣(見 tests/mingw_syntax_shim.h),
#   所以這支腳本綠了**不代表** MSVC 會綠。反過來才是真的:
#   這支腳本紅了,MSVC 幾乎一定也紅。
#
# 為什麼值得有:Windows 端唯一的建置管道是 GitHub Actions。TSF 那一層有
# 兩千行程式碼,一個打錯的成員名要等一輪 CI 才知道。這裡十秒就知道。
# 「推上去看會不會過」不是開發方式。
#
# 需要一份 mingw-w64 的交叉編譯器。沒有 root 也裝得起來:
#
#   mkdir -p ~/mingw-local/debs && cd ~/mingw-local/debs
#   apt-get download gcc-mingw-w64-base binutils-mingw-w64-x86-64 \
#       mingw-w64-common mingw-w64-x86-64-dev \
#       gcc-mingw-w64-x86-64-win32-runtime gcc-mingw-w64-x86-64-win32 \
#       g++-mingw-w64-x86-64-win32
#   cd .. && for d in debs/*.deb; do dpkg -x "$d" root; done
#
# 然後:
#   MINGW=~/mingw-local/root/usr/bin/x86_64-w64-mingw32-g++-win32 \
#     windows/syntax_check_mingw.sh
#
set -euo pipefail

log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[error]\033[0m %s\n' "$*" >&2; exit 1; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

MINGW="${MINGW:-}"
if [ -z "${MINGW}" ]; then
  for cand in x86_64-w64-mingw32-g++-win32 x86_64-w64-mingw32-g++ \
              "${HOME}/mingw-local/root/usr/bin/x86_64-w64-mingw32-g++-win32"; do
    if command -v "${cand}" >/dev/null 2>&1 || [ -x "${cand}" ]; then
      MINGW="${cand}"
      break
    fi
  done
fi
# 找不到就明確地失敗,不要安靜地「通過」。
# 這個專案抓過太多次「測試是綠的,因為它沒在跑」。
[ -n "${MINGW}" ] || die "找不到 mingw-w64 的 g++。設 MINGW=<路徑>,或照檔頭的說明取得一份。"
log "編譯器: ${MINGW}"
"${MINGW}" --version | head -1

SHIM="${SCRIPT_DIR}/tests/mingw_syntax_shim.h"
[ -f "${SHIM}" ] || die "找不到 ${SHIM}"

# 不帶 -municode:那是給 wmain 用的,而服務進程刻意用 main
# (見 service/main.cc 檔頭 —— glog 的 __argv)。
FLAGS=(-std=c++17 -fsyntax-only -DUNICODE -D_UNICODE
       -DWIN32_LEAN_AND_MEAN -Wall -Wextra -Wno-unused-parameter
       -include "${SHIM}"
       -I"${SCRIPT_DIR}/common" -I"${ROOT}/core/include")

SRCS=(
  "${SCRIPT_DIR}"/tsf/*.cc
  "${SCRIPT_DIR}"/winshared/*.cc
  "${SCRIPT_DIR}"/service/*.cc
  "${SCRIPT_DIR}"/setup/*.cc
  "${SCRIPT_DIR}/tests/probe_main.cc"
  "${SCRIPT_DIR}/tests/test_win32_layouts.cc"
  "${SCRIPT_DIR}/tests/test_win32_listview.cc"
  "${SCRIPT_DIR}/tests/tsf_host_main.cc"
)

# ── 檢查不了的檔案 ───────────────────────────────────────────────
#
# mingw-w64 的 ctfutb.h 是**不完整的**:它有 ITfLangBarItem / ITfLangBarItemMgr,
# 但沒有 ITfLangBarItemButton、TfLBIClick、ITfMenu、TF_LANGBARITEMINFO。
# 真正的 Windows SDK 有。所以 tsf/lang_bar.cc 在這支腳本裡檢查不了。
#
# ⚠ 跳過一定要**大聲**,而且要會自己過期。這個專案抓過「測試是綠的,
#   因為它沒在測」;一個安靜的跳過清單就是那件事的溫床。
#   所以下面先編一個探針:如果哪天 mingw 補上了那個介面,探針會編得過,
#   而這支腳本會**失敗並叫人把跳過拿掉**。
SKIP=("${SCRIPT_DIR}/tsf/lang_bar.cc")

probe="$(mktemp /tmp/rimewin-langbar-probe.XXXXXX.cc)"
cat > "${probe}" <<'PROBE'
#include <windows.h>
#include <msctf.h>
#include <ctfutb.h>
ITfLangBarItemButton* g_probe = nullptr;
PROBE
if "${MINGW}" -std=c++17 -fsyntax-only -DUNICODE -D_UNICODE \
     -DWIN32_LEAN_AND_MEAN "${probe}" >/dev/null 2>&1; then
  rm -f "${probe}"
  die "mingw 的 ctfutb.h 現在有 ITfLangBarItemButton 了 ——
  請把這支腳本裡的 SKIP 清單清空,讓 tsf/lang_bar.cc 也被檢查。"
fi
rm -f "${probe}"

fail=0
checked=0
skipped=0
for f in "${SRCS[@]}"; do
  [ -f "${f}" ] || continue
  skip=0
  for sk in "${SKIP[@]}"; do
    [ "${f}" = "${sk}" ] && skip=1
  done
  if [ "${skip}" -eq 1 ]; then
    skipped=$((skipped + 1))
    printf '  SKIP %s(mingw 的 ctfutb.h 缺 ITfLangBarItemButton;只有 MSVC 檢查得到)\n' \
      "${f#${ROOT}/}"
    continue
  fi
  checked=$((checked + 1))
  if "${MINGW}" "${FLAGS[@]}" "${f}"; then
    printf '  ok   %s\n' "${f#${ROOT}/}"
  else
    printf '  FAIL %s\n' "${f#${ROOT}/}" >&2
    fail=1
  fi
done

# 一個檔案都沒檢查,卻報「通過」——那正是這個專案抓過的失敗模式。
[ "${checked}" -gt 0 ] || die "一個原始檔都沒檢查到"
if [ "${skipped}" -gt 0 ]; then
  printf '\033[1;33m[warn]\033[0m 跳過了 %d 個檔案 —— 它們只有 CI 上的 MSVC 檢查得到。\n' \
    "${skipped}"
fi
log "檢查了 ${checked} 個檔案"
[ "${fail}" -eq 0 ] || die "語法檢查失敗,見上。"
log "語法檢查通過 ✓(提醒:這不等於 MSVC 會過)"
