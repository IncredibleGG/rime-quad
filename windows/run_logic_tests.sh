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
  "${SCRIPT_DIR}/common/net_gate_core.cc"
  "${SCRIPT_DIR}/common/net_ui.cc"
  "${SCRIPT_DIR}/common/update_manifest.cc"
  "${SCRIPT_DIR}/common/update_flow.cc"
  "${SCRIPT_DIR}/common/schema_list_patch.cc"
  "${SCRIPT_DIR}/common/mini_json.cc"
  "${SCRIPT_DIR}/common/sha256.cc"
  "${SCRIPT_DIR}/common/zip_reader.cc"
  "${SCRIPT_DIR}/common/archive_guard.cc"
  "${SCRIPT_DIR}/common/schema_preflight.cc"
  "${SCRIPT_DIR}/common/store_index.cc"
  "${SCRIPT_DIR}/common/store_engine.cc"
  "${SCRIPT_DIR}/common/elevation_policy.cc"
  "${SCRIPT_DIR}/common/key_eat_policy.cc"
  "${SCRIPT_DIR}/common/profile_choice.cc"
  "${SCRIPT_DIR}/common/ui_palette.cc"
  "${SCRIPT_DIR}/common/ui_layout.cc"
  "${SCRIPT_DIR}/common/statusbar_place.cc"
  "${SCRIPT_DIR}/common/ui_strings.cc"
  "${SCRIPT_DIR}/common/service_state.cc"
  "${SCRIPT_DIR}/common/status_cells.cc"
  "${SCRIPT_DIR}/common/hotkey_policy.cc"
  "${SCRIPT_DIR}/common/bar_visibility.cc"
  "${SCRIPT_DIR}/tests/test_main.cc"
  "${SCRIPT_DIR}/tests/test_protocol.cc"
  "${SCRIPT_DIR}/tests/test_keymap.cc"
  "${SCRIPT_DIR}/tests/test_policy.cc"
  "${SCRIPT_DIR}/tests/test_layout.cc"
  "${SCRIPT_DIR}/tests/test_link_state.cc"
  "${SCRIPT_DIR}/tests/test_schema_choice.cc"
  "${SCRIPT_DIR}/tests/test_settings.cc"
  "${SCRIPT_DIR}/tests/test_net_policy.cc"
  "${SCRIPT_DIR}/tests/test_net_gate_core.cc"
  "${SCRIPT_DIR}/tests/test_net_ui.cc"
  "${SCRIPT_DIR}/tests/test_sha256.cc"
  "${SCRIPT_DIR}/tests/test_update_manifest.cc"
  "${SCRIPT_DIR}/tests/test_update_flow.cc"
  "${SCRIPT_DIR}/tests/test_proto_compat.cc"
  "${SCRIPT_DIR}/tests/test_schema_list_patch.cc"
  "${SCRIPT_DIR}/tests/test_store_basics.cc"
  "${SCRIPT_DIR}/tests/test_zip_reader.cc"
  "${SCRIPT_DIR}/tests/test_archive_guard.cc"
  "${SCRIPT_DIR}/tests/test_schema_preflight.cc"
  "${SCRIPT_DIR}/tests/test_store_index.cc"
  "${SCRIPT_DIR}/tests/test_store_engine.cc"
  "${SCRIPT_DIR}/tests/test_elevation_policy.cc"
  "${SCRIPT_DIR}/tests/test_key_eat_policy.cc"
  "${SCRIPT_DIR}/tests/test_profile_choice.cc"
  "${SCRIPT_DIR}/tests/test_ui_layout.cc"
  "${SCRIPT_DIR}/tests/test_ui_palette.cc"
  "${SCRIPT_DIR}/tests/test_statusbar_place.cc"
  "${SCRIPT_DIR}/tests/test_ui_strings.cc"
  "${SCRIPT_DIR}/tests/test_service_state.cc"
  "${SCRIPT_DIR}/tests/test_status_cells.cc"
  "${SCRIPT_DIR}/tests/test_hotkey_policy.cc"
  "${SCRIPT_DIR}/tests/test_bar_visibility.cc"
)

