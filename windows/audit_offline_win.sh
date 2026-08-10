#!/usr/bin/env bash
#
# windows/audit_offline_win.sh — 離線稽核(原始碼層面)
#
# Android 端有 scripts/audit_offline.sh,守的是「整個 app 只有**一個**檔案
# 碰得到網路 API」,並且用 grep 讓那件事不必讀程式碼就驗得出來。
# 這一支是 Windows 端的對應物,而現在的主張是:
#
#   **windows/ 底下只有 service/net_gate.cc 一個檔案碰得到網路 API。**
#
# (在它存在之前,主張是「一個都沒有」。改成現在這句話的那一輪,同時補上了
#  開關、fail-closed 與連網紀錄 —— 見下面「允許清單的條件」。)
#
# ── 為什麼要有這支 ────────────────────────────────────────────────
#
# 「這個輸入法離線為預設」是產品定位,不是實作細節。而它現在是真的,
# 只是因為還沒有人破壞它 —— 而破壞它不會讓任何一個測試變紅。
# 這支腳本就是那個會變紅的東西。
#
# 它與 windows/check_binaries.sh 的網路檢查是**兩層**,兩層都要:
#   · 這一支看原始碼:有沒有**第二個**檔案 include 了 winhttp.h、呼叫了 socket()。
#   · check_binaries.sh 看**產物的匯入表**:靜態連結進來的第三方
#     (原始碼裡看不到)一樣會被抓到,而且它守著「瘦 DLL 永遠是零」。
#
# ── 允許清單的條件 ────────────────────────────────────────────────
#
# ALLOW 裡**恰好一個**路徑。要動它之前,那個出口必須同時滿足:
#   · 開關預設關,fail-closed(「不知道」必須等於「關」);
#   · 連網紀錄只記真的發生過的連線 —— 被開關擋下的嘗試**不記**,
#     否則「開關從沒開過 → 紀錄是空的」這句話就不成立,
#     而那句話正是使用者稽核我們的方式;
#   · 瘦 DLL 那一側**永遠**是零 —— 它住在瀏覽器與提權進程裡。
# 判斷邏輯在 windows/common/net_policy.h,流程在 common/net_gate_core.h,
# 兩份都是純函式而且在 Ubuntu 上有單元測試(windows/run_logic_tests.sh)。
#
# ── ⚠ 這支腳本自己踩過的兩個坑 ────────────────────────────────────
#
#   1. **允許清單比對的是路徑,不是檔名。** 用檔名比對的話,任何人在
#      windows/ 底下任何一個目錄新增一份叫 net_gate.cc 的檔案都會被放行,
#      而那正是這條豁免最容易被繞過的方式。--self-check 會植入這一種。
#
#   2. **去註解只去整行註解,不去行尾註解。** 原本用 `sed s://.*::`,
#      它會把一行裡第一個 `//` 之後的東西全部刪掉 —— 於是
#          const char* u = "https://x"; WinHttpOpen(...);
#      在比對之前就變成了 `const char* u = "https:`,那個呼叫消失了。
#      現在只刪「整行都是註解」的行,行尾註解一律保留。
#      寧可誤報,不可漏報。
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

# 允許碰網路 API 的檔案。**相對於 windows/ 的路徑,而且恰好一個。**
# 見檔頭「允許清單的條件」與坑 1。
ALLOW=("service/net_gate.cc")

# 這一組出現就要解釋。挑的是「拿來開連線」的東西,不是所有含 net 的字。
#
# ⚠ 刻意分成兩組,而且只有第一組不分大小寫:
#
#   · TOKENS_CI 是**標頭與函式庫的名字**。Windows 的檔案系統不分大小寫,
#     `#include <WinHttp.h>` 與 `#include <winhttp.h>` 是同一件事 ——
#     大小寫不該是繞過這道關卡的辦法。
#   · TOKENS_CS 是**API 名字**,大小寫就是它的一部分。這一組**不可以**
#     加 -i:`connect\(` 不分大小寫的話會打到 `ShouldAttemptConnect(` 與
#     `IpcClient::Connect()` —— 那是**具名管道**,不是網路,而它出現在
#     十幾處。一條會誤報十幾次的檢查,下一步就是被關掉(§3.1 的教訓)。
TOKENS_CI='winhttp|wininet|urlmon|ws2tcpip|winsock'
TOKENS_CS='InternetOpen|WinHttpOpen|WSAStartup|socket\(|connect\(|getaddrinfo|gethostbyname|inet_addr|inet_pton|closesocket|URLDownloadToFile|HttpSendRequest|DnsQuery'

# 掃描範圍:windows/ 底下的 .cc / .h。腳本自己與說明文件不算
# (它們刻意會提到這些名字,不然這段註解就寫不出來了)。
scan_dir="${SCRIPT_DIR}"

tmp="$(mktemp -d)"
cleanup() { rm -rf "${tmp}"; }
trap cleanup EXIT

if [ "${SELF_CHECK}" -eq 1 ]; then
  # 反向測試:複製一份原始碼樹,植入**兩種**真的違規,斷言兩種都抓得到。
  #
  # 這個專案有過「測試是綠的,因為它沒在測」。一支只會印綠字的稽核腳本
  # 比沒有更糟 —— 它讓人以為有人在看。
  log "反向測試:複製 windows/ 並植入兩個違規"
  cp -r "${SCRIPT_DIR}" "${tmp}/windows"

  # 違規 A:一個普通檔案開始碰網路。
  cat >> "${tmp}/windows/common/protocol.cc" <<'PLANT_A'
