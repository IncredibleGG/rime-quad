#!/usr/bin/env bash
#
# windows/audit_single_source.sh — 原始碼層面的「只能有一份」稽核
#
# 這個專案吃虧的形狀一向不是「程式碼寫錯」,是**同一件事在兩個地方各寫
# 一份,然後其中一份悄悄漂走**,而漂走的症狀在畫面上完全看不出來:
#
#   · 選項計畫:service/main.cc 的暖機與 pipe_server.cc 的 SESSION_NEW
#     各寫一份,上面掛著一句註解說「這一段必須逐字相同」。它漂了一個
#     `ascii_mode` → TakeSpareSession 的 SameOptions 連長度都不符 →
#     **每一個預熱好的備用 session 都被丟掉**,SESSION_NEW 的 300 毫秒
#     預算保護整個失效。畫面上什麼異狀都沒有,只是第一顆按鍵變慢。
#
#   · rs_select_schema:engine.cc 裡本來有四個裸呼叫點,而 librime 每次
#     載入方案都會跑 ConcreteEngine::InitializeOptions() 把 switches 重設回
#     方案宣告的值。四個裡有三個之後沒有重套簡繁 —— 使用者從那一橫的
#     方案選單換一次方案,他選的簡體就被洗掉,而畫面上那一格還畫著舊的。
#
# 一句註解不是守門。這支才是。
#
# ── 後來長出來的第二種形狀:「判斷有人守,接線沒有」──────────────
#
# 規則 4 / 5 守的不是「寫了兩份」,是**純函式驗不到的那一格**。這個專案
# 一再把判斷抽成 windows/common/ 底下的純函式(Ubuntu 上跑得動),而
# windows/service/ 與 windows/tsf/ 在 Ubuntu 上編不起來 —— 於是:
#
#   · 「有沒有人呼叫那支純函式」沒有人守(規則 3)。
#   · 「呼叫的時候傳進去的是**哪幾份東西**」也沒有人守(規則 5)——
#     規則 3 數的是 token 在不在、誰先誰後,參數對調它一聲都不吭。
#   · 「狀態機有沒有真的接在事件來源上」也沒有人守(規則 4)——
#     tsf/ 只做得到 -fsyntax-only,而把一行呼叫註解掉語法完全正確。
#
# 三個都覆核實跑過:植入之後三支守門全綠。這一支就是那三格。
#
# ⚠ 判準刻意是**原始碼層面**的:windows/service/ 在 Ubuntu 上編不起來
#   (只有 GitHub Actions 的 windows-latest 編得動),所以任何要靠編譯或
#   執行才看得到的守門在開發時都等於不存在。
#
#   windows/audit_single_source.sh              # 稽核
#   windows/audit_single_source.sh --self-check # 反向:植入違規,必須紅
#
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DEFAULT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# 在 $1 這個 repo 根底下跑一次稽核。回傳非零 = 有違規。
audit_root() {
  local root="$1"
  local bad=0
  local n

  # ── 規則 1:選項計畫只能有一份 ────────────────────────────────
  #
  # 建 session 的兩條路徑(暖機、SESSION_NEW)都必須走
  # common/schema_choice.cc 的 BuildOptionPlan,不得自己拼一份。
  n=$(cat "${root}/windows/service/main.cc" \
          "${root}/windows/service/pipe_server.cc" 2>/dev/null \
      | grep -c 'PlanVariant(')
  if [ "${n}" -ne 0 ]; then
    echo "!! main.cc / pipe_server.cc 裡有 ${n} 處直接呼叫 PlanVariant(" >&2
    echo "   選項計畫只能有一份 —— 走 BuildOptionPlan(),見它的註解。" >&2
    bad=1
  fi

  # ── 規則 2:rs_select_schema 只能有一個裸呼叫點 ────────────────
  #
  # 那一個必須在 Engine::SelectAndApply 裡。「載入方案」與「套用簡繁」
  # 是一個不可分割的動作,不能靠人記得 —— 靠人記得的版本已經漏了三次。
  #
  # ⚠ 只數真的呼叫(帶左括號),註解裡提到函式名不算。
  n=$(grep -c 'rs_select_schema(' "${root}/windows/service/engine.cc")
  if [ "${n}" -ne 1 ]; then
    echo "!! engine.cc 裡有 ${n} 處 rs_select_schema( —— 只能有 1 處" >&2
    echo "   那一處必須在 SelectAndApply 裡。換方案會把 switches 重設回" >&2
    echo "   方案宣告的值,之後不重套簡繁就等於把使用者的設定洗掉。" >&2
    bad=1
  fi
  if ! grep -q 'bool Engine::SelectAndApply(' "${root}/windows/service/engine.cc"; then
    echo "!! engine.cc 裡找不到 Engine::SelectAndApply —— 規則 2 沒有東西可守" >&2
    bad=1
  fi

  # ── 規則 3:換方案之前一定要重讀設定檔裡的簡繁偏好 ──────────────
  #
  # 守的是 648c02c ——「換方案洗掉簡繁」。規則 2 守的是「換完方案要重套」,
  # 這一條守的是**重套時拿的是哪一份偏好**:`Engine::variant_pref_` 是設定的
  # 複本,設定檔在服務跑著的時候被別人改掉(設定視窗有一顆「用記事本開啟
  # 設定檔」)它就過期了,而拿過期那一份重套 = 把使用者剛選的簡繁洗掉。
  #
  # ⚠ 為什麼這一條非要在原始碼層面守不可:648c02c 唯一的守門是
  #   windows/verify_installer.sh §6g 案例二,而那支只有 Windows 跑得動。
  #   覆核實跑證明:把 pipe_server.cc 那一行刪掉,三支守門全綠 ——
  #   也就是說那一輪唯一改變使用者看得到的行為的修法,在開發機上
  #   沒有任何東西攔得住它被刪掉。
  #
  #   判斷本身已經抽成 common/schema_choice.cc 的
  #   PickVariantPrefForSchemaSwitch(),tests/test_schema_choice.cc 驗得到;
  #   純函式驗不到的是**有沒有人呼叫它**,那一格就是這一條。
  #
  # ⚠ 只看 kSelectSchema 那一個 case 的區塊,而且**先把註解行濾掉** ——
  #   註解裡提到函式名不算(與規則 2 同一個判準)。
  local blk
  blk="$(awk '{ if (f == 1 && $0 ~ /case Op::/) exit;
                if (f == 1) print;
                if ($0 ~ /case Op::kSelectSchema:/) { f = 1; print } }' \
         "${root}/windows/service/pipe_server.cc" \
       | grep -v '^[[:space:]]*//')"
  if [ -z "${blk}" ]; then
    echo "!! pipe_server.cc 裡找不到 case Op::kSelectSchema —— 規則 3 沒有東西可守" >&2
    bad=1
  else
    local n_pick n_set n_sel
    n_pick=$(printf '%s\n' "${blk}" | grep -n 'PickVariantPrefForSchemaSwitch(' \
             | head -1 | cut -d: -f1)
    n_set=$(printf '%s\n' "${blk}" | grep -n 'engine_->SetVariantPref(' \
            | head -1 | cut -d: -f1)
    n_sel=$(printf '%s\n' "${blk}" | grep -n 'engine_->SelectSchema(' \
            | head -1 | cut -d: -f1)
    if [ -z "${n_pick}" ]; then
      echo "!! pipe_server.cc 的 kSelectSchema 沒有呼叫 PickVariantPrefForSchemaSwitch(" >&2
      echo "   換方案時會拿引擎手上那份**過期的**簡繁偏好去重套 ——" >&2
      echo "   使用者剛在設定檔裡選的簡繁被洗掉,而狀態列那一格還畫著新的。" >&2
      echo "   見 common/schema_choice.h 的 PickVariantPrefForSchemaSwitch。" >&2
      bad=1
    fi
    if [ -z "${n_set}" ]; then
      echo "!! pipe_server.cc 的 kSelectSchema 算了偏好卻沒有 engine_->SetVariantPref(" >&2
      echo "   算出來沒有交給引擎 = 沒算。" >&2
      bad=1
    fi
    if [ -z "${n_sel}" ]; then
      echo "!! pipe_server.cc 的 kSelectSchema 裡沒有 engine_->SelectSchema( —— 規則 3 沒有東西可守" >&2
      bad=1
    fi
    if [ -n "${n_pick}" ] && [ -n "${n_sel}" ] && [ "${n_pick}" -gt "${n_sel}" ]; then
      echo "!! pipe_server.cc 的 kSelectSchema 先換方案才重讀偏好 —— 順序反了" >&2
      echo "   順序就是這一條的全部意義:SelectAndApply 在換方案的**當下**" >&2
      echo "   就會拿 variant_pref_ 重套,晚一步更新等於沒更新。" >&2
      bad=1
    fi
  fi

  # ── 規則 4:輕點 Shift 的狀態機必須真的接在 TSF 的 sink 上 ──────
  #
  # 守的是工單 #89 這一輪自己的新碼。判斷本身(哪一串算一次輕點)是純函式,
  # common/shift_tap.cc + tests/test_shift_tap.cc 有 26 組真值表在守;
  # **接線**沒有任何東西守 —— tsf/ 在 Ubuntu 上只做得到 -fsyntax-only,
  # 而「把一行呼叫註解掉」語法上完全正確。實跑覆核過:
  #
  #   · 三處 shift_tap_.Reset() 全部註解掉 → 三支守門全綠。
  #     症狀:按著 Shift 切走輸入法 / 點到別的輸入框 / Alt+Tab 出去,
  #     那顆 Shift 的放開我們永遠看不到,下一次的第一顆 Shift 放開
  #     會被算成上一段的結尾而**誤切一次中英**。
  #   · 兩處 shift_tap_.OnKey() 也註解掉 → 三支守門全綠。
  #     症狀:輕點 Shift **整個功能完全不作用**,而沒有任何守門會叫。
  #
  # ── S4:這條規則的第三組呼叫點(滑鼠)────────────────────────────
  #
  # ⚠ 上面兩組守的是「按鍵」那一半。狀態機還有**看不到的那一半**:滑鼠。
  #   `按住 Shift → 滑鼠點擊 → 放開 Shift` 在四支 key event sink 眼裡與
  #   一次乾淨的輕點逐位元相同,而那是**延伸選取的標準手勢**,這顆鍵又
  #   預設是開的。⚠ 上面那三個重置點**一個都碰不到它**:在同一個輸入框裡
  #   點一下不會換 document manager,兩個 OnSetFocus 都不會來。
  #
  #   補法是第三個入口 shift_tap_.OnOtherInput()(common/shift_tap.h),
  #   接在兩處宿主會告訴我們「文件被別的東西動了」的回呼上。它同樣是
  #   「純函式驗得到判斷、驗不到有沒有人呼叫」,所以同樣落在這一條。
  #
  # ⚠ 這一組多守三格**線路本身**(繼承、QueryInterface、AdviseSink)。
  #   理由是那三格壞掉**完全沒有訊號**:少了 QueryInterface 那一行,
  #   AdviseSink 拿到 E_NOINTERFACE,而掛不上是刻意安靜放棄的(有些宿主
  #   本來就不給)—— 編得過、一個測試都不紅,只是這個修法在**每一個**
  #   宿主裡都不生效。
  #
  # ⚠ 判準刻意不是「grep 得到 shift_tap_ 這個名字」——tsf/text_service.cc
  #   的註解裡到處都是這個名字,那種判準第一次改動就會被繞開(而且它
  #   正是這個專案抓過的形狀)。所以:先把註解與字串遮掉,再**分函式**
  #   數,而且分母寫死在下面 —— 掃到 0 個是紅,不是「0 個違規」。
  if ! python3 - "${root}" <<'PY_SHIFT_TAP_WIRING'; then bad=1; fi