# ── 這份清單與 windows/CMakeLists.txt 必須對得上 ──────────────────
#
# ⚠ 擋的是這個專案剛剛真的發生過的兩件事,而**兩件都是一路全綠**:
#
#   · tests/test_sha256.cc、test_update_manifest.cc、test_update_flow.cc、
#     test_bar_visibility.cc 在下面這份 SRCS 裡跑,卻不在 CMakeLists 的
#     rime_tests 上 —— Windows CI 那支 rime_tests.exe 一次都沒有執行過
#     它們。線上更新的整個判斷層在 Windows 上等於沒有測試。
#   · service/update_service.cc 根本不在任何目標上。語法檢查是綠的
#     (它逐檔編),CMake 也不會抱怨一個沒人要的檔案 —— 要等到
#     rime_service.exe 連結時才炸成 8 個 LNK2019。
#
# 兩件的共同形狀是「檔案存在 ≠ 有人編它」。這一段用兩個方向擋:
# 這裡跑的每一支 CMakeLists 都要有,而且每一個 .cc 都要有人編。
echo "==> 建置清單對帳(本檔 ↔ windows/CMakeLists.txt)"
python3 - "${SCRIPT_DIR}" <<'PYPARITY'
import os, re, sys
d = sys.argv[1]
sh = open(os.path.join(d, 'run_logic_tests.sh'), encoding='utf-8').read()
cm = open(os.path.join(d, 'CMakeLists.txt'), encoding='utf-8').read()
here = set(re.findall(r'\$\{SCRIPT_DIR\}/((?:tests|common)/[A-Za-z0-9_]+\.cc)', sh))
if not here:
    print('!! 一個原始檔都沒抓到 —— 這一段的比對規則壞了,不當成通過', file=sys.stderr)
    raise SystemExit(2)
bad = []
for f in sorted(here):
    if f not in cm:
        bad.append('CMakeLists.txt 裡沒有 %s —— 它只在 Ubuntu 上跑過' % f)
for sub in ('common', 'service', 'tsf', 'setup', 'winshared'):
    p = os.path.join(d, sub)
    if not os.path.isdir(p):
        continue
    for name in sorted(os.listdir(p)):
        if name.endswith('.cc') and ('%s/%s' % (sub, name)) not in cm:
            bad.append('CMakeLists.txt 裡沒有 %s/%s —— 沒有任何目標編它'
                       % (sub, name))
for b in bad:
    print('!! ' + b, file=sys.stderr)
if bad:
    print('!! 建置清單對帳失敗:%d 項' % len(bad), file=sys.stderr)
    raise SystemExit(1)
print('   %d 個原始檔在兩份清單上都在' % len(here))
PYPARITY

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

# ── 守門腳本自己也要被驗 ────────────────────────────────────────
#
# 這個專案吃虧的形狀一向不是「程式碼寫錯」,是「守門的東西沒有人守」。
# 下面兩支都是 shell,不需要編譯,所以順手接在這裡 —— 開發時每一輪
# 都會跑到,不必等 CI。

echo
echo "==> 離線稽核(原始碼層面:只有 service/net_gate.cc 碰得到網路 API)"
"${SCRIPT_DIR}/audit_offline_win.sh"

echo
echo "==> 反向測試(離線稽核必須抓得到植入的違規)"
if "${SCRIPT_DIR}/audit_offline_win.sh" --self-check; then
  echo "!! 植入了真的 WinHttpOpen 呼叫,稽核腳本卻以 0 結束 —— 它不會紅" >&2
  exit 1
fi
echo "==> 反向測試通過(離線稽核會紅)"

echo
echo "==> 單一來源稽核(同一件事不得在兩個地方各寫一份)"
"${SCRIPT_DIR}/audit_single_source.sh"

echo
echo "==> 反向測試(單一來源稽核必須抓得到植入的違規)"
"${SCRIPT_DIR}/audit_single_source.sh" --self-check

echo
echo "==> check_binaries.sh 的網路允許矩陣"
"${SCRIPT_DIR}/verify_check_binaries.sh"

# ── 安裝程式那兩條純文字判準 ────────────────────────────────────
#
# verify_installer.sh 整支只有 Windows 跑得動(它會真的裝一次),但它裡面
# 兩條**判準本身**是純文字的,在這裡跑得完 —— 而它們守的正是這一輪最
# 容易被改壞、又最不容易被發現的兩件事:
#
#   · 開機佇列:「升級沒有把任何東西排進開機佇列」問的是**整個安裝目錄**,
#     不是只有 rime_tsf.dll 一個檔名。
#   · 更新之後把服務叫回來:不可以用 Exec(會繼承提權權杖),
#     要被 /RESTARTIME 守著,而且那一段不可以消失。
#
# 接在這裡的理由與上面幾支相同:開發時每一輪都會跑到,不必等 CI。
echo
echo "==> 線上更新那幾條規則的反向測試(真的把規則改壞,要求指定的測試變紅)"
"${SCRIPT_DIR}/verify_update_gates.sh"

echo
echo "==> 安裝程式:開機佇列判準的反向測試"
"${SCRIPT_DIR}/verify_installer.sh" --self-check-pending

echo
echo "==> 安裝程式:更新後重啟服務那一段的反向測試"
"${SCRIPT_DIR}/verify_installer.sh" --self-check-restart
