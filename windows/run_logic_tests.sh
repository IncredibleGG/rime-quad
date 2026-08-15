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
  "${SCRIPT_DIR}/common/log_rate.cc"
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
  "${SCRIPT_DIR}/tests/test_log_rate.cc"
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
  local en="$1" ps="$2" bad=0 n out line

  # ── (1)(2)(3) 三條純文字的,留在 bash 裡 ────────────────────────
  #
  # ⚠ 它們**不是**判準的主體(主體在下面那支 python:它從 case Op::kKey
  #   把出口數出來)。留著是因為它們抓得到主體抓不到的形狀:一段被貼在
  #   任何函式外面的 Post("按鍵")。
  n=$(grep -cE '^[[:space:]]*Post\("按鍵"' "${en}" || true)
  if [ "${n}" -ne 0 ]; then
    echo "!! engine.cc 又出現 ${n} 處無上限的 Post(按鍵) —— 那是 queue_.Call(...,0) = 永遠等,而 DLL 那側的上限是 common/key_deadline.h 的 kKeyTimeoutMs" >&2
    bad=1
  fi

  n=$(grep -c 'queue_.CallAbandonable(' "${en}" || true)
  if [ "${n}" -lt 1 ]; then
    echo "!! engine.cc 的按鍵沒有走 queue_.CallAbandonable() —— 上限與作廢權必須同時在" >&2
    bad=1
  fi

  # 逾時那一條不可以借用 kStDisabled。
  #
  #   kStDisabled 的語意由 common/service_state.cc 定死:「引擎還沒準備好」。
  #   借給「這一顆鍵沒排到」用的代價是那一橫四格整排消失、換成
  #   「正在準備字詞」,而它會一直說謊到下一顆成功的按鍵為止 ——
  #   而一顆鍵按下去沒反應之後,人的下一個動作正好是停手。
  #   ⚠ 錨在 `r.snap`(呼叫端的框):工作**本體**裡那一處
  #   `box->snap.status_flags = kStDisabled;` 是另一件事(引擎真的不認得
  #   這個 session),不在這一條的範圍裡。
  n=$(grep -cE '^[[:space:]]*r\.snap\.status_flags = kStDisabled;' "${en}" || true)
  if [ "${n}" -ne 0 ]; then
    echo "!! engine.cc 有 ${n} 處把逾時的結果標成 kStDisabled —— 那個旗標的意思是「引擎還沒準備好」,不是「這顆鍵沒排到」" >&2
    bad=1
  fi

  # ── (4) 以下是主體:從 case Op::kKey **數出口**,不是認字串 ────────
  #
  # ⚠ 上一輪這裡認的是 engine.cc 的 `Post("按鍵")` 與 `CallAbandonable`,
  #   而 `case Op::kKey` 那條路上**另外兩個出口**當時都還是無上限的:
  #     · Ctrl+空白 / 輕點 Shift → Engine::ToggleAsciiMode()
  #     · Ctrl+Shift+F → PipeServer::ToggleVariantPref() → ReadBackStatus()
  #                     → Engine::CurrentResult()
  #   四道判準一條都沒紅,而 commit 標題讓下一個人相信這件事已經關掉了。
  #
  # 所以判準改成:**把出口找出來**。
  #   a. 從 pipe_server.cc 的 `case Op::kKey` 那一格,收集所有 `engine_->X(`;
  #      並且往下一層,收集那一格呼叫到的 PipeServer 成員裡的 `engine_->X(`。
  #
  #      ⚠ **只有一層,而這是一個真的上限,不是「還沒遇到」。** 覆核者
  #        實測過:新增 `PipeServer::OuterRefresh → InnerRefresh →
  #        engine_->SelectCandidate`,再從 `case Op::kKey` 呼叫 OuterRefresh
  #        —— 這道判準**抓不到**(分母仍然是 4),因為 InnerRefresh 不是
  #        `case Op::kKey` 直接叫的,而收集只往下追一層。
  #        所以:**按鍵那條路上不要寫兩層的 helper**。真的需要的話,
  #        這裡要跟著改成遞迴收集(而不是「反正守門是綠的」)。
  #   b. 對每一個 X,去 engine.cc 看它**會不會排進佇列並等**
  #      (Post / queue_.Call / CallKeyBounded)。不會的(StalledMs 那些
  #      純讀數的)自動不在範圍裡 —— 不必維護一份會過期的名單。
  #   c. 會的每一個都必須:呼叫點帶著這顆鍵的預算、而且走 CallKeyBounded。
  out="$(python3 - "${en}" "${ps}" <<'PYKEY'