import os
import re
import sys

root = sys.argv[1]
path = os.path.join(root, 'windows', 'tsf', 'text_service.cc')


def mask(s):
    """把註解與字串字面值換成空白(長度不變,行號還對得上)。"""
    out = list(s)
    i, n = 0, len(s)
    while i < n:
        c = s[i]
        if c == '/' and i + 1 < n and s[i + 1] == '/':
            while i < n and s[i] != '\n':
                out[i] = ' '
                i += 1
            continue
        if c == '/' and i + 1 < n and s[i + 1] == '*':
            while i + 1 < n and not (s[i] == '*' and s[i + 1] == '/'):
                if s[i] != '\n':
                    out[i] = ' '
                i += 1
            for _ in range(2):
                if i < n:
                    out[i] = ' '
                    i += 1
            continue
        if c == '"' or c == "'":
            q = c
            i += 1
            while i < n:
                if s[i] == '\\' and i + 1 < n:
                    out[i] = out[i + 1] = ' '
                    i += 2
                    continue
                if s[i] == q:
                    i += 1
                    break
                if s[i] != '\n':
                    out[i] = ' '
                i += 1
            continue
        i += 1
    return ''.join(out)


def line_of(src, pos):
    return src.count('\n', 0, pos) + 1


bad = []
if not os.path.isfile(path):
    print('!! 找不到 %s —— 規則 4 沒有東西可守' % path, file=sys.stderr)
    raise SystemExit(1)
raw = open(path, encoding='utf-8').read()
src = mask(raw)

# ── 切出每一個頂層函式的本體 ────────────────────────────────────
funcs = []   # (名字, 簽章文字, 本體, 本體在檔案裡的起點)
for m in re.finditer(r'^[A-Za-z_][^\n;(){}]*TextService::(\w+)\(', src, re.M):
    # 從簽章的左括號配對到右括號,再找它後面第一個 '{'
    i = src.index('(', m.end() - 1)
    depth = 0
    while i < len(src):
        if src[i] == '(':
            depth += 1
        elif src[i] == ')':
            depth -= 1
            if depth == 0:
                break
        i += 1
    b = src.find('{', i)
    if b < 0:
        continue
    depth = 0
    j = b
    while j < len(src):
        if src[j] == '{':
            depth += 1
        elif src[j] == '}':
            depth -= 1
            if depth == 0:
                break
        j += 1
    funcs.append((m.group(1), src[m.start():b], src[b:j + 1], b))

if not funcs:
    print('!! text_service.cc 裡一個 TextService:: 函式都沒切出來 —— '
          '規則 4 的切法壞了,不當成通過', file=sys.stderr)
    raise SystemExit(2)


def body_of(name, sig_needle, what):
    hits = [f for f in funcs
            if f[0] == name and (sig_needle is None or sig_needle in f[1])]
    if len(hits) != 1:
        bad.append('找不到(或找到 %d 個)%s —— 規則 4 對不上這一格了,'
                   '不是「沒有違規」' % (len(hits), what))
        return None
    return hits[0][2]


# ── 分母:從程式碼數出來的,改動線路就要連這裡一起改 ──────────────
#
#   重置點 3 個 —— 三種「那顆 Shift 的放開我們永遠看不到」的走法:
RESET_SITES = [
    ('Deactivate', None,
     'ITfTextInputProcessor::Deactivate(按著 Shift 切走輸入法)'),
    ('OnSetFocus', 'ITfDocumentMgr',
     'ITfThreadMgrEventSink::OnSetFocus(按著 Shift 點到別的輸入框)'),
    ('OnSetFocus', 'BOOL',
     'ITfKeyEventSink::OnSetFocus(Alt+Tab 走掉,只有這一個會被呼叫)'),
]
#   餵入點 2 個 —— 按下與放開,而且只在 Test 那一趟(見那兩處的 ⚠)。
FEED_SITES = [
    ('OnTestKeyDown', None, 'false', '按下那一半'),
    ('OnTestKeyUp', None, 'true', '放開那一半'),
]
#   看不見的那一半 2 個 —— 滑鼠 / 觸控 / 手寫筆動了宿主的文件(S4)。
#   ⚠ 兩個都要,而且是**兩條獨立的腿**:OnEndEdit 不需要正在組字,
#     但需要宿主肯回報選取變化;OnCompositionTerminated 反過來。
OTHER_INPUT_SITES = [
    ('OnEndEdit', None,
     'ITfTextEditSink::OnEndEdit(在同一個輸入框裡用滑鼠點一下)'),
    ('OnCompositionTerminated', None,
     'ITfCompositionSink::OnCompositionTerminated(打字打到一半點下去)'),
]