// 植入的違規 A(--self-check 用)
#include <winhttp.h>
void RimePlantedViolationA() { WinHttpOpen(nullptr, 0, nullptr, nullptr, 0); }
PLANT_A

  # 違規 B:**檔名與允許清單相同,但路徑不同**。
  # 允許清單若用檔名比對,這一份會被安靜地放行 —— 那是這條豁免最容易
  # 被繞過的方式,所以它必須是反向測試的一部分。
  cat > "${tmp}/windows/common/net_gate.cc" <<'PLANT_B'
// 植入的違規 B(--self-check 用):同名不同路徑,不得被允許清單放行。
#include <winhttp.h>
void RimePlantedViolationB() { WinHttpOpen(nullptr, 0, nullptr, nullptr, 0); }
PLANT_B

  scan_dir="${tmp}/windows"
fi

# ── 掃描範圍先確認非空 ────────────────────────────────────────────
#
# 掃到零個檔案卻報「通過」—— 那正是這個專案抓過的失敗模式。
n_files="$(find "${scan_dir}" -type f \( -name '*.cc' -o -name '*.h' \) | wc -l)"
[ "${n_files}" -gt 10 ] || die "只掃到 ${n_files} 個原始檔,不合理 —— 路徑對嗎?"

# ── 允許清單本身要是活的 ──────────────────────────────────────────
#
# 一條指向不存在的檔案、或指向一個根本不連網的檔案的豁免,是一個
# 沒有人守著的洞:出口哪天搬走了、改名了、或被刪了,這條豁免會留在原地
# 等著放行下一個同名的東西。所以這裡反過來要求它**必須**命中。
for a in ${ALLOW[@]+"${ALLOW[@]}"}; do
  [ -f "${scan_dir}/${a}" ] || \
    die "允許清單裡的 ${a} 不存在 —— 出口搬走了?那條豁免現在是個沒人守的洞。"
  if ! grep -qEi "${TOKENS_CI}" "${scan_dir}/${a}" && \
     ! grep -qE  "${TOKENS_CS}" "${scan_dir}/${a}"; then
    die "允許清單裡的 ${a} 根本沒碰網路 API —— 請把它從 ALLOW 拿掉,
  否則這條豁免只是在等著放行下一個同名的檔案。"
  fi
done

# 只去掉**整行**註解(// 開頭、或區塊註解的 * / /* 續行)。
# 行尾註解刻意保留 —— 見檔頭的坑 2。
strip_full_line_comments() {
  sed -e 's:^[[:space:]]*//.*::' \
      -e 's:^[[:space:]]*\*.*::' \
      -e 's:^[[:space:]]*/\*.*::' "$1"
}

found=0
report="${tmp}/hits.txt"
: > "${report}"

while IFS= read -r f; do
  rel="${f#${scan_dir}/}"
  skip=0
  for a in ${ALLOW[@]+"${ALLOW[@]}"}; do
    # ⚠ 比對**相對路徑**,不是 basename。見檔頭的坑 1。
    [ "${rel}" = "${a}" ] && skip=1
  done
  [ "${skip}" -eq 1 ] && continue
  strip_full_line_comments "${f}" > "${tmp}/stripped.txt"
  # 兩組分開比對(見 TOKENS_CI / TOKENS_CS 的說明)。
  # ⚠ 不要把 grep 接進另一個會提早關 pipe 的東西:pipefail 下那會變成
  #   「命中反而失敗」,而且輸出小的時候完全正常。先落地成檔案再判斷。
  : > "${tmp}/one.txt"
  grep -nEi "${TOKENS_CI}" "${tmp}/stripped.txt" >> "${tmp}/one.txt" || true
  grep -nE  "${TOKENS_CS}" "${tmp}/stripped.txt" >> "${tmp}/one.txt" || true
  if [ -s "${tmp}/one.txt" ]; then
    found=1
    while IFS= read -r line; do
      printf '%s:%s\n' "${rel}" "${line}" >> "${report}"
    done < <(sort -t: -k1,1n -u "${tmp}/one.txt")
  fi
done < <(find "${scan_dir}" -type f \( -name '*.cc' -o -name '*.h' \) | sort)

if [ "${SELF_CHECK}" -eq 1 ]; then
  # 兩種植入都必須被抓到,而且要分別確認 —— 只抓到一種就代表另一條
  # 路仍然是開的,而那正是「綠燈但沒在守」的樣子。
  miss=""
  grep -q '^common/protocol.cc:' "${report}" || miss="${miss} A(普通檔案)"
  grep -q '^common/net_gate.cc:' "${report}" || \
    miss="${miss} B(同名不同路徑 —— 允許清單是用檔名比對的嗎?)"
  if [ -n "${miss}" ]; then
    echo "  抓到的:" >&2
    sed 's/^/    /' "${report}" >&2
    die "植入的違規沒有全部被抓到,漏了:${miss}"
  fi
  log "反向測試通過:兩個植入的違規都被抓到了"
  sed 's/^/    /' "${report}"
  exit 1   # CI 期待非零 —— 與 rime_tests --self-check 同一個約定
fi

if [ "${found}" -eq 1 ]; then
  echo "以下檔案碰到了網路 API:" >&2
  sed 's/^/    /' "${report}" >&2
  die "windows/ 底下只有 ${ALLOW[*]} 准許碰網路 API。要改這個主張,請先讀本檔的檔頭。"
fi

log "掃了 ${n_files} 個原始檔"
log "只有 ${ALLOW[*]} 碰得到網路 API,其餘 $((n_files - 1)) 個都沒有 ✓"
log "(另一層在 windows/check_binaries.sh:它看的是產物的匯入表,"
log "  而且守著「rime_tsf.dll 永遠是零」——那支住在每一個宿主進程裡)"
