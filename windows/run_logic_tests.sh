#!/usr/bin/env bash
#
# windows/run_logic_tests.sh — 在**非 Windows** 的機器上跑純邏輯測試
#
# 為什麼要有這支:Windows 端唯一的建置管道是 GitHub Actions,一輪十幾分鐘,
# 而「推上去看會不會過」不是開發方式。windows/common/ 底下的東西刻意不 include
# windows.h,就是為了能在開發用的 Ubuntu 上用 g++ 編起來、跑起來。
#
# 這裡跑得過**不代表** TSF 能用 —— 它只涵蓋按鍵映射、線路編解碼、組字政策、
# 候選窗排版、連線狀態機。真正的 TSF / COM / 候選窗只有 Windows 上驗得到,
# 而其中一部分只有真人驗得到(見 windows/README.md)。
#
#   windows/run_logic_tests.sh            # 編譯並執行
#   windows/run_logic_tests.sh --asan     # 加上 ASan/UBSan(解碼器的模糊邊界)
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
OUT="${ROOT}/third_party/build/logic-tests"

SAN=0
[ "${1:-}" = "--asan" ] && SAN=1

CXX="${CXX:-g++}"
FLAGS=(-std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter
       -I"${SCRIPT_DIR}/common" -I"${ROOT}/core/include")
if [ "${SAN}" -eq 1 ]; then
  FLAGS+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi

SRCS=(
  "${SCRIPT_DIR}/common/protocol.cc"
  "${SCRIPT_DIR}/common/keymap.cc"
  "${SCRIPT_DIR}/common/ime_policy.cc"
  "${SCRIPT_DIR}/common/cand_layout.cc"
  "${SCRIPT_DIR}/common/schema_choice.cc"
  "${SCRIPT_DIR}/common/settings.cc"
  "${SCRIPT_DIR}/common/net_policy.cc"
  "${SCRIPT_DIR}/common/schema_list_patch.cc"
  "${SCRIPT_DIR}/tests/test_main.cc"
  "${SCRIPT_DIR}/tests/test_protocol.cc"
  "${SCRIPT_DIR}/tests/test_keymap.cc"
  "${SCRIPT_DIR}/tests/test_policy.cc"
  "${SCRIPT_DIR}/tests/test_layout.cc"
  "${SCRIPT_DIR}/tests/test_link_state.cc"
  "${SCRIPT_DIR}/tests/test_schema_choice.cc"
  "${SCRIPT_DIR}/tests/test_settings.cc"
  "${SCRIPT_DIR}/tests/test_net_policy.cc"
  "${SCRIPT_DIR}/tests/test_proto_compat.cc"
  "${SCRIPT_DIR}/tests/test_schema_list_patch.cc"
)

mkdir -p "${OUT}"
echo "==> 編譯 (${CXX})"
"${CXX}" "${FLAGS[@]}" "${SRCS[@]}" -o "${OUT}/rime_tests"

echo "==> 執行"
"${OUT}/rime_tests"

# 反向測試:證明這套框架真的會紅。
# 這個專案有過「測試是綠的,因為它沒在測」,所以不接受只看綠燈。
echo
echo "==> 反向測試(--self-check 必須非零結束)"
if "${OUT}/rime_tests" --self-check; then
  echo "!! --self-check 竟然以 0 結束 —— 測試框架不會紅,上面的綠燈都不算數" >&2
  exit 1
fi
echo "==> 反向測試通過(框架會紅)"