RESET_RE = re.compile(r'\bshift_tap_\s*\.\s*Reset\s*\(\s*\)')
FEED_RE = re.compile(r'\bshift_tap_\s*\.\s*OnKey\s*\(')
OTHER_RE = re.compile(r'\bshift_tap_\s*\.\s*OnOtherInput\s*\(\s*\)')

total_reset = len(RESET_RE.findall(src))
total_feed = len(FEED_RE.findall(src))
total_other = len(OTHER_RE.findall(src))

if total_reset != len(RESET_SITES):
    bad.append('text_service.cc 裡有 %d 處 shift_tap_.Reset()(註解不算)'
               ' —— 應該是 %d 處。少一處 = 那條路的 Shift 放開看不到,'
               '下一次會誤切一次中英。'
               % (total_reset, len(RESET_SITES)))
if total_feed != len(FEED_SITES):
    bad.append('text_service.cc 裡有 %d 處 shift_tap_.OnKey((註解不算)'
               ' —— 應該是 %d 處。0 處 = 輕點 Shift 整個功能斷線,'
               '而畫面上什麼都看不出來;3 處 = 同一顆鍵餵兩次,'
               '狀態機會把它當自動重複而整段作廢(見 OnKeyUp 的 ⚠)。'
               % (total_feed, len(FEED_SITES)))
if total_other != len(OTHER_INPUT_SITES):
    bad.append('text_service.cc 裡有 %d 處 shift_tap_.OnOtherInput()(註解不算)'
               ' —— 應該是 %d 處。那是狀態機**唯一看得到滑鼠**的地方:'
               '少了它,「按住 Shift → 滑鼠點擊 → 放開 Shift」'
               '(延伸選取的標準手勢)會切一次中英。'
               % (total_other, len(OTHER_INPUT_SITES)))

for name, needle, what in RESET_SITES:
    body = body_of(name, needle, what)
    if body is None:
        continue
    n = len(RESET_RE.findall(body))
    if n != 1:
        bad.append('%s 裡有 %d 處 shift_tap_.Reset() —— 要剛好 1 處'
                   % (what, n))

for name, needle, key_up, what in FEED_SITES:
    body = body_of(name, needle, what)
    if body is None:
        continue
    n = len(FEED_RE.findall(body))
    if n != 1:
        bad.append('%s(%s)裡有 %d 處 shift_tap_.OnKey( —— 要剛好 1 處'
                   % (name, what, n))
        continue
    # 餵進去的必須是**這一趟**的事件。兩邊餵同一個方向的話,
    # 狀態機收不到放開,或收到兩次按下 —— 兩種都是這顆鍵完全不動。
    if not re.search(r'BuildKeyEvent\(\s*w\s*,\s*l\s*,\s*%s\s*\)' % key_up,
                     body):
        bad.append('%s 裡找不到 BuildKeyEvent(w, l, key_up=%s) —— '
                   '餵進狀態機的不是這一趟的事件' % (name, key_up))

for name, needle, what in OTHER_INPUT_SITES:
    body = body_of(name, needle, what)
    if body is None:
        continue
    n = len(OTHER_RE.findall(body))
    if n != 1:
        bad.append('%s 裡有 %d 處 shift_tap_.OnOtherInput() —— 要剛好 1 處'
                   % (what, n))

# ── S4 的線路本身:壞掉完全沒有訊號的那三格 ──────────────────────
#
# ⚠ 上面數的是「有沒有人呼叫」。但 OnEndEdit 要**被呼叫得到**,還得有
#   三件事同時成立,而三件的任何一件壞掉都是「編得過、沒有測試會紅、
#   在每一個宿主裡都不生效」:
#     (a) TextService 真的繼承 ITfTextEditSink;
#     (b) QueryInterface 認得 IID_ITfTextEditSink —— 少了它 AdviseSink
#         回 E_NOINTERFACE,而掛不上是刻意安靜放棄的(有些宿主不給);
#     (c) 真的有人 AdviseSink(IID_ITfTextEditSink, …) 把它掛到 context 上。
hdr_path = os.path.join(root, 'windows', 'tsf', 'text_service.h')
if not os.path.isfile(hdr_path):
    bad.append('找不到 %s —— 規則 4 的線路那三格沒有東西可守' % hdr_path)
else:
    hdr = mask(open(hdr_path, encoding='utf-8').read())
    if not re.search(r'public\s+ITfTextEditSink\b', hdr):
        bad.append('text_service.h 沒有 public ITfTextEditSink —— '
                   'OnEndEdit 不會是任何人的回呼,S4 的主要那條腿整條不存在')

qi = body_of('QueryInterface', None, 'TextService::QueryInterface')
if qi is not None and 'IID_ITfTextEditSink' not in qi:
    bad.append('TextService::QueryInterface 不認得 IID_ITfTextEditSink —— '
               'AdviseSink 會拿到 E_NOINTERFACE,而掛不上是安靜放棄的:'
               '沒有任何訊號,只是 Shift+滑鼠點擊在每個宿主裡都誤切一次')

if not re.search(r'AdviseSink\(\s*IID_ITfTextEditSink', src):
    bad.append('text_service.cc 裡沒有 AdviseSink(IID_ITfTextEditSink —— '
               '介面實作了卻沒有掛到任何 context 上,一則通知都不會來')

# ⚠ 上面那一條只驗「AdviseSink 那一行還在檔案裡」,而它住在 WatchContext
#   的本體裡 —— Deactivate 的 WatchContext(nullptr) 讓那一行**永遠**留著。
#   於是把**掛接的呼叫端**全部註解掉(5 處 WatchFocusedContext() /
#   WatchContextOf()),AdviseSink 一次都執行不到,而三支守門仍然全綠:
#   這是實跑出來的,不是推的。症狀與上面那三格一模一樣 —— 編得過、
#   一個測試都不紅、S4 在**每一個**宿主裡都不生效。所以這裡再分函式驗
#   一次「有沒有人真的去掛」,從入口一路到 AdviseSink 那一行。
ATTACH_SITES = [
    ('ActivateEx', None, 'WatchFocusedContext',
     'ActivateEx(切過來的當下焦點可能已經在某個輸入框上了,'
     '不主動掛一次的話那個輸入框裡一則 OnEndEdit 都不會有)'),
    ('OnSetFocus', 'ITfDocumentMgr', 'WatchContextOf',
     'ITfThreadMgrEventSink::OnSetFocus(換了輸入框就要換掛的 context)'),
    ('OnPushContext', None, 'WatchFocusedContext',
     'OnPushContext(context 堆疊動了,最上層那一個可能換人)'),
    ('OnPopContext', None, 'WatchFocusedContext', 'OnPopContext(同上)'),
    ('WatchFocusedContext', None, 'WatchContextOf',
     'WatchFocusedContext(問焦點 → 掛)'),
    ('WatchContextOf', None, 'WatchContext',
     'WatchContextOf(取最上層 context → 掛)'),
]
for _name, _needle, _callee, _what in ATTACH_SITES:
    _body = body_of(_name, _needle, _what)
    if _body is None:
        continue
    if not re.search(r'\b%s\s*\(' % _callee, _body):
        bad.append('%s 裡沒有呼叫 %s( —— ITfTextEditSink 掛不到任何 context '
                   '上。AdviseSink 那一行還在(Deactivate 的 '
                   'WatchContext(nullptr) 讓它永遠留著),所以上面那一條'
                   '看不出來:編得過、一個測試都不紅,而 S4 在每一個宿主裡'
                   '都不生效。' % (_what, _callee))

