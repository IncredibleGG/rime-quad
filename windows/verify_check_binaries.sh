#!/usr/bin/env bash
#
# windows/verify_check_binaries.sh — 驗 check_binaries.sh 的網路允許矩陣
#
# ── 為什麼需要這一支 ──────────────────────────────────────────────
#
# windows/check_binaries.sh 是「誰有網路能力」這件事**從外面**驗得到的
# 那一層(它讀產物的匯入表)。但它自己只有 Windows 上跑得起來:要 cygpath,
# 要 dumpbin。於是「我改的那段判斷到底對不對」在推 CI 之前是驗不到的,
# 而那正是這個專案反覆吃虧的形狀 —— 守門腳本自己沒有人守。
#
# 這一支把 cygpath 與 dumpbin 換成假的,餵進各種相依清單,斷言**該紅的紅、
# 該綠的綠**。它驗的是**真的那支腳本**,不是它的複製品。
#
# 矩陣的三句話(與 check_binaries.sh 的 NET_DLLS 那一段同一份):
#   1. rime_tsf.dll      網路 DLL 一個都不准,連 ws2_32 也不行。
#   2. rime_service.exe  只准 winhttp.dll(我們的出口)+ ws2_32.dll(librime)。
#   3. rime_ime_setup.exe 只准 ws2_32.dll。
#
# ⚠ 這一支**沒有**接進 .github/workflows/windows.yml。理由:CI 的 Windows
#   runner 上有真的 dumpbin,而這裡要用假的蓋掉它 —— 為了驗一支守門腳本
#   而在 CI 上動 PATH,風險大於收益(而且真正的 check_binaries.sh 本來就會
#   在那裡拿真的產物跑一次)。它接在 windows/run_logic_tests.sh 的尾巴,
#   也就是開發時每一輪都會跑到的地方。
#
# 用法:
#   windows/verify_check_binaries.sh            # 用同目錄的 check_binaries.sh
#   windows/verify_check_binaries.sh <路徑>     # 指定一支
# 驗的是真的那支腳本,不是它的複製品。
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCRIPT="${1:-${SCRIPT_DIR}/check_binaries.sh}"
[ -f "${SCRIPT}" ] || { echo "找不到 ${SCRIPT}" >&2; exit 1; }
WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

mkdir -p "${WORK}/bin" "${WORK}/fakebin"
touch "${WORK}/bin/rime_tsf.dll" "${WORK}/bin/rime_service.exe" \
      "${WORK}/bin/rime_ime_setup.exe"

cat > "${WORK}/fakebin/cygpath" <<'EOF'
#!/usr/bin/env bash
# -u / -w / -p 一律原樣回傳:這裡不需要真的轉路徑。
while [ $# -gt 1 ]; do shift; done
printf '%s\n' "$1"
EOF

cat > "${WORK}/fakebin/dumpbin.exe" <<'EOF'
#!/usr/bin/env bash
mode="$1"; file="$2"
base="$(basename "${file}")"
if [ "${mode}" = "//exports" ]; then
  echo "    ordinal hint RVA      name"
  echo "          1    0 00001000 DllCanUnloadNow"
  echo "          2    1 00001010 DllGetClassObject"
  echo "          3    2 00001020 DllRegisterServer"
  echo "          4    3 00001030 DllUnregisterServer"
  exit 0
fi
echo "  Image has the following dependencies:"
case "${base}" in
  rime_tsf.dll)       echo "    ${DEPS_TSF}" ;;
  rime_service.exe)   echo "    ${DEPS_SERVICE}" ;;
  rime_ime_setup.exe) echo "    ${DEPS_SETUP}" ;;
esac
echo "  Summary"
EOF
chmod +x "${WORK}/fakebin/cygpath" "${WORK}/fakebin/dumpbin.exe"
export PATH="${WORK}/fakebin:${PATH}"

pass=0
fail=0

# expect: ok | red
run_case() {
  local name="$1" expect="$2"
  local out rc
  out="$(bash "${SCRIPT}" "${WORK}/bin" 2>&1)"
  rc=$?
  local got="ok"
  [ "${rc}" -ne 0 ] && got="red"
  if [ "${got}" = "${expect}" ]; then
    printf '  \033[1;32mok\033[0m   %-58s (%s)\n' "${name}" "${got}"
    pass=$((pass + 1))
  else
    printf '  \033[1;31mFAIL\033[0m %-58s 預期 %s,實際 %s\n' "${name}" "${expect}" "${got}"
    echo "${out}" | sed 's/^/       /'
    fail=$((fail + 1))
  fi
}

BASE_TSF="kernel32.dll user32.dll advapi32.dll ole32.dll oleaut32.dll"
BASE_SERVICE="kernel32.dll user32.dll gdi32.dll shell32.dll comctl32.dll"
BASE_SETUP="kernel32.dll user32.dll advapi32.dll ole32.dll"

echo "== 基準:各自帶著該有的東西 =="
export DEPS_TSF="${BASE_TSF}"
export DEPS_SERVICE="${BASE_SERVICE} winhttp.dll ws2_32.dll"
export DEPS_SETUP="${BASE_SETUP} ws2_32.dll"
run_case "服務帶 winhttp + ws2_32;DLL 乾淨" ok

echo
echo "== 瘦 DLL 那一側:永遠是零 =="
export DEPS_TSF="${BASE_TSF} winhttp.dll"
run_case "rime_tsf.dll 長出 winhttp.dll" red
export DEPS_TSF="${BASE_TSF} ws2_32.dll"
run_case "rime_tsf.dll 長出 ws2_32.dll(連這個也不准)" red
export DEPS_TSF="${BASE_TSF}"

echo
echo "== 服務進程:只准宣告過的那兩個 =="
export DEPS_SERVICE="${BASE_SERVICE} winhttp.dll ws2_32.dll wininet.dll"
run_case "服務多長出 wininet.dll" red
export DEPS_SERVICE="${BASE_SERVICE} winhttp.dll ws2_32.dll dnsapi.dll"
run_case "服務多長出 dnsapi.dll" red
export DEPS_SERVICE="${BASE_SERVICE} WinHttp.DLL ws2_32.dll"
run_case "服務的 winhttp 大小寫不同仍算同一個" ok
export DEPS_SERVICE="${BASE_SERVICE} winhttp.dll ws2_32.dll"

echo
echo "== 安裝工具:不連網 =="
export DEPS_SETUP="${BASE_SETUP} ws2_32.dll winhttp.dll"
run_case "rime_ime_setup.exe 長出 winhttp.dll" red
export DEPS_SETUP="${BASE_SETUP} ws2_32.dll"

echo
echo "== 順帶確認舊的兩條沒被我改壞 =="
export DEPS_TSF="${BASE_TSF} vcruntime140.dll"
run_case "rime_tsf.dll 相依動態 CRT(/MT 沒生效)" red
export DEPS_TSF="${BASE_TSF} dwmapi.dll"
run_case "rime_tsf.dll 多一個不在允許清單的 DLL" red
export DEPS_TSF="${BASE_TSF}"

echo
printf '通過 %d,失敗 %d\n' "${pass}" "${fail}"
[ "${fail}" -eq 0 ]
