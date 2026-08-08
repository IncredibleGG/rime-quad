#!/usr/bin/env bash
#
# windows/verify_console.sh — 不經 UI 的核心驗證(Windows)
#
# Android 端的對應物是 scripts/run_console_test.sh。做的是同一件事:
# 把「librime + schema 資料 + rime_shell 門面」和「平台 UI」分開驗證。
# 這裡綠而 TSF 打不出字 → 問題必在 TSF;這裡就紅 → 再怎麼改 UI 都沒用。
#
# 用法(Git Bash):
#   windows/verify_console.sh                       # 跑預設的拼音與注音兩組
#   windows/verify_console.sh --keys nihao --schema luna_pinyin_tw --expect 你好
#
set -euo pipefail

log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[error]\033[0m %s\n' "$*" >&2; exit 1; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
ARCH="${ARCH:-x64}"
BUILD_ROOT="${ROOT}/third_party/build/windows-${ARCH}"
CONSOLE="${BUILD_ROOT}/console/bin/rime_console.exe"
WORK="${BUILD_ROOT}/verify"

KEYS=""; SELECT=1; SCHEMA=""; EXPECT=""
while [ $# -gt 0 ]; do
  case "$1" in
    --keys)   KEYS="$2"; shift 2 ;;
    --select) SELECT="$2"; shift 2 ;;
    --schema) SCHEMA="$2"; shift 2 ;;
    --expect) EXPECT="$2"; shift 2 ;;
    -h|--help) sed -n '2,14p' "$0"; exit 0 ;;
    *) die "未知參數: $1" ;;
  esac
done

command -v cygpath >/dev/null 2>&1 || die "必須在 Git Bash / MSYS2 下執行"
w() { cygpath -m "$1"; }

[ -f "${CONSOLE}" ] || die "找不到 ${CONSOLE};先跑 windows/build.sh"
[ -d "${ROOT}/core/data/shared" ] \
  || die "缺少 core/data/shared。先跑 scripts/fetch_rime_data.sh 與 scripts/collect_data.sh"

# 使用者目錄每次重來:第一次會完整編譯詞庫,之後同一份沿用(第二個案例因此很快)。
# 不重來的話,上一輪失敗留下的半成品 build/ 會讓這一輪的結果無法解釋。
rm -rf "${WORK}"
mkdir -p "${WORK}/user" "${WORK}/logs"
cp -r "${ROOT}/core/data/user/." "${WORK}/user/" 2>/dev/null || true

SHARED_W="$(w "${ROOT}/core/data/shared")"
USER_W="$(w "${WORK}/user")"

fail=0