import re, sys

# ⚠ **註解一定要先剝掉。** 這棵樹的註解裡寫滿了「以前長什麼樣」——
#   那段說明是這幾輪改動的主要價值之一,但判準若讀得到它,守門就會被
#   自己的說明文字咬到,而那種紅只會教人把說明刪掉。(第一版就是這樣:
#   註解裡的 `push_ui(` 讓「恰好一次」數成三次。)
def strip_comments(src):
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == '"' or c == "'":
            q = c
            out.append(c)
            i += 1
            while i < n:
                out.append(src[i])
                if src[i] == '\\':
                    if i + 1 < n:
                        out.append(src[i + 1])
                        i += 2
                        continue
                elif src[i] == q:
                    i += 1
                    break
                i += 1
            continue
        if c == '/' and i + 1 < n and src[i + 1] == '/':
            while i < n and src[i] != '\n':
                i += 1
            continue
        if c == '/' and i + 1 < n and src[i + 1] == '*':
            i += 2
            while i + 1 < n and not (src[i] == '*' and src[i + 1] == '/'):
                i += 1
            i += 2
            continue
        out.append(c)
        i += 1
    return ''.join(out)

en = strip_comments(open(sys.argv[1], encoding='utf-8', errors='replace').read())
ps = strip_comments(open(sys.argv[2], encoding='utf-8', errors='replace').read())

def block_from(src, i):
    b = src.find('{', i)
    if b < 0:
        return ''
    depth = 0
    for j in range(b, len(src)):
        if src[j] == '{':
            depth += 1
        elif src[j] == '}':
            depth -= 1
            if depth == 0:
                return src[b + 1:j]
    return src[b:]

def paren_args(src, i):          # i 指著 '('
    depth = 0
    for j in range(i, len(src)):
        if src[j] == '(':
            depth += 1
        elif src[j] == ')':
            depth -= 1
            if depth == 0:
                return src[i + 1:j]
    return src[i + 1:]

# 定義一律從第 0 欄開始 —— 註解裡提到的函式名不會被當成定義。
def def_body(src, name):
    m = re.search(r'(?m)^[A-Za-z_][^\n]*\bEngine::' + re.escape(name) + r'\s*\(',
                  src)
    return None if m is None else block_from(src, m.start())

def pipe_body(src, name):
    m = re.search(r'(?m)^[A-Za-z_][^\n]*\bPipeServer::' + re.escape(name) +
                  r'\s*\(', src)
    return None if m is None else block_from(src, m.start())

# 「這一支會排進引擎佇列並且等它」。
# ⚠ 判準是行為不是名字:StalledMs / OldestWaitingMs / CurrentJobLabel 只是
#   讀 queue_ 的數字,不入列 —— 它們自動不在範圍裡,不必維護白名單
#   (白名單會過期,而過期的白名單長得跟綠燈一模一樣)。
def enqueues_and_waits(body):
    if re.search(r'(?<![A-Za-z_])Post\s*\(', body):
        return True
    return 'CallKeyBounded(' in body or 'queue_.Call' in body

i = ps.find('case Op::kKey:')
if i < 0:
    print('NOKEYCASE=1')
    raise SystemExit(0)
key_block = block_from(ps, i)

calls = []
CALL = re.compile(r'engine_->(\w+)\s*\(')
def collect(text, where, helper=None, helper_args=None):
    for m in CALL.finditer(text):
        calls.append((m.group(1), paren_args(text, m.end() - 1), where,
                      helper, helper_args))

