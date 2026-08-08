#!/usr/bin/env bash
#
# windows/audit_offline_win.sh — 離線稽核(原始碼層面)
#
# Android 端有 scripts/audit_offline.sh,守的是「整個 app 只有**一個**檔案
# 碰得到網路 API」,並且用 grep 讓那件事不必讀程式碼就驗得出來。
# 這一支是 Windows 端的對應物,而目前的主張更強:
#
#   **windows/ 底下沒有任何一個檔案碰網路 API。**
#
# ── 為什麼要有這支 ────────────────────────────────────────────────
#
# 「這個輸入法不連網」是產品定位,不是實作細節。而它現在是真的,
# 只是因為還沒有人破壞它 —— 而破壞它不會讓任何一個測試變紅。
# 這支腳本就是那個會變紅的東西。
#
# 它與 windows/check_binaries.sh 的網路檢查是**兩層**,兩層都要:
#   · 這一支看原始碼:有沒有人 include 了 winhttp.h、呼叫了 socket()。
#   · check_binaries.sh 看**產物的匯入表**:靜態連結進來的第三方
#     (原始碼裡看不到)一樣會被抓到。
#
# ── 哪天真的要加連網功能 ──────────────────────────────────────────
#
# 這支腳本會紅,而那是對的。那時要做的是把 ALLOW 清單改成**恰好一個檔案**
# (服務進程那一側的 net_gate.cc),並且:
#   · 開關預設關,fail-closed(「不知道」必須等於「關」);
#   · 連網紀錄只記真的發生過的連線 —— 被開關擋下的嘗試**不記**,
#     否則「開關從沒開過 → 紀錄是空的」這句話就不成立了,
#     而那句話正是使用者稽核我們的方式;
#   · 瘦 DLL 那一側**永遠**是零 —— 它住在瀏覽器與提權進程裡。
# 判斷邏輯與紀錄格式已經寫好而且有測試:windows/common/net_policy.h。
#
# 用法:
#   windows/audit_offline_win.sh
#   windows/audit_offline_win.sh --self-check   # 反向測試,必須非零結束
#
set -euo pipefail

log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[error]\033[0m %s\n' "$*" >&2; exit 1; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

SELF_CHECK=0
[ "${1:-}" = "--self-check" ] && SELF_CHECK=1

# 允許碰網路 API 的檔案。**目前是空的。** 見檔頭。
ALLOW=()

# 這一組出現就要解釋。挑的是「拿來開連線」的東西,不是所有含 net 的字。
TOKENS='winhttp|wininet|urlmon|InternetOpen|WinHttpOpen|WSAStartup|socket\(|connect\(|getaddrinfo|gethostbyname|inet_addr|inet_pton|closesocket|URLDownloadToFile|HttpSendRequest|ws2tcpip|winsock|DnsQuery'

# 掃描範圍:windows/ 底下的 .cc / .h。腳本自己與說明文件不算
# (它們刻意會提到這些名字,不然這段註解就寫不出來了)。
scan_dir="${SCRIPT_DIR}"

tmp="$(mktemp -d)"
cleanup() { rm -rf "${tmp}"; }
trap cleanup EXIT

if [ "${SELF_CHECK}" -eq 1 ]; then
  # 反向測試:複製一份原始碼樹,植入一個真的違規,斷言這支腳本抓得到。
  #
  # 這個專案有過「測試是綠的,因為它沒在測」。一支只會印綠字的稽核腳本
  # 比沒有更糟 —— 它讓人以為有人在看。
  log "反向測試:複製 windows/ 並植入一個違規"
  cp -r "${SCRIPT_DIR}" "${tmp}/windows"
  cat >> "${tmp}/windows/common/protocol.cc" <<'PLANT'
// 植入的違規(--self-check 用)
#include <winhttp.h>
void RimePlantedViolation() { WinHttpOpen(nullptr, 0, nullptr, nullptr, 0); }
PLANT
  scan_dir="${tmp}/windows"
fi

# 先把整行註解拿掉。這份檔案的註解裡刻意寫滿了那些名字 ——
# 不剝掉的話這支腳本會永遠是紅的,而永遠是紅的關卡等於沒有關卡。
# ⚠ 行尾註解仍然會被抓到。那是刻意保守:寧可誤報,不可漏報。
found=0
report="${tmp}/hits.txt"
: > "${report}"

while IFS= read -r f; do
  base="$(basename "${f}")"
  skip=0
  for a in ${ALLOW[@]+"${ALLOW[@]}"}; do
    [ "${base}" = "${a}" ] && skip=1
  done
  [ "${skip}" -eq 1 ] && continue
  # 去掉整行註解(// 與 # 開頭),再比對。
  if sed -e 's://.*::' -e 's:^[[:space:]]*\*.*::' "${f}" \
       | grep -nE "${TOKENS}" > "${tmp}/one.txt" 2>/dev/null; then
    found=1
    while IFS= read -r line; do
      printf '%s:%s\n' "${f#${scan_dir}/}" "${line}" >> "${report}"
    done < "${tmp}/one.txt"
  fi
done < <(find "${scan_dir}" -type f \( -name '*.cc' -o -name '*.h' \) | sort)

# 掃到零個檔案卻報「通過」—— 那正是這個專案抓過的失敗模式。
n_files="$(find "${scan_dir}" -type f \( -name '*.cc' -o -name '*.h' \) | wc -l)"
[ "${n_files}" -gt 10 ] || die "只掃到 ${n_files} 個原始檔,不合理 —— 路徑對嗎?"

if [ "${SELF_CHECK}" -eq 1 ]; then
  if [ "${found}" -eq 1 ]; then
    log "反向測試通過:植入的違規被抓到了"
    sed 's/^/    /' "${report}"
    exit 1   # CI 期待非零 —— 與 rime_tests --self-check 同一個約定
  fi
  die "植入了一個真的 WinHttpOpen 呼叫,這支腳本卻沒抓到 —— 它不會紅。"
fi

if [ "${found}" -eq 1 ]; then
  echo "以下檔案碰到了網路 API:" >&2
  sed 's/^/    /' "${report}" >&2
  die "Windows 端目前的主張是「完全不連網」。要改這個主張,請先讀本檔的檔頭。"
fi

log "掃了 ${n_files} 個原始檔,沒有任何一個碰網路 API ✓"
log "(另一層在 windows/check_binaries.sh:它看的是產物的匯入表)"
