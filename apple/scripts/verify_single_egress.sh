#!/usr/bin/env bash
#
# verify_single_egress.sh — 「整個 app 只有一個地方連得上網」是一條斷言,不是一句承諾
#
# 這個專案的定位是離線優先,而 macOS 上我們**沒有辦法**用權限清單證明什麼:
# 一般 app 不需要宣告網路權限就連得上網。所以證明的方式只剩下「你自己查」,
# 而查法必須簡單到不必讀程式碼 —— 一行 grep。
#
# 這一支就是把那一行 grep 變成 CI 的關卡。它自己有反向測試:
# 植入一行假的 URLSession 之後,斷言必須變紅。
#
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC="${ROOT}/apple/LuminaKey"
GATE="Sources/LuminaKeyKit/NetworkGate.swift"

# 任何一個能建立連線的 API。加新的到這裡,不要放寬。
PATTERN='URLSession|NSURLConnection|NWConnection|NWBrowser|CFSocket|CFStream|Network\.framework|getaddrinfo|socket\(|BSD socket'

EXPECT_FAIL=0
[ "${1:-}" = "--expect-fail" ] && EXPECT_FAIL=1

scan() {
  # -I 跳過二進位;只看 .swift。
  grep -rnE "${PATTERN}" "${SRC}" --include='*.swift' 2>/dev/null \
    | grep -v "^${SRC}/${GATE}:" \
    | grep -v '^\s*//' \
    || true
}

OFFENDERS="$(scan | grep -v "//" || true)"

if [ "${EXPECT_FAIL}" -eq 1 ]; then
  # 反向測試:植入一行,斷言它會被抓到。
  VICTIM="${SRC}/AppSources/AppContext.swift"
  cp "${VICTIM}" "${VICTIM}.egressbak"
  # shellcheck disable=SC2016
  printf '\n// 反向測試植入的違規,verify_single_egress.sh 應該抓到它\nlet _sneaky = URLSession.shared\n' >> "${VICTIM}"
  AFTER="$(scan | grep -v '^\s*//' || true)"
  mv "${VICTIM}.egressbak" "${VICTIM}"
  if [ -z "${AFTER}" ]; then
    echo "!! 反向測試失敗:植入了一行 URLSession,掃描卻沒有抓到。" >&2
    echo "   這支腳本本身壞了,它的「通過」沒有意義。" >&2
    exit 1
  fi
  echo "反向測試通過:植入的違規被抓到了 ✓"
  exit 0
fi

if [ -n "${OFFENDERS}" ]; then
  echo "!! 除了 ${GATE} 以外還有地方碰得到網路 —— 離線定位的單一出口破功:" >&2
  printf '%s\n' "${OFFENDERS}" | sed 's/^/    /' >&2
  echo >&2
  echo "   要嘛把它移進 NetworkGate,要嘛說明為什麼它不算出口(並更新本腳本)。" >&2
  exit 1
fi

# 出口本身要在,否則「只有一個出口」在一個沒有出口的專案上也成立。
if ! grep -q 'URLSession' "${SRC}/${GATE}"; then
  echo "!! ${GATE} 裡沒有 URLSession —— 這條斷言在空轉。" >&2
  exit 1
fi

# 預設拒絕。忘了接政策時行為必須是完全離線。
if ! grep -qE 'public static var policy: \(\) -> Bool = \{ false \}' "${SRC}/${GATE}"; then
  echo "!! NetworkGate.policy 的預設值不是 { false } —— fail-closed 破功。" >&2
  exit 1
fi

echo "單一連網出口 ✓(只有 ${GATE},且預設拒絕)"