collect(key_block, 'case Op::kKey')
# 往下一層:那一格呼叫到的 PipeServer 成員(簡繁那條就藏在這裡)。
# ⚠ 連**那個成員自己的呼叫點**也要看:它把預算往下傳沒有用,如果按鍵
#   那一格給它的是一份完整的 kKeyDeadlineMs 而不是「剩下的」。
for name in sorted(set(re.findall(r'PipeServer::(\w+)\s*\(', ps))):
    m = re.search(r'(?<![A-Za-z_>.])' + re.escape(name) + r'\s*\(', key_block)
    if m:
        b = pipe_body(ps, name)
        if b:
            collect(b, 'PipeServer::' + name, name,
                    paren_args(key_block, m.end() - 1))

# 呼叫點要帶著「這顆鍵剩下的預算」。key_budget_left() 是那一格算出來的,
# deadline_ms 是被呼叫的 PipeServer 成員把它往下傳的那個參數。
BUDGET = ('key_budget_left()', 'deadline_ms')

gated = 0
for name, args, where, helper, helper_args in calls:
    body = def_body(en, name)
    if body is None or not enqueues_and_waits(body):
        continue                    # 純讀數的、或不在 engine.cc 裡
    gated += 1
    print('EXIT=%s|%s' % (name, where))
    flat = re.sub(r'\s+', '', args)
    if not any(re.sub(r'\s+', '', t) in flat for t in BUDGET):
        print('NOBUDGET=%s|%s' % (name, where))
    if helper is not None and 'key_budget_left()' not in re.sub(
            r'\s+', '', helper_args or ''):
        print('NOBUDGETHELPER=%s' % helper)
    if 'CallKeyBounded(' not in body:
        print('UNBOUNDED=%s' % name)
    elif (re.search(r'(?<![A-Za-z_])Post\s*\(', body) and
          'deadline_ms > 0' not in body):
        print('ALSOPOSTS=%s' % name)
print('NEXIT=%d' % gated)

# ── (5) 守門那一格 ──────────────────────────────────────────────
gi = key_block.find('DecideKeyUiAction(kw.timed_out, key_result_is_current)')
if gi < 0:
    print('NOGUARD=1')
else:
    ui = key_block.find('KeyUiAction::kUpdateUi', gi)
    if ui < 0 or 'push_ui(' not in block_from(key_block, ui):
        print('GUARDEMPTY=1')

# ⚠ 這一條是覆核者實測出來的:判準「push_ui 在守門裡面」證明不了
#   「push_ui 只發生在守門裡面」。他在守門那個大括號**後面**多加一行
#   push_ui(r.snap),四條判準全綠。所以數出現次數,而且必須恰好 1。
print('NPUSHUI=%d' % len(re.findall(r'push_ui\s*\(', key_block)))

# ── (6) 簡繁那條:那件不可逆的事排在**最後一趟** ────────────────
#
# ⚠ 這一條守的是這一輪自己造出來的迴歸:上一輪的順序是「先切、再取
#   快照」,而 ToggleVariantPref() 回 true 的當下簡繁**已經真的換掉了**。
#   第二趟(這一輪才加上限的 CurrentResult)逾時 → handled 留 false →
#   TSF 把 Ctrl+Shift+F **也**交給宿主:簡繁在背後換了,而 VS Code 的
#   跨檔搜尋同時開了。判斷住在 common/key_deadline.h 的 PlanVariantKey()
#   (兩條硬性要求在那裡是 static_assert),這裡守的是**接線**。
mvb = re.search(r'if\s*\(\s*action\s*==\s*KeyAction::kToggleVariant\s*\)',
                key_block)
if mvb is None:
    print('NOVARIANTBRANCH=1')