# ── 餵了但把答案丟掉 = 沒餵 ──────────────────────────────────────
#
# 只數呼叫點的話,「保留 OnKey、把回傳值扔掉」會是綠的,而那與整個
# 功能斷線是同一個症狀。所以放開那一半要一路驗到它真的送出切換。
up = body_of('OnTestKeyUp', None, 'ITfKeyEventSink::OnTestKeyUp')
if up is not None:
    m = re.search(r'ShiftTap\s+(\w+)\s*=\s*shift_tap_\s*\.\s*OnKey\(', up)
    if not m:
        bad.append('OnTestKeyUp 沒有把 shift_tap_.OnKey( 的回傳值接起來 —— '
                   '餵了卻不看答案,與整個功能斷線是同一個症狀')
    else:
        var = m.group(1)
        if not re.search(r'\b%s\b\s*[!=]=\s*ShiftTap::kToggleAsciiMode' % var,
                         up):
            bad.append('OnTestKeyUp 接了回傳值 %s 卻沒有拿它跟 '
                       'ShiftTap::kToggleAsciiMode 比 —— 同上' % var)
        if 'SendAsciiToggle(' not in up:
            bad.append('OnTestKeyUp 裡沒有 SendAsciiToggle( —— '
                       '偵測到輕點之後沒有人真的去切中英')

for b in bad:
    print('!! ' + b, file=sys.stderr)
if bad:
    print('!! 輕點 Shift 的接線稽核失敗:%d 項。判斷在 common/shift_tap.cc,'
          '接線只有這一條守得到。' % len(bad), file=sys.stderr)
    raise SystemExit(1)
print('   輕點 Shift 接線:%d 個餵入點 / %d 個重置點 / %d 個「看不到的輸入」'
      '入口 / %d 個掛接呼叫點,都在該在的函式裡'
      % (total_feed, total_reset, total_other, len(ATTACH_SITES)))
PY_SHIFT_TAP_WIRING

  # ── 規則 5:換方案那一趟,傳進去的**是哪兩份偏好** ────────────────
  #
  # 規則 3 驗的是「三個 token 在不在、誰先誰後」——**它不驗參數**。
  # 那個縫隙很寬,實跑覆核過兩個植入,兩個都是三支守門全綠:
  #
  #   · 把 PickVariantPrefForSchemaSwitch 的兩份偏好對調
  #     → 648c02c 的原缺陷(換方案洗掉簡繁)原封不動復活。
  #   · settings_readable 永遠傳 false
  #     → 永遠不讀設定檔,同上,而原始碼上只是一個 `false`。
  #
  # 第一個現在由**型別**擋:兩份偏好包成 OnDiskPref / EngineCopyPref
  # (schema_choice.h),對調就編不過,run_logic_tests.sh 與
  # syntax_check_mingw.sh 各紅一次。這一條守的是型別擋不掉的那一半 ——
  # **包進去的是不是正確的來源**:設定檔那一格必須真的走 settings_->Load(),
  # 引擎那一格必須真的是 VariantPrefCopy()。包錯來源型別一樣過得了編譯器。
  #
  # ⚠ 判準是「取出這一個呼叫的實際引數,再看每一格的來源」,不是
  #   grep 整個檔案 —— 後者只要別處出現過一次 settings_ 就會被騙過去。
  if ! python3 - "${root}" <<'PY_VARIANT_PICK_ARGS'; then bad=1; fi
import os
import re
import sys

root = sys.argv[1]
path = os.path.join(root, 'windows', 'service', 'pipe_server.cc')


def mask(s):
    out = list(s)
    i, n = 0, len(s)
    while i < n:
        c = s[i]
        if c == '/' and i + 1 < n and s[i + 1] == '/':
            while i < n and s[i] != '\n':
                out[i] = ' '
                i += 1
            continue
        if c == '/' and i + 1 < n and s[i + 1] == '*':
            while i + 1 < n and not (s[i] == '*' and s[i + 1] == '/'):
                if s[i] != '\n':
                    out[i] = ' '
                i += 1
            for _ in range(2):
                if i < n:
                    out[i] = ' '
                    i += 1
            continue
        if c == '"' or c == "'":
            q = c
            i += 1
            while i < n:
                if s[i] == '\\' and i + 1 < n:
                    out[i] = out[i + 1] = ' '
                    i += 2
                    continue
                if s[i] == q:
                    i += 1
                    break
                if s[i] != '\n':
                    out[i] = ' '
                i += 1
            continue
        i += 1
    return ''.join(out)


if not os.path.isfile(path):
    print('!! 找不到 %s —— 規則 5 沒有東西可守' % path, file=sys.stderr)
    raise SystemExit(1)
src = mask(open(path, encoding='utf-8').read())

start = src.find('case Op::kSelectSchema:')
if start < 0:
    print('!! pipe_server.cc 裡找不到 case Op::kSelectSchema —— '
          '規則 5 沒有東西可守', file=sys.stderr)
    raise SystemExit(1)
nxt = src.find('case Op::', start + 1)
blk = src[start:nxt if nxt > 0 else len(src)]

calls = [m for m in re.finditer(r'PickVariantPrefForSchemaSwitch\s*\(', blk)]
if len(calls) != 1:
    print('!! kSelectSchema 裡有 %d 處 PickVariantPrefForSchemaSwitch( —— '
          '要剛好 1 處(0 = 換方案拿引擎手上那份過期的偏好去洗掉使用者'
          '剛選的簡繁;2 處以上 = 有一份會漂走)' % len(calls), file=sys.stderr)
    raise SystemExit(1)

# 括號配對,取出實際引數,再照頂層逗號切開。
i = blk.index('(', calls[0].end() - 1)
depth = 0
j = i
while j < len(blk):
    if blk[j] == '(':
        depth += 1
    elif blk[j] == ')':
        depth -= 1
        if depth == 0:
            break
    j += 1
inner = blk[i + 1:j]
args, depth, cur = [], 0, ''
for ch in inner:
    if ch in '([{':
        depth += 1
    elif ch in ')]}':
        depth -= 1
    if ch == ',' and depth == 0:
        args.append(cur)
        cur = ''
        continue
    cur += ch
args.append(cur)
args = [' '.join(a.split()) for a in args]

bad = []
if len(args) != 2:
    bad.append('這一個呼叫有 %d 個引數 —— 簽章應該是 '
               '(const OnDiskPref&, const EngineCopyPref&) 兩個。'
               '規則 5 對不上了,不是「沒有違規」:%r' % (len(args), args))
else:
    on_disk, engine_copy = args
    # 第一格:設定檔那一份。必須真的有一條會去讀設定檔的路。
    if 'OnDiskPref::FromSettingsFile(' not in on_disk or \
       'settings_' not in on_disk or 'SchemaPref()' not in on_disk:
        bad.append('第 1 個引數不是從設定檔讀來的:%r。'
                   '永遠給 OnDiskPref::Unreadable() = 永遠拿引擎手上那份'
                   '過期的複本 = 648c02c 的原缺陷換一個入口。' % on_disk)
    if 'VariantPrefCopy' in on_disk:
        bad.append('第 1 個引數裡出現 VariantPrefCopy —— 兩份偏好包反了。'
                   '設定檔那一格拿到引擎的複本,就是換方案洗掉簡繁本身。')
    # 第二格:引擎手上那一份複本。
    if 'EngineCopyPref(' not in engine_copy or \
       'VariantPrefCopy()' not in engine_copy:
        bad.append('第 2 個引數不是 EngineCopyPref(engine_->VariantPrefCopy())'
                   ':%r' % engine_copy)
    if 'settings_' in engine_copy:
        bad.append('第 2 個引數裡出現 settings_ —— 兩份偏好包反了(同上)')

