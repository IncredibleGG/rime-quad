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
)

fail=0
checked=0
for f in "${SRCS[@]}"; do
  [ -f "${f}" ] || continue
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
log "檢查了 ${checked} 個檔案"
[ "${fail}" -eq 0 ] || die "語法檢查失敗,見上。"
log "語法檢查通過 ✓(提醒:這不等於 MSVC 會過)"