else:
    vb = block_from(key_block, mvb.start())
    i_snap = vb.find('CurrentResult(')
    i_tog = vb.find('ToggleVariantPref(')
    if i_snap < 0 or i_tog < 0:
        print('VARIANTTRIPMISSING=1')
    else:
        if i_tog < i_snap:
            print('SIDEEFFECTFIRST=1')
        # 那件不可逆的事必須被 may_toggle 擋著 —— 順序對了但沒有那道門的話,
        # 下一個人把取快照那一趟搬走,順序就又反了。
        i_may = vb.find('may_toggle')
        if i_may < 0 or i_may > i_tog:
            print('TOGGLEUNGUARDED=1')
    m_plan = re.search(r'PlanVariantKey\s*\(', vb)
    if m_plan is None:
        print('NOVARIANTPLAN=1')
    else:
        # ⚠ ② 就在這一格:第一趟跑完了**不等於**那一份是現況。引擎在
        #   Find(id) 失敗時回一份 handled=false 的全 0 快照,而 CallKeyBounded
        #   照樣回 true。答案已經在 r.handled 裡,呼叫端不可以丟掉它。
        first_args = re.sub(r'\s+', '', paren_args(vb, m_plan.end() - 1))
        if 'r.handled' not in first_args:
            print('NOSESSIONCHECK=1')
    # handled 與「碰不碰 UI」都要從那份計畫來,不可以自己寫死。
    m_h = re.search(r'r\.handled\s*=\s*true', vb)
    i_eat = vb.find('vp.eat_key')
    if m_h is not None and (i_eat < 0 or i_eat > m_h.start()):
        print('HANDLEDNOTPLANNED=1')
    if not re.search(r'key_result_is_current\s*=\s*vp\.ui_is_current', vb):
        print('UINOTPLANNED=1')