for b in bad:
    print('!! ' + b, file=sys.stderr)
if bad:
    print('!! 換方案的簡繁來源稽核失敗:%d 項。見 common/schema_choice.h 的 '
          'OnDiskPref 檔頭。' % len(bad), file=sys.stderr)
    raise SystemExit(1)
print('   換方案的簡繁來源:設定檔 → OnDiskPref、引擎複本 → EngineCopyPref,'
      '兩格都沒包反')
PY_VARIANT_PICK_ARGS

  # ── 規則 6:那一橫的「我在用」訊號,四個邊一個都不准掉 ─────────────
  #
  # 守的是工單 #82 的另一半,而它與規則 4 是同一個形狀:**判準有人守,
  # 訊號沒有。** 那一橫的兩層判準都是純函式(common/bar_owner.cc 收斂
  # 13 個宿主、common/bar_visibility.cc 管遲滯,兩支都有單元測試),
  # 而「使用者切到我們 / 切走」這件事怎麼進到服務端 —— 也就是在場連線
  # 那四個邊 —— 只有這一條守得到。
  #
  # 四個邊,而且**每一個都對應一個使用者實機回報過的症狀**:
  #
  #   開 ActivateEx           切到我們(從別的輸入法)。少了它 = S1:
  #                           切回來之後要打第一個字那一橫才出現。
  #   開 OnActivated(啟用邊)  在我們自家三份 profile 之間切。三份共用
  #                           一個 CLSID,所以 TSF **不會**重新 Activate
  #                           —— 少了它,繁↔簡切一次那一橫就不見了。
  #   關 Deactivate           切走 / 宿主收工。少了它 = #82:常駐。
  #   關 OnActivated(非啟用邊) 使用者切到微軟拼音。⚠ **這是四個邊裡唯一
  #                           在那一刻保證會來的**,而它以前被兩個早退
  #                           整個丟掉 = S4:「我現在用其他的輸入法,
  #                           但是他突然出現了」。
  #
  # ⚠ 所以這一條**不可以**只數 Start / Stop 的次數:兩個早退還在的時候
  #   次數是對的,而 S4 照樣發生。要看的是 OnActivated 有沒有在讀
  #   `flags` **之前**就 return 掉。
  #
  # ⚠ 這一條**不能**用 grep 名字了事:text_service.cc 的註解裡到處都是
  #   PresenceLink 這幾個字。與規則 4 同一個判準 —— 先遮掉註解與字串,
  #   再**分函式**看。
  #
  # ⚠ 為什麼連「ActivateEx 裡不可以有 EnsureReady(」也守:那是最像對的
  #   修法,而它會把開管道與握手搬到**宿主的 UI 執行緒**上等
  #   (text_service.cc 那一段註解拒絕它的理由),症狀是每一次
  #   Win+空白鍵切輸入法都卡一下。編得過、測試全綠、沒有任何訊號。
  if ! python3 - "${root}" <<'PY_BAR_PRESENCE_WIRING'; then bad=1; fi
import os
import re
import sys

root = sys.argv[1]
path = os.path.join(root, 'windows', 'tsf', 'text_service.cc')
hdr_path = os.path.join(root, 'windows', 'tsf', 'text_service.h')


def mask(s):
    """把註解與字串字面值換成空白(長度不變,行號還對得上)。"""
    out = list(s)
    i, n = 0, len(s)
    while i < n:
        c = s[i]
        if c == '/' and i + 1 < n and s[i + 1] == '/':
            while i < n and s[i] != '\n':
                out[i] = ' '
                i += 1
            continue
        if c == '/' and i + 1 < n and s[i + 1] == '*':
            while i + 1 < n and not (s[i] == '*' and s[i + 1] == '/'):
                if s[i] != '\n':
                    out[i] = ' '
                i += 1
            for _ in range(2):
                if i < n:
                    out[i] = ' '
                    i += 1
            continue
        if c == '"' or c == "'":
            q = c
            i += 1
            while i < n:
                if s[i] == '\\' and i + 1 < n:
                    out[i] = out[i + 1] = ' '
                    i += 2
                    continue
                if s[i] == q:
                    i += 1
                    break
                if s[i] != '\n':
                    out[i] = ' '
                i += 1
            continue
        i += 1
    return ''.join(out)


bad = []
for p in (path, hdr_path):
    if not os.path.isfile(p):
        print('!! 找不到 %s —— 規則 6 沒有東西可守' % p, file=sys.stderr)
        raise SystemExit(1)
src = mask(open(path, encoding='utf-8').read())
hdr = mask(open(hdr_path, encoding='utf-8').read())
pipe_path = os.path.join(root, 'windows', 'service', 'pipe_server.cc')
bar_path = os.path.join(root, 'windows', 'service', 'status_bar.cc')
for p2 in (pipe_path, bar_path):
    if not os.path.isfile(p2):
        print('!! 找不到 %s —— 規則 6 沒有東西可守' % p2, file=sys.stderr)
        raise SystemExit(1)
pipe = mask(open(pipe_path, encoding='utf-8').read())
bar = mask(open(bar_path, encoding='utf-8').read())

funcs = []   # (名字, 簽章文字, 本體)
for m in re.finditer(r'^[A-Za-z_][^\n;(){}]*TextService::(\w+)\(', src, re.M):
    i = src.index('(', m.end() - 1)
    depth = 0
    while i < len(src):
        if src[i] == '(':
            depth += 1
        elif src[i] == ')':
            depth -= 1
            if depth == 0:
                break
        i += 1
    b = src.find('{', i)
    if b < 0:
        continue
    depth = 0
    j = b
    while j < len(src):
        if src[j] == '{':
            depth += 1
        elif src[j] == '}':
            depth -= 1
            if depth == 0:
                break
        j += 1
    funcs.append((m.group(1), src[m.start():b], src[b:j + 1]))

if not funcs:
    print('!! text_service.cc 裡一個 TextService:: 函式都沒切出來 —— '
          '規則 6 的切法壞了,不當成通過', file=sys.stderr)
    raise SystemExit(2)


def body_of(name, what):
    hits = [f for f in funcs if f[0] == name]
    if len(hits) != 1:
        bad.append('找不到(或找到 %d 個)%s —— 規則 6 對不上這一格了,'
                   '不是「沒有違規」' % (len(hits), what))
        return None
    return hits[0][2]


START_RE = re.compile(r'\bPresenceLink\s*::\s*Start\s*\(')
STOP_RE = re.compile(r'\bpresence_\s*->\s*Stop\s*\(')
OPEN_RE = re.compile(r'\bEnsurePresence\s*\(\s*\)')
CLOSE_RE = re.compile(r'\bClosePresence\s*\(\s*\)')

# ── 分母:從程式碼數出來的。掃到 0 個是紅,不是「0 個違規」──────────
n_start = len(START_RE.findall(src))
n_stop = len(STOP_RE.findall(src))
if n_start != 1:
    bad.append('text_service.cc 裡有 %d 處 PresenceLink::Start((註解不算)'
               ' —— 應該剛好 1 處,而且在 EnsurePresence 裡。0 處 = 那一橫'
               '的訊號整條不存在:切到我們之後服務端看不到這個宿主,'
               '使用者要按下第一顆鍵才會有一條連線(S1)。' % n_start)
if n_stop != 1:
    bad.append('text_service.cc 裡有 %d 處 presence_->Stop((註解不算)'
               ' —— 應該剛好 1 處,而且在 ClosePresence 裡。0 處 = **#82 原'
               '缺陷復活**:切走輸入法之後那筆註冊還在,那一橫藏不起來。'
               % n_stop)