run_case() {
  local keys="$1" sel="$2" schema="$3" expect="$4"
  local tag="${schema:-default}-${keys}"
  local out="${WORK}/logs/${tag}.log"

  echo
  echo "################################################################"
  echo "# 案例: keys=${keys} select=${sel} schema=${schema:-預設} expect=${expect}"
  echo "################################################################"

  local rc=0
  "${CONSOLE}" "${SHARED_W}" "${USER_W}" "${keys}" "${sel}" "${schema}" \
    > "${out}.raw" 2>&1 || rc=$?

  # MSVC 的 CRT 在文字模式下把 \n 寫成 \r\n。留著的話,底下每一個「整行精確比對」
  # 都會因為行尾多一個 CR 而失敗,而錯誤訊息看起來會像「你好 != 你好」。
  tr -d '\r' < "${out}.raw" > "${out}"
  rm -f "${out}.raw"

  # glog 的部署訊息很吵,成功時只印我們自己的輸出。
  grep -v -E '^[WIEF][0-9]{4,8} ' "${out}" || true

  if [ "${rc}" -ne 0 ]; then
    echo "--- 完整輸出 ---"; cat "${out}"
    echo "!! rime_console 以 ${rc} 結束" >&2
    fail=1
    return
  fi

  local bad=0

  # ── 1. keysym 查表 ────────────────────────────────────────────────────
  #
  # 這不是湊數的檢查。core/src/rime_shell.cc 就地重宣告了 librime 私有標頭
  # rime/key_table.h 的兩個符號(RimeGetKeycodeByName / RimeGetKeyName),
  # 靠 C++ mangling 對上同一個定義。MSVC 的 mangling 規則與 Clang 不同,
  # 這是 docs/handoff-windows.md 點名的兩個坑之一。
  #
  # 對不上會是連結錯誤(exe 根本不會產生);但「對上了、卻接到別的東西」
  # 只有實際查一次表才看得出來。所以正查與反查都驗:
  #   BackSpace        -> 0x00FF08 且反查回 "BackSpace"
  #   no_such_key_name -> 0(門面層把 librime 的 XK_VoidSymbol 正規化成 0)
  if grep -qE '^ +BackSpace +-> 0x00FF08 +反查=BackSpace$' "${out}"; then
    echo "  ✓ keysym 正查/反查正確(私有符號的 MSVC mangling 對上了)"
  else
    echo "  !! keysym 查表結果不對。實際輸出:" >&2
    grep -E '^ +\S+ +-> 0x' "${out}" >&2 || echo "     (完全沒有查表輸出)" >&2
    bad=1
  fi
  if grep -qE '^ +no_such_key_name +-> 0x000000 +反查=\(NULL\)$' "${out}"; then
    echo "  ✓ 未知鍵名回傳 0(沒有把 XK_VoidSymbol 洩漏出去)"
  else
    echo "  !! 未知鍵名沒有正規化成 0 —— 前端會把未知的鍵當有效鍵送進引擎" >&2
    bad=1
  fi

  # ── 2. 部署 ───────────────────────────────────────────────────────────
  # 明著要求 SUCCESS。rime_console 在等不到結果時只會印警告然後繼續,
  # 那種「沒部署但後面剛好過了」的狀態不可以被當成綠燈。
  if grep -q '^\[deploy\] SUCCESS$' "${out}"; then
    echo "  ✓ 部署 SUCCESS"
  else
    echo "  !! 沒有收到部署 SUCCESS" >&2
    bad=1
  fi

  # ── 3. 真的打出字 ─────────────────────────────────────────────────────
  # 取最後那一行累計結果(行首無縮排的那個),整串精確比對。
  # 只用 grep -q "你好" 是不夠的:上屏成「你好嗎」一樣會通過。
  local actual
  # `|| true` 不可省:沒有 COMMIT 時 grep 非零結束,配上 pipefail 會讓腳本
  # 在「印出『沒有產生 COMMIT』」之前就死掉 —— 而那正是最需要看到訊息的時候。
  actual="$(grep '^>>> COMMIT: ' "${out}" | tail -1 | sed 's/^>>> COMMIT: "//; s/"$//' || true)"
  if [ -z "${actual}" ]; then
    echo "  !! 沒有產生 COMMIT —— 組字或選字沒走通" >&2
    bad=1
  elif [ "${actual}" != "${expect}" ]; then
    echo "  !! commit 是「${actual}」,預期「${expect}」" >&2
    bad=1
  else
    echo "  ✓ commit == 「${expect}」"
  fi

  if [ "${bad}" -ne 0 ]; then
    echo "--- 完整輸出 (${out}) ---"
    cat "${out}"
    fail=1
  fi
}

log "console : ${CONSOLE}"
log "shared  : ${SHARED_W}"
log "user    : ${USER_W}"

if [ -n "${KEYS}" ]; then
  run_case "${KEYS}" "${SELECT}" "${SCHEMA}" "${EXPECT}"
else
  # 兩組涵蓋兩條不同的詞庫路徑,與 Android 的 run_console_test.sh 完全相同:
  #   拼音走 luna_pinyin.dict;注音走 terra_pinyin.dict。
  # 只測拼音是不夠的 —— 注音方案選字之後仍停留在組字狀態,必須靠 menu.count
  # 走完政策迴圈才會上屏。那條路只有注音案例會踩到。
  run_case nihao  1 luna_pinyin_tw 你好
  run_case su3cl3 1 bopomofo_tw    你好
fi

echo
if [ "${fail}" -ne 0 ]; then
  die "核心驗證失敗。日誌在 ${WORK}/logs/"
fi
log "核心驗證全部通過 ✓"