PYKEY
)"

  while IFS= read -r line; do
    case "${line}" in
      NOKEYCASE=*)
        echo "!! pipe_server.cc 裡找不到 case Op::kKey —— 掃描範圍錯了" >&2
        bad=1 ;;
      NOBUDGET=*)
        echo "!! 按鍵那條路上的 ${line#NOBUDGET=} 沒有帶著這顆鍵剩下的預算 —— 一顆鍵在服務端只有一份預算(common/key_deadline.h 的 RemainingKeyBudgetMs),兩趟各給一份的話最壞 200ms,而 DLL 只有 150ms:服務端不再『先放棄』" >&2
        bad=1 ;;
      UNBOUNDED=*)
        echo "!! 按鍵那條路上的 Engine::${line#UNBOUNDED=} 沒有走 CallKeyBounded() —— 它會排進引擎那條唯一的 FIFO 並且**永遠等**,而 DLL 150ms 就 Fail() → Close():整條連線被丟掉" >&2
        bad=1 ;;
      ALSOPOSTS=*)
        echo "!! Engine::${line#ALSOPOSTS=} 裡還留著一條無上限的 Post() —— 有上限的路旁邊放一條沒上限的路,等於沒有上限" >&2
        bad=1 ;;
      NOGUARD=*)
        echo "!! pipe_server.cc 的按鍵那一格沒有問過 DecideKeyUiAction(kw.timed_out, key_result_is_current) —— 逾時的佔位、以及『什麼都沒做』那一份全 0 的快照,都會被當成現況餵進候選窗與那一橫" >&2
        bad=1 ;;
      NOBUDGETHELPER=*)
        echo "!! 按鍵那一格呼叫 PipeServer::${line#NOBUDGETHELPER=}() 時沒有給它 key_budget_left() —— 它裡面會進引擎佇列,而給一份完整的預算等於這顆鍵在服務端拿了兩份:最壞 200ms,而 DLL 只有 150ms" >&2
        bad=1 ;;
      GUARDEMPTY=*)
        echo "!! pipe_server.cc 問了 DecideKeyUiAction() 卻沒有把 push_ui 排在它裡面 —— 問完不理它與沒問是同一件事" >&2
        bad=1 ;;
      NOVARIANTBRANCH=*)
        echo "!! pipe_server.cc 的按鍵那一格找不到 KeyAction::kToggleVariant 的分支 —— 掃描範圍錯了" >&2
        bad=1 ;;
      VARIANTTRIPMISSING=*)
        echo "!! 簡繁那一格少了兩趟裡的一趟(CurrentResult / ToggleVariantPref)—— 掃描範圍錯了,或那條路被改成別的形狀" >&2
        bad=1 ;;
      SIDEEFFECTFIRST=*)
        echo "!! 簡繁那一格把**有副作用**的 ToggleVariantPref() 排在取快照之前 —— 它回 true 的當下簡繁已經真的換掉了(SetVariantPref → WM_RIME_SET_VARIANT → store_->Save() + ApplyVariantAll),而取快照那一趟逾時會讓 handled 留 false,於是 TSF 把 Ctrl+Shift+F **也**交給宿主:使用者按一下同時得到「簡繁換了」與「VS Code 的跨檔搜尋開了」。那件不可逆的事要排在最後一趟(common/key_deadline.h 的 PlanVariantKey)" >&2
        bad=1 ;;
      TOGGLEUNGUARDED=*)
        echo "!! 簡繁那一格呼叫 ToggleVariantPref() 之前沒有問過 PlanVariantKey() 的 may_toggle —— 順序這一次是對的,而沒有那道門的話下一個人把取快照那一趟搬走,順序就又反了,而每一條判準都還是綠的" >&2
        bad=1 ;;
      NOVARIANTPLAN=*)
        echo "!! 簡繁那一格沒有走 common/key_deadline.h 的 PlanVariantKey() —— 兩條硬性要求(副作用發生了那顆鍵就一定要算被吃掉;算被吃掉就一定要拿得出現況)的 static_assert 在那支函式上,繞過它等於那兩條沒有人守" >&2
        bad=1 ;;
      NOSESSIONCHECK=*)
        echo "!! 簡繁那一格判斷「第一趟是不是現況」時沒有讀 r.handled —— Engine::CurrentResult 在 Find(id) 失敗時工作**跑完了**卻回一份 handled=false 的全 0 Result,而 CallKeyBounded 照樣回 true。只看 kw.timed_out 的話那一份會被當成現況:候選窗被收掉、那一橫把中/英寫成「中」,而 DLL 會把空快照套進文件,使用者打到一半的組字當場消失而且沒有上屏。⚠ 這不是競態:重新部署後重建失敗的那個宿主,它的 id **永久**不在 sessions_ 裡" >&2
        bad=1 ;;
      HANDLEDNOTPLANNED=*)
        echo "!! 簡繁那一格自己把 r.handled 寫成 true,沒有先問過 PlanVariantKey() 的 eat_key —— 那正是「什麼都沒做卻宣稱吃掉了」的形狀" >&2
        bad=1 ;;
      UINOTPLANNED=*)
        echo "!! 簡繁那一格的 key_result_is_current 不是從 PlanVariantKey() 的 ui_is_current 來的 —— 那一格決定要不要把快照餵進候選窗與那一橫,自己寫死等於繞過判斷" >&2
        bad=1 ;;
    esac
  done <<EOF
${out}
EOF

  # ⚠ 分母:按鍵那條路上會進佇列的出口至少四個(ProcessKey、
  #   ToggleAsciiMode、ReadBackStatus、CurrentResult)。掃不到就是範圍
  #   寫錯,而那必須是紅的 —— 一道掃到 0 個出口的判準永遠是綠的。
  n=$(printf '%s\n' "${out}" | grep '^NEXIT=' | cut -d= -f2 || true)
  n="${n:-0}"
  if [ "${n}" -lt 4 ]; then
    echo "!! 按鍵那條路上只掃到 ${n} 個會進佇列的出口(至少要 4)—— 掃描範圍錯了" >&2
    bad=1
  fi

  n=$(printf '%s\n' "${out}" | grep '^NPUSHUI=' | cut -d= -f2 || true)
  n="${n:-0}"
  if [ "${n}" -ne 1 ]; then
    echo "!! 按鍵那一格裡 push_ui( 出現 ${n} 次(必須恰好 1)—— 守門外面多一行 push_ui,前面每一條判準都還是綠的,而使用者組字到一半的候選窗照樣被收掉" >&2
    bad=1
  fi

  return "${bad}"
}