# ⚠ 兩支包裝函式存在的理由是「各有兩個呼叫點」。Start/Stop 收在裡面,
#   守門才有辦法分別檢查那四個邊,而不是只數總次數。
ensure = body_of('EnsurePresence', 'TextService::EnsurePresence')
if ensure is not None and len(START_RE.findall(ensure)) != 1:
    bad.append('EnsurePresence 裡沒有 PresenceLink::Start( —— 那一橫的開場'
               '訊號被搬走了,而 ActivateEx 與 profile sink 兩個呼叫點看'
               '起來都還在。')
closer = body_of('ClosePresence', 'TextService::ClosePresence')
if closer is not None and len(STOP_RE.findall(closer)) != 1:
    bad.append('ClosePresence 裡沒有 presence_->Stop( —— 收尾訊號被掏空,'
               '而兩個呼叫點看起來都還在。')

# ── 四個邊,一個一個看 ───────────────────────────────────────────
act = body_of('ActivateEx', 'ITfTextInputProcessorEx::ActivateEx')
if act is not None:
    if len(OPEN_RE.findall(act)) != 1:
        bad.append('ActivateEx 裡沒有 EnsurePresence() —— 訊號沒有接在'
                   '「使用者切到這個輸入法」這件事上。接在別的地方(例如'
                   '第一顆按鍵)就是 S1 這個缺陷本身。')
    # ⚠ 最像對、而且**不可以**的那個修法。
    if re.search(r'\bEnsureReady\s*\(', act):
        bad.append('ActivateEx 裡出現了 EnsureReady( —— 開管道與握手會在'
                   '**宿主的 UI 執行緒**上等,每一次 Win+空白鍵切輸入法都會'
                   '卡一下。理由見 ActivateEx 裡那一段註解與 PresenceLink 的'
                   '檔頭:在場連線要走自己的背景執行緒。')

deact = body_of('Deactivate', 'ITfTextInputProcessor::Deactivate')
if deact is not None and len(CLOSE_RE.findall(deact)) != 1:
    bad.append('Deactivate 裡沒有 ClosePresence() —— 切走輸入法之後那條'
               '在場連線還開著,服務端那筆註冊不會消失,那一橫**常駐'
               '在螢幕上**。那正是使用者上一次回報的缺陷(#82)。')

# ── ⭐ profile sink:S4 那兩個邊 ──────────────────────────────────
onact = body_of('OnActivated', 'ITfInputProcessorProfileActivationSink::OnActivated')
if onact is not None:
    # ⚠ 這一格是這一輪的核心。舊版的形狀是:
    #     if (!IsEqualCLSID(clsid, CLSID_RimeTextService)) return S_OK;
    #     if (!(flags & TF_IPSINK_FLAG_ACTIVE)) return S_OK;
    #   兩個早退把「使用者切到微軟拼音」那一刻**唯一會來的**兩則通知
    #   整個丟掉。判準因此是位置式的:旗標要在第一個 return 之前被讀到。
    iflag = onact.find('TF_IPSINK_FLAG_ACTIVE')
    mret = re.search(r'\breturn\b', onact)
    iret = mret.start() if mret else -1
    if iflag < 0:
        bad.append('OnActivated 裡讀不到 TF_IPSINK_FLAG_ACTIVE —— 分不出'
                   '「被啟用」與「被停用」,而後者是使用者切到別的輸入法時'
                   '唯一會來的東西(S4)。')
    elif iret >= 0 and iret < iflag:
        bad.append('OnActivated 在讀 flags **之前**就 return 了 —— 那正是'
                   '舊版那兩個早退的形狀,而它丟掉的是使用者切到微軟拼音'
                   '那一刻唯一會來的兩則通知。症狀是 S4:'
                   '「我現在用其他的輸入法,但是他突然出現了」。')
    if len(OPEN_RE.findall(onact)) != 1:
        bad.append('OnActivated 裡沒有 EnsurePresence() —— 三份 profile 共用'
                   '一個 CLSID,在自家繁↔簡之間切**不會**重新 ActivateEx,'
                   '只有這一則會來。少了它,切一次字形那一橫就再也不回來。')
    if len(CLOSE_RE.findall(onact)) != 1:
        bad.append('OnActivated 裡沒有 ClosePresence() —— 使用者切到別的'
                   '輸入法時服務端不會知道,那一橫會在他已經換了輸入法'
                   '之後繼續冒出來(S4)。Deactivate 補不上這一格:'
                   '背景執行緒上它延遲或永不會來。')

# ── 這條連線報不報得出自己是誰 ───────────────────────────────────
#
# ⚠ 少了這一格,在場連線就退回「有一條 handle 開著」—— 一個沒有產品
#   意義的量,而 12 個背景宿主每一條都算一票。
if not re.search(r'\bh\.host_tid\s*=\s*host_tid_', src):
    bad.append('PresenceLink 的 HELLO 沒有帶 host_tid —— 服務端只知道'
               '「有一條連線」,不知道它是**哪一條執行緒**上的。輸入法在'
               'Windows 上是 per-thread 的,少了 tid 那一橫在'
               '「每個視窗各自的輸入法」模式下會再壞一次。')
if not re.search(r'\bEncodeHello\s*\(', src):
    bad.append('PresenceLink 不再握手 —— 沒握手的連線在服務端是'
               'activated=false,一票都不投,那一橫永遠不會顯示。')

# ── 成員宣告在不在 ───────────────────────────────────────────────
if not re.search(r'PresenceLink\s*\*\s*presence_', hdr):
    bad.append('text_service.h 裡沒有 PresenceLink* presence_ —— 上面那兩格'
               '就沒有東西可守了')

# ── 服務端:那筆註冊要真的收得到身分,而且要真的去問前景 ────────────
if not re.search(
        r'bar_->OnClientIdentified\(client_id,\s*h\.host_pid,\s*h\.host_tid\)',
        pipe):
    bad.append('pipe_server.cc 的 HELLO 分支沒有把 (pid, tid) 交給那一橫 —— '
               '每一條連線於是又變回一張沒有身分的票,而那就是 S4。')
if len(re.findall(r'\bDecideBarOwner\s*\(', bar)) != 1:
    bad.append('status_bar.cc 沒有(或有多處)呼叫 DecideBarOwner( —— '
               '收斂 13 個宿主的那一層被繞開了。判準本身有測試,'
               '**被繞開**沒有任何測試看得到。')
if not re.search(r'if\s*\(\s*!\s*owner\.os_unknown\s*\)', bar):
    bad.append('status_bar.cc 沒有看 owner.os_unknown —— OS 答不出前景'
               '(UAC 提示、安全桌面)時會被讀成「沒有人在用」,那一橫'
               '在使用者只是被問了一次系統權限之後開始倒數消失。')
if not re.search(r'\bReadForegroundOwner\s*\(\s*\)', bar):
    bad.append('status_bar.cc 沒有去問 OS 前景是誰 —— 剩下的就只有'
               '宿主自己的宣稱,而兩個宿主同時宣稱就退化回舊判準的 OR。')

# ── 提權宿主那道閘只有一個入口 ───────────────────────────────────
#
# ⚠ 在場連線那條路**只准 CreateFileW,不准啟動服務**:啟動會產生一支
#   與使用者日常身分不同的服務(common/elevation_policy.h),而那道閘
#   住在 LaunchService() 裡。這裡守的是「它沒有長出第二個呼叫點」。
n_launch = len(re.findall(r'\bLaunchService\s*\(', src))
if n_launch != 1:
    bad.append('text_service.cc 裡有 %d 處 LaunchService((註解不算)—— '
               '應該剛好 1 處,而且是 ServiceStarterThread 那一處。'
               '在場連線那條路只准連,不准啟動:提權宿主那道閘'
               '(MayStartUserService)在 LaunchService 裡。' % n_launch)

for b in bad:
    print('!! ' + b, file=sys.stderr)
if bad:
    print('!! 那一橫的在場訊號稽核失敗:%d 項。判準在 common/bar_owner.cc 與'
          ' common/bar_visibility.cc(都有單元測試),**訊號**只有這一條與'
          ' verify_tsf.sh 的 --watch-presence 守得到。' % len(bad),
          file=sys.stderr)
    raise SystemExit(1)
