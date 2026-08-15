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
# ⚠ -pthread:common/work_queue.cc 起一條真的執行緒。少了它,g++ 連得起來
#   但 std::thread 在執行期直接丟 system_error —— 而那看起來像「測試壞了」。
FLAGS=(-std=c++17 -O1 -g -pthread -Wall -Wextra -Wno-unused-parameter
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
  "${SCRIPT_DIR}/common/ui_accent.cc"
  "${SCRIPT_DIR}/common/ui_layout.cc"
  "${SCRIPT_DIR}/common/statusbar_place.cc"
  "${SCRIPT_DIR}/common/statusbar_layout.cc"
  "${SCRIPT_DIR}/common/ui_strings.cc"
  "${SCRIPT_DIR}/common/service_state.cc"
  "${SCRIPT_DIR}/common/redeploy_flow.cc"
  "${SCRIPT_DIR}/common/status_cells.cc"
  "${SCRIPT_DIR}/common/hotkey_policy.cc"
  "${SCRIPT_DIR}/common/work_queue.cc"
  "${SCRIPT_DIR}/common/shift_tap.cc"
  "${SCRIPT_DIR}/common/bar_owner.cc"
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
  "${SCRIPT_DIR}/tests/test_statusbar_layout.cc"
  "${SCRIPT_DIR}/tests/test_ui_strings.cc"
  "${SCRIPT_DIR}/tests/test_key_deadline.cc"
  "${SCRIPT_DIR}/tests/test_service_state.cc"
  "${SCRIPT_DIR}/tests/test_redeploy_flow.cc"
  "${SCRIPT_DIR}/tests/test_status_cells.cc"
  "${SCRIPT_DIR}/tests/test_hotkey_policy.cc"
  "${SCRIPT_DIR}/tests/test_work_queue.cc"
  "${SCRIPT_DIR}/tests/test_callback_gate.cc"
  "${SCRIPT_DIR}/tests/test_status_line.cc"
  "${SCRIPT_DIR}/tests/test_shift_tap.cc"
  "${SCRIPT_DIR}/tests/test_bar_owner.cc"
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
#
# ⚠ 方向二原本不掃 `tests/`,而那正好是上面第一件事發生的地方 ——
#   一支放在 windows/tests/ 卻**兩份清單都沒有**的 .cc 會完全隱形:
#   方向一只看得到寫在本檔 SRCS 裡的檔案,方向二又跳過那個目錄。
#   test_sha256.cc 那一組當初就是這個形狀,只是它剛好有一半在 SRCS 裡
#   才被抓到。現在補上(掃到的孤兒數:0)。
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
for sub in ('common', 'service', 'tsf', 'setup', 'winshared', 'tests'):
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

# ── #93/#108:按鍵那條路的四條文字判準,以及它們自己的反向測試 ────
#
# service/engine.cc 與 service/pipe_server.cc 在這台 Ubuntu 上**編不起來**
# (要 MSVC),所以這四條只能用文字判準守。判準本身寫成一支函式,
# 為的是能拿**改壞的複本**去問它 —— 這個專案吃過太多次「守門綠著,
# 卻抓不到它宣稱抓的東西」,而一道從來沒有紅過的文字判準就是那種形狀。
#
# 判斷得出來的那一半(兩個上限的關係、逾時那一份的去處)已經搬進
# common/key_deadline.h,由 tests/test_key_deadline.cc 真的跑。
# 這裡守的是**接線**:那個判斷有沒有被接在該接的地方。
#
# ⚠ 用 grep -c 先存進變數再比,不要寫 printf 接 grep -q:
#   在 set -o pipefail 底下**命中會變成失敗**(SIGPIPE 141)——
#   這棵樹被咬過五次。
#
# ⚠ 錨在行首:engine.cc 的**註解裡**寫著那幾行以前長什麼樣
#   (那段說明是這次改動的主要價值之一)。不錨行首的話,守門會被
#   自己的說明文字咬到 —— 而那種紅只會教人把說明刪掉。
key_path_gates() {   # $1 = engine.cc  $2 = pipe_server.cc;回非零 = 有違規
  local en="$1" ps="$2" bad=0 n

  # (1) 按鍵不可以回到無上限的等待。
  n=$(grep -cE '^[[:space:]]*Post\("按鍵"' "${en}" || true)
  if [ "${n}" -ne 0 ]; then
    echo "!! engine.cc 又出現 ${n} 處無上限的 Post(按鍵) —— 那是 queue_.Call(...,0) = 永遠等,而 DLL 那側的上限是 common/key_deadline.h 的 kKeyTimeoutMs" >&2
    bad=1
  fi

  # (2) 上限一定要配作廢權(不然遲到的工作會把同一顆鍵再打進 librime)。
  n=$(grep -c 'queue_.CallAbandonable(' "${en}" || true)
  if [ "${n}" -lt 1 ]; then
    echo "!! engine.cc 的按鍵沒有走 queue_.CallAbandonable() —— 上限與作廢權必須同時在" >&2
    bad=1
  fi

  # (3) 逾時那一條不可以借用 kStDisabled。
  #
  #   kStDisabled 的語意由 common/service_state.cc 定死:「引擎還沒準備好」。
  #   借給「這一顆鍵沒排到」用的代價是那一橫四格整排消失、換成
  #   「正在準備字詞」,而寫進那一格的 atomic 沒有任何一條路清得回來。
  #   ⚠ 錨在 `r.snap`(呼叫端的框):工作**本體**裡那一處
  #   `box->snap.status_flags = kStDisabled;` 是另一件事(引擎真的不認得
  #   這個 session),不在這一條的範圍裡。
  n=$(grep -cE '^[[:space:]]*r\.snap\.status_flags = kStDisabled;' "${en}" || true)
  if [ "${n}" -ne 0 ]; then
    echo "!! engine.cc 有 ${n} 處把逾時的結果標成 kStDisabled —— 那個旗標的意思是「引擎還沒準備好」,不是「這顆鍵沒排到」。借用它會讓一個健康的引擎在狀態列上自稱「正在準備字詞」,而那一格卡著" >&2
    bad=1
  fi

  # (4) 逾時的那一份不可以走進 push_ui。
  #
  #   這是覆核者逐行走過的那條鏈的出口:逾時 → 空快照 → push_ui →
  #   ui_->Hide(),使用者組字到一半候選窗當場被收掉,而引擎那邊組字
  #   原封不動。判準在 common/key_deadline.h 的 DecideKeyUiAction(),
  #   這裡守的是「按鍵那一格真的問過它、而且 push_ui 排在它裡面」。
  n=$(grep -cE '^[[:space:]]*if \(DecideKeyUiAction\(key_timed_out\) == KeyUiAction::kUpdateUi\) \{$' "${ps}" || true)
  if [ "${n}" -ne 1 ]; then
    echo "!! pipe_server.cc 的按鍵那一格沒有(或有 ${n} 處)問過 DecideKeyUiAction() —— 逾時那一份佔位會被當成快照餵進候選窗與那一橫" >&2
    bad=1
  fi
  # 問了還要真的把 push_ui 排在裡面 —— 問完不理它與沒問是同一件事。
  # ⚠ 用 index() 逐字比,不用正規式:這一串裡的括號與大括號在三種
  #   正規式方言裡各有各的跳脫規則,而跳脫寫錯的方向剛好是「永遠不匹配」
  #   = 永遠是綠的。
  n=$(awk 'index($0, "if (DecideKeyUiAction(key_timed_out) == KeyUiAction::kUpdateUi) {") { f = 3; next }
           f > 0 { f--; if (index($0, "push_ui(r.snap);")) hit++ }
           END   { print hit + 0 }' "${ps}")
  if [ "${n}" -lt 1 ]; then
    echo "!! pipe_server.cc 問了 DecideKeyUiAction() 卻沒有把 push_ui(r.snap) 排在它裡面 —— 問完不理它與沒問是同一件事" >&2
    bad=1
  fi

  return "${bad}"
}

echo
echo "==> 按鍵那條路的四條判準(#93/#108)"
key_path_gates "${SCRIPT_DIR}/service/engine.cc" \
               "${SCRIPT_DIR}/service/pipe_server.cc" || exit 1
echo "   上限 + 作廢權都在;逾時那一條沒有借用 kStDisabled,也沒有走進 push_ui"

# ── 反向測試:四條判準每一條都要真的會紅 ─────────────────────────
#
# ⚠ 沒有這一段的話,上面那四條與「echo 一句好聽的話」沒有分別。
#   四種拆法全部是覆核者實際做過、或這一輪實際犯過的那一種。
echo
echo "==> 反向測試(按鍵那四條判準必須抓得到植入的違規)"
KEY_GATE_TMP="$(mktemp -d)"
for plant in unbounded_post no_abandon borrow_disabled ui_unguarded; do
  cp "${SCRIPT_DIR}/service/engine.cc" "${KEY_GATE_TMP}/engine.cc"
  cp "${SCRIPT_DIR}/service/pipe_server.cc" "${KEY_GATE_TMP}/pipe_server.cc"
  case "${plant}" in
    # 換回無上限的等待:一件慢工作 = 整條連線被丟掉。
    unbounded_post)
      printf '  Post("按鍵", [&] { (void)0; });\n' >> "${KEY_GATE_TMP}/engine.cc" ;;
    # 有上限、**沒有**作廢權:遲到的工作把同一顆鍵再打進 librime,
    # 引擎組了字而宿主也打了字 —— 比原本更糟,而且是靜默的。
    no_abandon)
      sed -i 's/queue_.CallAbandonable(/queue_.Call(/' "${KEY_GATE_TMP}/engine.cc" ;;
    # 這一輪自己犯的那一個:逾時借用 kStDisabled。
    borrow_disabled)
      sed -i 's|^\([[:space:]]*\)r.handled = false;$|\1r.handled = false;\n\1r.snap.status_flags = kStDisabled;|' \
        "${KEY_GATE_TMP}/engine.cc" ;;
    # 守門拆掉,push_ui 照走:候選窗在使用者組字到一半時被收掉。
    ui_unguarded)
      sed -i '/if (DecideKeyUiAction(key_timed_out) == KeyUiAction::kUpdateUi) {/d' \
        "${KEY_GATE_TMP}/pipe_server.cc" ;;
  esac
  if key_path_gates "${KEY_GATE_TMP}/engine.cc" \
                    "${KEY_GATE_TMP}/pipe_server.cc" 2>/dev/null; then
    echo "!! 植入 ${plant} 之後那四條判準仍然是綠的 —— 它們不算數" >&2
    exit 1
  fi
  echo "   ok  植入 ${plant} → 變紅"
done
rm -rf "${KEY_GATE_TMP}"

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