echo
echo "==> 按鍵那條路的判準(#93/#108)"
key_path_gates "${SCRIPT_DIR}/service/engine.cc" \
               "${SCRIPT_DIR}/service/pipe_server.cc" || exit 1
echo "   三個出口都有上限 + 作廢權、都吃同一份預算;逾時與「什麼都沒做」都不碰 UI,而 push_ui 恰好一處;簡繁那條的副作用排在最後一趟,而且『是不是現況』讀的是 r.handled 不是只有 kw.timed_out"

# ── 反向測試:每一條判準都要真的會紅 ───────────────────────────────
#
# ⚠ 沒有這一段的話,上面那些與「echo 一句好聽的話」沒有分別。
#   每一種拆法都是覆核者實際做過、或這幾輪實際犯過的那一種。
echo
echo "==> 反向測試(按鍵那些判準必須抓得到植入的違規)"
KEY_GATE_TMP="$(mktemp -d)"
for plant in unbounded_post no_abandon borrow_disabled ui_unguarded \
             toggle_ascii_unbudgeted variant_unbudgeted \
             toggle_ascii_reverted push_ui_outside_guard ui_always_current \
             variant_side_effect_first variant_ignores_session \
             variant_toggle_unguarded variant_handled_forced; do
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
    # 上一輪自己犯的那一個:逾時借用 kStDisabled。
    borrow_disabled)
      sed -i 's|^\([[:space:]]*\)r.handled = false;$|\1r.handled = false;\n\1r.snap.status_flags = kStDisabled;|' \
        "${KEY_GATE_TMP}/engine.cc" ;;
    # 守門拆掉,push_ui 照走:候選窗在使用者組字到一半時被收掉。
    ui_unguarded)
      sed -i 's/DecideKeyUiAction(kw.timed_out, key_result_is_current)/KeyUiAction::kUpdateUi/' \
        "${KEY_GATE_TMP}/pipe_server.cc" ;;
    # ── 這一輪補的四種,前兩種正是上一輪漏掉的那兩個出口 ────────────
    # Ctrl+空白 不吃這顆鍵的預算:它自己拿一份完整的 100ms,
    # 加上前面那趟就超過 DLL 的 150ms。
    toggle_ascii_unbudgeted)
      sed -i 's/ToggleAsciiMode(k.session, key_budget_left(), &kw)/ToggleAsciiMode(k.session, kKeyDeadlineMs, \&kw)/' \
        "${KEY_GATE_TMP}/pipe_server.cc" ;;
    # 簡繁那條(兩趟)不吃預算。
    variant_unbudgeted)
      sed -i 's/ToggleVariantPref(key_budget_left(), &kw)/ToggleVariantPref(kKeyDeadlineMs, \&kw)/' \
        "${KEY_GATE_TMP}/pipe_server.cc" ;;
    # Ctrl+空白 換回無上限的 Post —— 上一輪 main 上的樣子。
    toggle_ascii_reverted)
      python3 - "${KEY_GATE_TMP}/engine.cc" <<'PYREVERT'
import io, re, sys
p = sys.argv[1]
s = io.open(p, encoding='utf-8').read()
# 把「切中英後取快照」那一趟換回 Engine::Post(=永遠等)。
s = s.replace('''  auto box = std::make_shared<Snapshot>();
  if (!CallKeyBounded(
          "切中英後取快照", [this, id, box] { *box = TakeSnapshot(id); },
          deadline_ms, wait)) {''',
              '''  auto box = std::make_shared<Snapshot>();
  Post("切中英後取快照", [this, id, box] { *box = TakeSnapshot(id); });
  if (false) {''')