print('   那一橫的在場訊號:四個邊都在(ActivateEx / OnActivated 啟用邊 開,'
      'Deactivate / OnActivated 非啟用邊 收);OnActivated 先讀 flags 才 return;'
      'HELLO 帶得出 host_tid;服務端接得到身分、而且真的去問前景;'
      'ActivateEx 裡沒有 EnsureReady(;LaunchService( 仍然只有 1 個呼叫點')
PY_BAR_PRESENCE_WIRING

  return "${bad}"
}

if [ "${1:-}" = "--self-check" ]; then
  # 反向測試:真的把違規植入一份**複本**,要求上面那一支抓得到。
  # 不接受只看綠燈 —— 這個專案有過「守門的東西沒有人守」。
  fail=0
  for plant in plan_variant bare_select_schema no_select_and_apply \
               no_variant_reread variant_reread_too_late \
               shift_tap_no_reset shift_tap_no_feed \
               shift_tap_drops_the_answer \
               shift_tap_no_other_input \
               shift_tap_no_other_input_on_end_edit \
               shift_tap_no_edit_sink_base shift_tap_no_edit_sink_qi \
               shift_tap_no_edit_sink_advise \
               shift_tap_edit_sink_never_attached \
               variant_pick_swapped_sources \
               variant_pick_settings_always_unreadable \
               presence_no_start presence_never_closed \
               presence_ensure_ready_in_activate \
               presence_launches_service \
               profile_sink_drops_the_deactivate_edge \
               presence_not_reopened_on_profile_switch \
               presence_not_closed_on_profile_switch \
               presence_never_says_who_it_is \
               bar_takes_every_connection_as_a_vote \
               bar_ignores_the_foreground \
               bar_overwrites_state_when_os_cannot_answer; do
    tmp="$(mktemp -d)"
    mkdir -p "${tmp}/windows/service" "${tmp}/windows/tsf"
    cp "${ROOT_DEFAULT}/windows/service/main.cc" \
       "${ROOT_DEFAULT}/windows/service/pipe_server.cc" \
       "${ROOT_DEFAULT}/windows/service/status_bar.cc" \
       "${ROOT_DEFAULT}/windows/service/engine.cc" "${tmp}/windows/service/"
    # ⚠ .h 也要複製:S4 那三格線路檢查(繼承 / QueryInterface / AdviseSink)
    #   其中一格看的是標頭。少了它,那一格會變成「找不到檔案」的紅,
    #   而不是「植入被抓到」的紅 —— 兩者長得一樣但證明的事不同。
    cp "${ROOT_DEFAULT}/windows/tsf/text_service.cc" \
       "${ROOT_DEFAULT}/windows/tsf/text_service.h" "${tmp}/windows/tsf/"
    case "${plant}" in
      plan_variant)
        echo '  auto oops = rimewin::PlanVariant(true, 0x0804);' \
          >> "${tmp}/windows/service/main.cc" ;;
      bare_select_schema)
        echo '  rs_select_schema(sess, "luna_pinyin");' \
          >> "${tmp}/windows/service/engine.cc" ;;
      no_select_and_apply)
        sed -i 's/^bool Engine::SelectAndApply(/bool Engine::SelectAndApplyX(/' \
          "${tmp}/windows/service/engine.cc" ;;
      # 這一個就是覆核實際做過的那個植入:把重讀設定檔那一行刪掉。
      # 在規則 3 存在之前,做完這件事三支守門全綠。
      no_variant_reread)
        sed -i '/PickVariantPrefForSchemaSwitch(/d' \
          "${tmp}/windows/service/pipe_server.cc" ;;
      # 順序反了:先換方案、才重讀偏好。SelectAndApply 在換方案的當下就
      # 拿 variant_pref_ 重套了,所以晚一步 = 沒更新,而它看起來完全正確。
      variant_reread_too_late)
        sed -i 's/^\([[:space:]]*\)const VariantPrefPick pick = PickVariantPrefForSchemaSwitch(/\1Result r0 = engine_->SelectSchema(sc.session, sc.schema_id);\n\1const VariantPrefPick pick = PickVariantPrefForSchemaSwitch(/' \
          "${tmp}/windows/service/pipe_server.cc" ;;

      # ── 規則 4 的三個(都是覆核實際做過的植入)────────────────
      #
      # 三處失焦重置全部註解掉。這樣做完,三支守門在規則 4 存在之前
      # 全綠 —— 而使用者按著 Shift 切走再回來,第一次放開就誤切中英。
      shift_tap_no_reset)
        sed -i 's|^\([[:space:]]*\)shift_tap_\.Reset();|\1// shift_tap_.Reset();|' \
          "${tmp}/windows/tsf/text_service.cc" ;;
      # 兩處餵入全部註解掉 = 輕點 Shift 整個功能斷線,而且沒有任何
      # 守門會叫(語法完全正確,純函式測試照樣二十組全過)。
      shift_tap_no_feed)
        sed -i 's|^\([[:space:]]*\)shift_tap_\.OnKey(|\1// shift_tap_.OnKey(|' \
          "${tmp}/windows/tsf/text_service.cc" ;;
      # 只數呼叫點的守門會被這一個繞開:餵照餵,答案扔掉。
      shift_tap_drops_the_answer)
        sed -i 's|if (tap != ShiftTap::kToggleAsciiMode) return S_OK;|if (true) return S_OK;|' \
          "${tmp}/windows/tsf/text_service.cc" ;;

      # ── S4 的五個(狀態機看不到滑鼠那一組)────────────────────
      #
      # 這五個就是「修好之前」的樣子:三支守門在這一組存在之前全綠,
      # 而使用者按住 Shift 用滑鼠選一段字就會誤切一次中英。
      shift_tap_no_other_input)
        sed -i 's|^\([[:space:]]*\)shift_tap_\.OnOtherInput();|\1// shift_tap_.OnOtherInput();|' \
          "${tmp}/windows/tsf/text_service.cc" ;;
      # 只拆主要那條腿(選取變化)。第二條腿還在,所以「總數」那一條
      # 抓得到而「每個函式剛好 1 處」也抓得到 —— 兩層都要驗。
      shift_tap_no_other_input_on_end_edit)
        sed -i '/^STDMETHODIMP TextService::OnEndEdit(/,/^}/ { /shift_tap_\.OnOtherInput()/d; }' \
          "${tmp}/windows/tsf/text_service.cc" ;;
      # ⚠ 底下這三個是「編得過、一個測試都不紅、在每一個宿主裡都不生效」
      #   的那三格。它們是這一組裡最需要守門的,因為完全沒有其他訊號。
      shift_tap_no_edit_sink_base)
        sed -i 's/public ITfTextEditSink/public IUnknown/' \
          "${tmp}/windows/tsf/text_service.h" ;;
      shift_tap_no_edit_sink_qi)
        sed -i '/^STDMETHODIMP TextService::QueryInterface(/,/^}/ { /IID_ITfTextEditSink/d; /static_cast<ITfTextEditSink\*>(this)/d; }' \
          "${tmp}/windows/tsf/text_service.cc" ;;
      shift_tap_no_edit_sink_advise)
        sed -i '/AdviseSink(IID_ITfTextEditSink/d' \
          "${tmp}/windows/tsf/text_service.cc" ;;
      # ⚠ 這一個是覆核時實跑出來的**漏網**:介面繼承、QueryInterface、
      #   AdviseSink 那一行三格都在,只是**沒有人去呼叫掛接**。
      #   (定義留著,只註解掉有縮排的呼叫行。)補這一條之前:三支全綠。
      shift_tap_edit_sink_never_attached)
        sed -i -e 's|^\([[:space:]]\+\)WatchFocusedContext();|\1// WatchFocusedContext();|' \
               -e 's|^\([[:space:]]\+\)WatchContextOf(|\1// WatchContextOf(|' \
          "${tmp}/windows/tsf/text_service.cc" ;;

      # ── 規則 6 的四個(那一橫的訊號)───────────────────────────
      #
      # ⚠ 前兩個是這一輪那個缺陷的**兩個方向**,而且兩邊都真的發生過:
      #   少了 Start = 使用者這一次回報的(切回來那一橫不見了);
      #   少了 Stop  = 上一次回報的(#82:切走了還常駐)。
      #   兩個都是「編得過、九支純函式測試照樣全綠」。
      presence_no_start)
        python3 - "${tmp}/windows/tsf/text_service.cc" <<'PY_NOSTART'
