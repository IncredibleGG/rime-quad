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

# ⚠ 剝註解一定要剝在**內容**上,不能剝在 `grep -rn` 的輸出上 ——
#   那一行長得是 `路徑:行號:內容`,所以 `^\s*//` 永遠不可能命中,
#   那一層是空的。這支腳本以前就是這樣:真正在過濾的是後面一句
#   `grep -v "//"`,而它丟掉的是**任何含有 // 的行**,包含
#       let x = URLSession.shared   // 只是為了檢查更新
#   這種帶行尾註解的真違規 —— 實測 rc=0 並印出「單一連網出口 ✓」。
#   (對照組:Android scripts/audit_offline.sh 的 strip_comments 有把
#    `路徑:行號:` 前綴算進去;Windows audit_offline_win.sh 用 sed 砍行尾註解。)
#
# 現在改成逐檔 sed 再 grep -n:sed 不會改變行數,所以行號仍然是對的。
#   1. 整行註解(// 或 * 或 /* 開頭)→ 整行清掉。刻意在說明文字裡寫
#      「本檔沒有任何 URLSession」的註解不該讓守門永遠紅。
#   2. 行尾註解 → 只砍註解那一段,**程式碼留著**繼續比對。
#      `://` 不算註解起點,否則字串裡的 https:// 會被誤砍成假陰性。
#
# ⚠ **不可以用 `\|` 寫成一條**。`\(//\|\*\|/\*\)` 是 GNU 的 BRE 擴充,
#   BSD sed(也就是 macOS 上的 sed,而這一支正是跑在 macOS 上)不認得它 ——
#   它會把 `\|` 當成字面的 `|`,於是整行註解**一條都剝不掉**,
#   守門變成永遠紅。這一輪就是這樣被 CI 抓到的(run 31325261953,
#   「區塊註解的續行」被誤判成違規),而在開發用的 Linux 上一切正常。
#   同型的事這個專案已經發生過:`grep -oP '...\K...'` 的詞庫檢查在 BSD 上
#   每次都印「所有詞庫都齊全」而完全沒有檢查(見 docs/coordination.md §5)。
#   所以拆成三條純 POSIX BRE,不用任何擴充。
strip_comments_sed=(
  -e 's,^[[:space:]]*//.*,,'
  -e 's,^[[:space:]]*\*.*,,'
  -e 's,^[[:space:]]*/\*.*,,'
  -e 's,\([^:]\)//.*,\1,'
)

swift_files() {
  find "${SRC}" -name '*.swift' -type f ! -path "${SRC}/${GATE}" 2>/dev/null | sort
}

# 掃描結果 == 違規清單。呼叫端不要再疊任何過濾式:
# 反向測試以前就是疊了**另一條**(而且無效的)過濾,才會測不到真正生效的那一層。
scan() {
  local f
  while IFS= read -r f; do
    [ -n "${f}" ] || continue
    sed "${strip_comments_sed[@]}" "${f}" \
      | grep -nE "${PATTERN}" \
      | sed "s,^,${f}:,"
  done < <(swift_files)
  return 0
}

# 掃描範圍不能是空的 —— 「沒有檔案可掃」跟「掃過都乾淨」在輸出上長得一模一樣。
N_SWIFT="$(swift_files | wc -l | tr -d ' ')"
if [ "${N_SWIFT}" -lt 2 ]; then
  echo "!! ${GATE} 以外只找到 ${N_SWIFT} 個 .swift —— 這條斷言在空轉,掃描範圍壞了。" >&2
  exit 1
fi
if [ ! -f "${SRC}/${GATE}" ]; then
  echo "!! 找不到 ${GATE} —— 出口本身不在,這條斷言沒有意義。" >&2
  exit 1
fi

OFFENDERS="$(scan)"

if [ "${EXPECT_FAIL}" -eq 1 ]; then
  # 反向測試:證明這支腳本在該紅的時候真的會紅,而且在不該紅的時候不會紅。
  VICTIM="${SRC}/AppSources/AppContext.swift"
  if [ ! -f "${VICTIM}" ]; then
    echo "!! 反向測試找不到植入對象 ${VICTIM}" >&2
    exit 1
  fi
  # 未植入前就已經有違規的話,「植入後有輸出」不能證明任何事。
  if [ -n "${OFFENDERS}" ]; then
    echo "!! 反向測試無法進行:什麼都還沒植入,掃描就已經有違規了。" >&2
    printf '%s\n' "${OFFENDERS}" | sed 's/^/    /' >&2
    exit 1
  fi

  cp "${VICTIM}" "${VICTIM}.egressbak"
  restore() { [ -f "${VICTIM}.egressbak" ] && mv -f "${VICTIM}.egressbak" "${VICTIM}"; }
  trap restore EXIT

  PLANT_FAILS=0
  # $1=說明 $2=植入的那一行 $3=hit|miss
  check_plant() {
    cp -f "${VICTIM}.egressbak" "${VICTIM}"
    printf '\n%s\n' "$2" >> "${VICTIM}"
    local got
    got="$(scan)"
    cp -f "${VICTIM}.egressbak" "${VICTIM}"
    if [ "$3" = "hit" ] && [ -z "${got}" ]; then
      echo "!! 反向測試失敗:植入「$1」之後,掃描沒有抓到它。" >&2
      PLANT_FAILS=$((PLANT_FAILS + 1))
      return
    fi
    if [ "$3" = "miss" ] && [ -n "${got}" ]; then
      echo "!! 反向測試失敗:「$1」不是違規,卻被抓到了(守門會永遠紅):" >&2
      printf '%s\n' "${got}" | sed 's/^/    /' >&2
      PLANT_FAILS=$((PLANT_FAILS + 1))
      return
    fi
    echo "  ✓ $1"
  }

  # ⚠ 第 2 條就是這支腳本以前放行的形狀。少了它,反向測試會在 bug 還在的時候
  #   照樣通過 —— 舊版植入的剛好是一行沒有註解的 URLSession,必然抓得到。
  check_plant "裸的違規"                  'let _sneaky = URLSession.shared' hit
  check_plant "帶行尾註解的違規"          'let _sneaky2 = URLSession.shared   // 只是為了檢查更新' hit
  check_plant "帶行尾註解的違規(無空白)" 'let _sneaky3 = NWConnection.self// 檢查更新' hit
  check_plant "只是提到 token 的整行註解" '// 說明:本檔沒有任何 URLSession' miss
  check_plant "區塊註解的續行"            '  * 說明:也沒有用 NWConnection' miss

  if [ "${PLANT_FAILS}" -ne 0 ]; then
    echo "!! 這支腳本本身壞了,它的「通過」沒有意義(${PLANT_FAILS} 條植入不符預期)。" >&2
    exit 1
  fi
  echo "反向測試通過:植入的違規都被抓到了,註解都沒有被誤判 ✓"
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

echo "單一連網出口 ✓(只有 ${GATE},且預設拒絕,掃過 ${N_SWIFT} 個 .swift)"