io.open(p, 'w', encoding='utf-8').write(s)
PYREVERT
      ;;
    # ⚠ 覆核者實測過的那一種:守門照樣在,只是**後面**多一行 push_ui。
    #   在加上「恰好一次」之前,這一種是全綠的。
    push_ui_outside_guard)
      python3 - "${KEY_GATE_TMP}/pipe_server.cc" <<'PYEXTRA'
import io, sys
p = sys.argv[1]
s = io.open(p, encoding='utf-8').read()
old = '''          note_schema(r.snap);
          push_ui(r.snap);
        }
'''
new = '''          note_schema(r.snap);
          push_ui(r.snap);
        }
        push_ui(r.snap);
'''
assert s.count(old) == 1, s.count(old)
io.open(p, 'w', encoding='utf-8').write(s.replace(old, new, 1))
PYEXTRA
      ;;
    # 「什麼都沒做」那一格照樣更新 UI(② 在 main 上的樣子)。
    ui_always_current)
      sed -i 's/DecideKeyUiAction(kw.timed_out, key_result_is_current)/DecideKeyUiAction(kw.timed_out, true)/' \
        "${KEY_GATE_TMP}/pipe_server.cc" ;;
    # ── ① 這一輪自己造出來的那個迴歸,以及它的三種鄰居 ─────────────
    # 換回「先切、再取快照」:引擎切了,而逾時讓宿主的 Ctrl+Shift+F 也開了。
    variant_side_effect_first)
      python3 - "${KEY_GATE_TMP}/pipe_server.cc" <<'PYSEFIRST'
import io, sys
p = sys.argv[1]
s = io.open(p, encoding='utf-8').read()
old = """          r = engine_->CurrentResult(k.session, key_budget_left(), &kw);
          VariantKeyPlan vp =
              PlanVariantKey(!kw.timed_out && r.handled, /*toggled=*/false);
          if (vp.may_toggle) {
            vp = PlanVariantKey(true, ToggleVariantPref(key_budget_left(), &kw));
          }
"""
new = """          VariantKeyPlan vp = PlanVariantKey(false, false);
          if (ToggleVariantPref(key_budget_left(), &kw)) {
            r = engine_->CurrentResult(k.session, key_budget_left(), &kw);
            vp = PlanVariantKey(!kw.timed_out && r.handled, true);
          }
"""
assert s.count(old) == 1, s.count(old)
io.open(p, 'w', encoding='utf-8').write(s.replace(old, new, 1))
PYSEFIRST
      ;;
    # ② 本體:把引擎已經算好的答案(r.handled)丟掉。
    variant_ignores_session)
      sed -i 's/PlanVariantKey(!kw.timed_out && r.handled,/PlanVariantKey(!kw.timed_out,/' \
        "${KEY_GATE_TMP}/pipe_server.cc" ;;
    # 順序對了,但那件不可逆的事沒有被 may_toggle 擋著。
    variant_toggle_unguarded)
      python3 - "${KEY_GATE_TMP}/pipe_server.cc" <<'PYUNGUARD'
import io, sys
p = sys.argv[1]
s = io.open(p, encoding='utf-8').read()
old = """          if (vp.may_toggle) {
            vp = PlanVariantKey(true, ToggleVariantPref(key_budget_left(), &kw));
          }
"""
new = """          vp = PlanVariantKey(true, ToggleVariantPref(key_budget_left(), &kw));
"""
assert s.count(old) == 1, s.count(old)
io.open(p, 'w', encoding='utf-8').write(s.replace(old, new, 1))
PYUNGUARD
      ;;
    # 不問計畫,自己宣稱吃掉了。
    variant_handled_forced)
      sed -i 's/          if (vp.eat_key) {/          if (true) {/' \
        "${KEY_GATE_TMP}/pipe_server.cc" ;;
  esac
  if key_path_gates "${KEY_GATE_TMP}/engine.cc" \
                    "${KEY_GATE_TMP}/pipe_server.cc" 2>/dev/null; then
    echo "!! 植入 ${plant} 之後那些判準仍然是綠的 —— 它們不算數" >&2
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