import sys
p = sys.argv[1]
s = open(p, encoding='utf-8').read()
old = """  presence_ = PresenceLink::Start(
      static_cast<uint32_t>(::GetCurrentThreadId()));"""
assert old in s, '植入對不上 EnsurePresence 的寫法了 —— 反向測試會變成假綠'
open(p, 'w', encoding='utf-8').write(s.replace(old, '  presence_ = nullptr;', 1))
PY_NOSTART
        ;;
      presence_never_closed)
        sed -i '/^[[:space:]]*presence_->Stop();$/d' \
          "${tmp}/windows/tsf/text_service.cc" ;;
      # ⭐ 這一個就是 S4 本身:把兩個早退加回 OnActivated 的最頂端。
      #   加回去之後 Start / Stop 的次數一個都沒變、四個呼叫點也都還在,
      #   而使用者切到微軟拼音時那一橫照樣冒出來。只數次數的守門抓不到。
      profile_sink_drops_the_deactivate_edge)
        python3 - "${tmp}/windows/tsf/text_service.cc" <<'PY_EARLYRET'
import sys
p = sys.argv[1]
s = open(p, encoding='utf-8').read()
anchor = '  const bool ours = IsEqualCLSID(clsid, CLSID_RimeTextService);'
assert anchor in s, '植入對不上 OnActivated 的寫法了 —— 反向測試會變成假綠'
early = ('  if (!IsEqualCLSID(clsid, CLSID_RimeTextService)) return S_OK;\n'
         '  if (!(flags & TF_IPSINK_FLAG_ACTIVE)) return S_OK;\n')
open(p, 'w', encoding='utf-8').write(s.replace(anchor, early + anchor, 1))
PY_EARLYRET
        ;;
      presence_not_reopened_on_profile_switch)
        python3 - "${tmp}/windows/tsf/text_service.cc" <<'PY_NOREOPEN'
import re, sys
p = sys.argv[1]
s = open(p, encoding='utf-8').read()
i = s.index('STDMETHODIMP TextService::OnActivated(')
j = s.index('\n}\n', i)
body = s[i:j]
assert 'EnsurePresence();' in body, '植入對不上 —— 反向測試會變成假綠'
open(p, 'w', encoding='utf-8').write(
    s[:i] + body.replace('EnsurePresence();', '(void)0;', 1) + s[j:])
PY_NOREOPEN
        ;;
      presence_not_closed_on_profile_switch)
        python3 - "${tmp}/windows/tsf/text_service.cc" <<'PY_NOCLOSE'
import sys
p = sys.argv[1]
s = open(p, encoding='utf-8').read()
i = s.index('STDMETHODIMP TextService::OnActivated(')
j = s.index('\n}\n', i)
body = s[i:j]
assert 'ClosePresence();' in body, '植入對不上 —— 反向測試會變成假綠'
open(p, 'w', encoding='utf-8').write(
    s[:i] + body.replace('ClosePresence();', '(void)0;', 1) + s[j:])
PY_NOCLOSE
        ;;
      presence_never_says_who_it_is)
        sed -i 's|^\([[:space:]]*\)h\.host_tid = host_tid_;|\1h.host_pid = 0;|' \
          "${tmp}/windows/tsf/text_service.cc" ;;
      bar_takes_every_connection_as_a_vote)
        sed -i 's|if (bar_) bar_->OnClientIdentified(client_id, h.host_pid, h.host_tid);|(void)client_id;|' \
          "${tmp}/windows/service/pipe_server.cc" ;;
      bar_ignores_the_foreground)
        sed -i 's|      DecideBarOwner(snapshot, ReadForegroundOwner());|      BarOwnerDecision();|' \
          "${tmp}/windows/service/status_bar.cc" ;;
      bar_overwrites_state_when_os_cannot_answer)
        sed -i 's|  if (!owner.os_unknown) {|  if (true) {|' \
          "${tmp}/windows/service/status_bar.cc" ;;
      # 最像對、而且**不可以**的那個修法:直接在 ActivateEx 上連線。
      # 它會過上面兩條(Start / Stop 都還在),而症狀是每一次切輸入法
      # 都在宿主的 UI 執行緒上等一次開管道 + 握手。
      presence_ensure_ready_in_activate)
        sed -i 's|^\([[:space:]]*\)StartServiceInBackground(service_path_);|\1StartServiceInBackground(service_path_);\n\1ipc_.EnsureReady();|' \
          "${tmp}/windows/tsf/text_service.cc" ;;
      # 在場連線那條路自己去啟動服務 = 繞過提權宿主那道閘。
      presence_launches_service)
        echo 'static bool oops() { return rimewin::LaunchService(L"x"); }' \
          >> "${tmp}/windows/tsf/text_service.cc" ;;

      # ── 規則 5 的兩個 ──────────────────────────────────────────
      #
      # 兩份偏好的**來源**互換。型別擋得住引數對調,擋不住「包錯來源」——
      # 這一版照樣編得過,而它就是 648c02c 的原缺陷本身。
      variant_pick_swapped_sources)
        python3 - "${tmp}/windows/service/pipe_server.cc" <<'PY_SWAP'
import sys
p = sys.argv[1]
s = open(p, encoding='utf-8').read()
old = """            settings_ ? OnDiskPref::FromSettingsFile(
                            settings_->Load().SchemaPref())
                      : OnDiskPref::Unreadable(),
            EngineCopyPref(engine_->VariantPrefCopy()));"""
new = """            OnDiskPref::FromSettingsFile(engine_->VariantPrefCopy()),
            EngineCopyPref(settings_ ? settings_->Load().SchemaPref()
                                     : SchemaPreference()));"""
assert old in s, '植入對不上呼叫端的寫法了 —— 反向測試會變成假綠'
open(p, 'w', encoding='utf-8').write(s.replace(old, new))
PY_SWAP
        ;;
      # 舊簽章裡的 `settings_readable 永遠傳 false`,翻成新簽章的樣子:
      # 永遠說設定檔讀不到 = 永遠拿引擎手上那份過期的複本。
      variant_pick_settings_always_unreadable)
        python3 - "${tmp}/windows/service/pipe_server.cc" <<'PY_UNREADABLE'
import sys
p = sys.argv[1]
s = open(p, encoding='utf-8').read()
old = """            settings_ ? OnDiskPref::FromSettingsFile(
                            settings_->Load().SchemaPref())
                      : OnDiskPref::Unreadable(),"""
new = """            OnDiskPref::Unreadable(),"""
assert old in s, '植入對不上呼叫端的寫法了 —— 反向測試會變成假綠'
open(p, 'w', encoding='utf-8').write(s.replace(old, new))
PY_UNREADABLE
        ;;
    esac
    if audit_root "${tmp}" >/dev/null 2>&1; then
      echo "!! 植入了 ${plant},稽核卻以 0 結束 —— 它不會紅" >&2
      fail=1
    else
      echo "  ok   植入 ${plant} 之後稽核紅了"
    fi
    rm -rf "${tmp}"
  done
  # 現況必須是綠的(否則上面的「紅」證明不了任何事)。
  if ! audit_root "${ROOT_DEFAULT}"; then
    echo "!! 現況就是紅的 —— 反向測試不成立" >&2
    fail=1
  else
    echo "  ok   現況是綠的"
  fi
  exit "${fail}"
fi

audit_root "${ROOT_DEFAULT}"
