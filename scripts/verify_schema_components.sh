#!/usr/bin/env bash
#
# verify_schema_components.sh — 打包進某一端的方案，它用到的元件必須在
#                               **那一端的二進位檔裡**問得到
#
# ═══════════════════════════════════════════════════════════════════════════
#  這一關是工單 #109 換來的
# ═══════════════════════════════════════════════════════════════════════════
#
# `core/data/schemas/luminakey_charset.custom.yaml:28` 有這麼一行：
#
#     "engine/filters/@before last": lua_filter@*luminakey_charset
#
# `scripts/collect_charset_guard.sh` 把它**原封不動**複製成三份
# （luna_pinyin / bopomofo / t9_pinyin 的 `.custom.yaml`），四端共用同一個
# 位元組；Windows 的 `make_installer.sh` 用 `cp -r` 整棵搬、`.iss` 用
# `data\*` 遞迴打包 —— 所以它**確實裝進了使用者的機器**。
#
# 而 Windows 的 librime 沒有編 librime-lua，`lua_filter` 這個元件在
# Registry 裡不存在。librime 對此的處置是：
#
#     src/rime/engine.cc:310-315
#     auto c = T::Require(ticket.klass);
#     if (!c) { LOG(ERROR) << "error creating " << component_type
#                          << ": '" << ticket.klass << "'"; continue; }
#
# —— **只記一行錯、把那個 filter 跳過**。於是：安裝程式編得出來、簽章正確、
# 安裝成功、服務起得來、輸入法切得過去、候選也出得來，每一關都是綠的，
# 而字集守門那一層在使用者機器上根本不存在。狀態列寫著「繁」，輸出裡卻混得進
# 不屬於 Big5 的字。**沒有任何一個現有關卡看得見這件事。**
#
# ── 為什麼是問二進位檔，而不是維護一份清單 ────────────────────────────────
#
# 「這個平台有哪些元件」寫成一張手寫表，會在下一次有人改建置選項時安靜地
# 過期，而過期的方向正好是綠燈。所以這裡不猜：`rime_console --has-component`
# 直接對**要出貨的那一支二進位檔**呼叫 `rime::Registry::instance().Find()`
# （librime src/rime/registry.h:20，是 RIME_DLL 匯出的 API）。
# 答案由那個檔案給，不由原始碼給。
#
# 同一支腳本、各端各自指向自己的 console 二進位檔 —— 「各端行為一致」因此
# 從一句話變成一個機械檢查。
#
# ═══════════════════════════════════════════════════════════════════════════
#  ⚠ 2026-08-16:這支腳本目前**沒有任何呼叫者**
# ═══════════════════════════════════════════════════════════════════════════
#
# 它唯一的呼叫點是 windows/make_installer.sh(:167 與 :254),而 Windows 那條
# 線已經收掉、windows/ 從樹上移除。腳本本身刻意保留:它的判準與平台無關,
# --platform 只是缺口清冊的 id 前綴,接到任何一端都成立。
#
# 要接到 Android,缺的不是這支腳本而是一支**主機上跑得動的** rime_console:
# 現在 scripts/run_console_test.sh 是用 NDK clang 編出 Android ABI 的二進位檔
# 再 adb push 到模擬器上跑,而下面是直接在本機 exec "${CONSOLE}"。兩條路:
#   甲、在 build.yml 的慢車道用 adb 把 --has-component 問到裝置上(要改這支);
#   乙、另外編一支 host ABI 的 rime_console 專門回答這個問題。
# ⚠ 在接上之前,docs/settings-model.md 的那條規範**沒有機械檢查** ——
#   那一段已經照實改寫,不要讓它繼續宣稱有人在守。
#
# ── 已知缺口怎麼放行 ──────────────────────────────────────────────────────
#
# 這道閘一加，Windows **現在就會紅**，因為我們確實在打包 lua_filter。
# 那正是它該做的事，但這一版還得發得出去。所以放行的形狀刻意**不是**一個
# 「預設關閉的開關」（那等於沒有閘），而是：
#
#     一份具名的缺口清冊，每一項都有工單、有到期日，而且會被**印出來**。
#
# 清冊是 scripts/lib/component_gaps.tsv，格式與 scripts/lib/known_gaps.tsv
# **逐欄相同**（docs/coordination.md 2026-08-13 那一則要求桌面端共用格式）。
# 三條規矩一起成立，缺一條這個機制就會爛掉：
#
#   1. 沒登記的缺口 → 紅。
#   2. 登記了但過期 → 紅（要延就改那個日期，那是一個看得見的動作）。
#   3. 登記了而**這一輪根本沒缺** → 紅。缺口補上的當下，豁免會立刻變紅
#      逼人把它刪掉；豁免不會爛在清冊裡。
#
# 再加一條 `--announce-in`：被放行的每一張單號，必須**逐字**出現在指定檔案
# 裡**會被執行、而且會被印出來**的那一行上。Windows 這一端指的是
# `windows/service/main.cc` 的那句 Say() —— 它會印進使用者的 service.log。
# 放行可以，靜默不行。
#
# ⚠ 「出現在檔案裡」**不夠**，而這不是理論上的顧慮：main.cc 的**註解**裡
#   本來就有三個 `#109`。覆核者實測過 —— 把那句 Say() 整句刪掉、註解一個
#   字不動，這道閘照樣 exit 0，還印「單號有在裡面說出來」。也就是說，
#   「這一版可以帶著『Windows 沒有 lua_filter、字集守門第二層整層是死的』
#   出貨」這件事，靠的就是這條規矩，而擔保它的是一行**註解**。
#   下一輪有人把那句 Say() 拿掉或改寫，打包、CI、所有守門全綠，而使用者
#   機器上那一橫寫著「繁」、輸出裡混著不屬於該字集的字，service.log 裡
#   一個字的解釋都沒有。
#
# ── 用法 ──────────────────────────────────────────────────────────────────
#
#   scripts/verify_schema_components.sh \
#       --data <資料目錄> --console <rime_console 路徑> --platform <平台名> \
#       [--gaps <tsv>] [--announce-in <檔案>]
#
#   scripts/verify_schema_components.sh --self-test
#       只驗抽取器自己（不需要二進位檔、不碰網路）：每一種 patch 鍵的形狀
#       各驗一次。漏掉任何一種形狀，那一行就會安靜地不被檢查 —— 而那是這
#       支腳本最可能的假綠來源。
#
#   結束碼：0 = 全部問得到（或全部有登記的缺口）；1 = 有問題。
#
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

red()  { printf '\033[1;31m[FAIL]\033[0m %s\n' "$*" >&2; }
warn() { printf '\033[1;33m[缺口]\033[0m %s\n' "$*"; }
ok()   { printf '  \033[1;32mok\033[0m   %s\n' "$*"; }
info() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }

DATA=""
CONSOLE=""
PLATFORM=""
GAPS="${ROOT}/scripts/lib/component_gaps.tsv"
ANNOUNCE=""
SELF_TEST=0

while [ $# -gt 0 ]; do
  case "$1" in
    --data)        DATA="${2:-}"; shift 2 ;;
    --console)     CONSOLE="${2:-}"; shift 2 ;;
    --platform)    PLATFORM="${2:-}"; shift 2 ;;
    --gaps)        GAPS="${2:-}"; shift 2 ;;
    --announce-in) ANNOUNCE="${2:-}"; shift 2 ;;
    --self-test)   SELF_TEST=1; shift ;;
    -h|--help)     sed -n '2,90p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *)             red "未知參數: $1"; exit 1 ;;
  esac
done

# ⚠ windows-latest 的 runner 上 python 不一定叫 `python3`。找不到就**明確
#   失敗**，不要安靜地跳過（與 windows/check_ui_spec.sh:43 同一個寫法）。
PY=""
for cand in python3 python py; do
  if command -v "${cand}" >/dev/null 2>&1; then PY="${cand}"; break; fi
done
if [ -z "${PY}" ]; then
  red "找不到 python（試過 python3 / python / py）—— 方案檔的抽取靠它"
  exit 1
fi

TMP="$(mktemp -d)"
cleanup() { rm -rf "${TMP}"; }
trap cleanup EXIT

# ─────────────────────────────────────────────────────────── 抽取器
#
# 從 *.schema.yaml / *.custom.yaml 抽出「元件處方」，逐行印
#     <檔案><TAB><行號><TAB><類別名><TAB><原文>
#
# 兩種來源都要收：
#   · 一般的 engine/{processors,segmentors,translators,filters} 清單
#   · patch 形式的鍵：`"engine/filters/@before last"`、`@next`、`@after X`、
#     `@before X`、`@first`、`@last`、`@0`…
#
# ⚠ 這個抽取器自己就是一個假綠來源：漏掉任何一種 patch 鍵形狀，那一行就
#   安靜地不會被檢查。所以 --self-test 逐種形狀各驗一次。
cat > "${TMP}/extract.py" <<'PYEOF'
import io, os, re, sys

# engine 底下的四種元件清單。路徑一旦落在它們底下，值就是元件處方。
RECIPE_PATH = re.compile(
    r'(?:^|/)engine/(?:processors|segmentors|translators|filters)(?:/|$)')

KEY = re.compile(r'^("(?:[^"\\]|\\.)*"|\'(?:[^\']|\'\')*\'|[^:#]+?)\s*:\s*(.*)$')


def strip_comment(s):
    """去掉行尾註解，但不要碰引號裡的 #。"""
    out = []
    q = None
    for i, c in enumerate(s):
        if q:
            out.append(c)
            if c == q:
                q = None
            continue
        if c in '"\'':
            q = c
            out.append(c)
            continue
        if c == '#' and (i == 0 or s[i - 1] in ' \t'):
            break
        out.append(c)
    return ''.join(out)


def unquote(k):
    k = k.strip()
    if len(k) >= 2 and k[0] == k[-1] and k[0] in '"\'':
        return k[1:-1]
    return k


def class_of(recipe):
    """處方 → 類別名。`lua_filter@*luminakey_charset` → `lua_filter`。"""
    r = unquote(recipe.strip())
    if not r or r in ('~', 'null'):
        return None
    # 行內註解可能已經被去掉了，這裡再保險一次。
    r = r.split('#')[0].strip()
    if not r:
        return None
    return r.split('@', 1)[0].strip()


def scan(path, rel):
    hits = []
    stack = []  # [(indent, key)]
    try:
        lines = io.open(path, encoding='utf-8', errors='replace').read().splitlines()
    except OSError as e:
        sys.stderr.write('讀不到 %s: %s\n' % (path, e))
        return None

    for lineno, raw in enumerate(lines, 1):
        line = strip_comment(raw.rstrip()).rstrip()
        if not line.strip():
            continue
        indent = len(line) - len(line.lstrip(' \t'))
        body = line.strip()

        if body.startswith('- ') or body == '-':
            joined = '/'.join(k for _, k in stack)
            if RECIPE_PATH.search(joined):
                item = body[2:].strip() if body.startswith('- ') else ''
                # `- key: value` 形式的清單項不是處方，跳過。
                if item and ':' not in item.split('@', 1)[0]:
                    c = class_of(item)
                    if c:
                        hits.append((rel, lineno, c, item))
            continue

        while stack and stack[-1][0] >= indent:
            stack.pop()

        m = KEY.match(body)
        if not m:
            continue
        key = unquote(m.group(1))
        value = m.group(2).strip()
        stack.append((indent, key))
        if not value:
            continue

        joined = '/'.join(k for _, k in stack)
        if not RECIPE_PATH.search(joined):
            continue
        if value.startswith('['):          # 行內流式清單
            inner = value.strip('[]')
            for part in inner.split(','):
                c = class_of(part)
                if c:
                    hits.append((rel, lineno, c, part.strip()))
        else:
            c = class_of(value)
            if c:
                hits.append((rel, lineno, c, value))
    return hits


def main():
    root = sys.argv[1]
    files = []
    for dirpath, _dirnames, filenames in os.walk(root):
        for fn in sorted(filenames):
            if fn.endswith('.schema.yaml') or fn.endswith('.custom.yaml'):
                files.append(os.path.join(dirpath, fn))
    files.sort()
    sys.stderr.write('scanned=%d\n' % len(files))
    if not files:
        return 0
    for f in files:
        rel = os.path.relpath(f, root)
        hits = scan(f, rel)
        if hits is None:
            return 2
        for rel_, lineno, cls, txt in hits:
            sys.stdout.write('%s\t%d\t%s\t%s\n' % (rel_, lineno, cls, txt))
    return 0


sys.exit(main())
PYEOF

# ─────────────────────────────────────────────────────────── 自我測試
#
# 抽取器認得每一種 patch 鍵的形狀嗎？（risks 第 7 條：驗一種就算的話，
# 沒被認出來的那幾種會安靜地不被檢查。）
self_test() {
  local fx="${TMP}/fixture"
  mkdir -p "${fx}"
  cat > "${fx}/probe.schema.yaml" <<'YEOF'
schema:
  schema_id: probe
engine:
  processors:
    - ascii_composer
    - express_editor
  segmentors:
    - matcher
  translators:
    - script_translator
    - table_translator@custom_phrase
  filters:
    - simplifier@zh_hans
    - uniquifier
speller:
  algebra:
    __patch:
      - pinyin:/abbreviation
YEOF
  cat > "${fx}/probe.custom.yaml" <<'YEOF'
patch:
  "engine/filters/@before last": shape_before_named
  "engine/filters/@next": shape_next
  "engine/filters/@after uniquifier": shape_after_named
  "engine/translators/@first": shape_first
  "engine/processors/@last": shape_last
  "engine/segmentors/@0": shape_index
  engine/filters/@before last: shape_unquoted
  "zh_hans/opencc_config": luminakey_t2s.json
  "engine/filters":
    - shape_whole_list
YEOF
  cat > "${fx}/inline.schema.yaml" <<'YEOF'
engine:
  filters: [shape_inline_a, shape_inline_b]
YEOF

  local out rc
  out="$("${PY}" "${TMP}/extract.py" "${fx}" 2>"${TMP}/st.err")"
  rc=$?
  if [ "${rc}" -ne 0 ]; then
    red "自我測試：抽取器以 ${rc} 結束"
    cat "${TMP}/st.err" >&2
    return 1
  fi

  local failed=0 want n
  # 每一種形狀都要被認出來。
  for want in shape_before_named shape_next shape_after_named shape_first \
              shape_last shape_index shape_unquoted shape_whole_list \
              shape_inline_a shape_inline_b \
              ascii_composer express_editor matcher script_translator \
              table_translator simplifier uniquifier; do
    # ⚠ 不用 `printf … | grep -q`：這棵樹在 pipefail 底下被 SIGPIPE(141) 咬過
    #   五次（命中反而變成失敗）。先存成變數再用 grep -c。
    n="$(printf '%s\n' "${out}" | grep -c -- "	${want}	" || true)"
    if [ "${n}" -lt 1 ]; then
      red "自我測試：抽取器沒認出處方形狀 '${want}' —— 那一行會安靜地不被檢查"
      failed=1
    fi
  done
  # 不該被當成元件的東西，一個都不准混進來。
  for want in luminakey_t2s.json pinyin:/abbreviation probe; do
    n="$(printf '%s\n' "${out}" | grep -c -- "	${want}	" || true)"
    if [ "${n}" -ne 0 ]; then
      red "自我測試：抽取器把 '${want}' 誤當成元件處方了"
      failed=1
    fi
  done

  if [ "${failed}" -ne 0 ]; then
    printf '%s\n' "${out}" | sed 's/^/    /' >&2
    return 1
  fi
  ok "抽取器認得 8 種 patch 鍵形狀、行內流式清單與一般清單，而且沒有誤收"
  return 0
}

if [ "${SELF_TEST}" -eq 1 ]; then
  info "verify_schema_components.sh --self-test"
  self_test || exit 1
  exit 0
fi

# ─────────────────────────────────────────────────────────── 參數檢查
[ -n "${DATA}" ]     || { red "缺 --data <資料目錄>"; exit 1; }
[ -n "${CONSOLE}" ]  || { red "缺 --console <rime_console 路徑>"; exit 1; }
[ -n "${PLATFORM}" ] || { red "缺 --platform <平台名>（缺口清冊的 id 前綴）"; exit 1; }
[ -d "${DATA}" ]     || { red "找不到資料目錄: ${DATA}"; exit 1; }
if [ ! -f "${CONSOLE}" ]; then
  red "找不到 ${CONSOLE}
  這道閘要問的是**要出貨的那一支二進位檔**有沒有那些元件。沒有它就沒有答案，
  而「沒有答案」不可以被當成「沒問題」——先把 rime_console 建出來。"
  exit 1
fi

info "方案元件檢查（平台 ${PLATFORM}）"

# ─────────────────────────────────────────────────────────── 抽取
RECIPES="${TMP}/recipes.tsv"
if ! "${PY}" "${TMP}/extract.py" "${DATA}" > "${RECIPES}" 2>"${TMP}/extract.err"; then
  red "抽取方案元件失敗："
  sed 's/^/    /' "${TMP}/extract.err" >&2
  exit 1
fi
SCANNED="$(sed -n 's/^scanned=//p' "${TMP}/extract.err" | tail -1)"
SCANNED="${SCANNED:-0}"
if [ "${SCANNED}" -lt 1 ]; then
  red "在 ${DATA} 底下一份 *.schema.yaml / *.custom.yaml 都沒掃到 ——
  這道閘等於沒有在檢查。資料目錄給錯了，或者資料根本沒被打包進來。"
  exit 1
fi

NAMES="$(cut -f3 "${RECIPES}" | sort -u | sed '/^$/d')"
if [ -z "${NAMES}" ]; then
  red "掃了 ${SCANNED} 份方案檔，卻一個元件處方都沒抽出來 ——
  抽取器壞了（或方案檔的形狀變了）。這種情況下的綠燈不算數。"
  exit 1
fi
N_NAMES="$(printf '%s\n' "${NAMES}" | wc -l | tr -d ' ')"
ok "掃了 ${SCANNED} 份方案檔，抽出 ${N_NAMES} 個不重複的元件"

# ─────────────────────────────────────────────────────────── 問二進位檔
#
# 資料目錄要餵給 librime，而在 Git Bash 底下傳 POSIX 路徑給原生執行檔會被
# MSYS 轉換一次；這裡先自己轉成 Windows 路徑，不要把邊界交給那層轉換。
winpath() {
  if command -v cygpath >/dev/null 2>&1; then cygpath -w "$1"; else printf '%s' "$1"; fi
}
SHARED_DIR="${DATA}"
[ -d "${DATA}/shared" ] && SHARED_DIR="${DATA}/shared"
USER_DIR="${TMP}/user"
mkdir -p "${USER_DIR}"

CSV="$(printf '%s\n' "${NAMES}" | paste -sd, -)"
ANSWER="${TMP}/answer.tsv"
"${CONSOLE}" --has-component "$(winpath "${SHARED_DIR}")" \
             "$(winpath "${USER_DIR}")" "${CSV}" \
             > "${ANSWER}" 2> "${TMP}/console.err"
CONSOLE_RC=$?
if [ "${CONSOLE_RC}" -ge 2 ]; then
  red "rime_console --has-component 問不出答案（結束碼 ${CONSOLE_RC}）："
  sed 's/^/    /' "${TMP}/console.err" >&2
  sed 's/^/    /' "${ANSWER}" >&2
  exit 1
fi

# 每一個問過的名字都必須有一行答案；少一行就是答案不完整，不可以當成 yes。
MISSING_NAMES=""
for n in ${NAMES}; do
  a="$(awk -F'\t' -v k="${n}" '$1==k{print $2; exit}' "${ANSWER}")"
  case "${a}" in
    yes) ;;
    no)  MISSING_NAMES="${MISSING_NAMES} ${n}" ;;
    *)   red "rime_console 對 '${n}' 沒有給出 yes/no（拿到 '${a}'）——
  答案不完整不可以被當成通過。"; exit 1 ;;
  esac
done

# ─────────────────────────────────────────────────────────── 缺口清冊
FAILED=0
GAPS_USED=""

gap_row() { awk -F'\t' -v want="$1" '$1==want{print; exit}' "${GAPS}" 2>/dev/null; }

if [ -n "${MISSING_NAMES}" ]; then
  for n in ${MISSING_NAMES}; do
    id="${PLATFORM}:${n}"
    row="$(gap_row "${id}")"
    # 這個元件被誰用到？逐條點名，錯誤訊息要能直接動手。
    where="$(awk -F'\t' -v k="${n}" '$3==k{printf "      %s:%s  %s\n", $1, $2, $4}' "${RECIPES}")"
    if [ -z "${row}" ]; then
      red "打包進去的方案用到 '${n}'，而 ${PLATFORM} 這一端的 librime 裡沒有這個元件：
${where}
  librime 對此的處置是只記一行錯就跳過（engine.cc:310-315 的 continue）——
  引擎照樣跑、候選照樣有，那一層卻整層不存在，而且沒有任何一關會紅。
  要嘛把那個元件編進這一端，要嘛不要把用到它的方案打包進來。
  真的要暫時放行，就在 ${GAPS} 登記 '${id}'（工單 + 到期日），
  它會被印出來，不會變成一個沒有人負責的綠燈。"
      FAILED=$((FAILED + 1))
      continue
    fi
    until="$(printf '%s' "${row}" | cut -f2)"
    ticket="$(printf '%s' "${row}" | cut -f3)"
    why="$(printf '%s' "${row}" | cut -f4)"
    if [ -z "${until}" ] || [ -z "${ticket}" ] || [ -z "${why}" ]; then
      red "${GAPS} 的 '${id}' 欄位不全（要 id/到期日/工單/說明，以 TAB 分隔）"
      FAILED=$((FAILED + 1))
      continue
    fi
    case "${until}" in
      [0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]) ;;
      *) red "${GAPS} 的 '${id}' 到期日不是 YYYY-MM-DD：${until}"
         FAILED=$((FAILED + 1)); continue ;;
    esac
    today="$(date -u +%F)"
    if [ "${today}" ">" "${until}" ]; then
      red "'${n}' 在 ${PLATFORM} 上仍然問不到，而 '${id}' 的豁免在 ${until} 就過期了（工單 ${ticket}）。
${where}
  要嘛把它做完，要嘛改 ${GAPS} 的到期日並在 commit 訊息裡說為什麼。
  不改而讓它一直放行，就是把「還沒做」變成永久狀態。"
      FAILED=$((FAILED + 1))
      continue
    fi
    GAPS_USED="${GAPS_USED} ${id}"
    warn "${PLATFORM} 這一端沒有 '${n}' —— 這是**已知缺口**，工單 ${ticket}，豁免到 ${until} 為止。
      ${why}
      用到它的地方：
${where}"
  done
fi

# 清冊不見了 = 這個機制整個不存在。不可以安靜地當成「沒有任何豁免」——
# 那在「什麼都不缺」的那一天看起來會是綠的，而它其實已經壞了。
if [ ! -f "${GAPS}" ]; then
  red "找不到缺口清冊 ${GAPS}
  沒有這份清單，被放行的缺口就沒有工單、沒有到期日，也不會被印出來。"
  exit 1
fi

# 登記了卻沒缺 = 那件事做完了，豁免必須跟著刪掉。
while IFS= read -r _l; do
  case "${_l}" in ''|'#'*) continue ;; esac
  _id="$(printf '%s' "${_l}" | cut -f1)"
  case "${_id}" in
    "${PLATFORM}:"*) ;;
    *) continue ;;                     # 別的平台的缺口不歸這一輪管
  esac
  case " ${GAPS_USED} " in
    *" ${_id} "*) ;;
    *) red "${GAPS} 登記著 '${_id}'，但這一輪 ${PLATFORM} 並不缺那個元件 ——
  那件事做完了嗎？做完了就把這一行刪掉。豁免留著不會有人再看第二眼，
  而下一次真的缺了它的時候，這道閘會直接放行。"
       FAILED=$((FAILED + 1)) ;;
  esac
done < "${GAPS}"

# ── 只留下「會被執行、而且會被印出來」的那些字 ───────────────────────────
#
# 兩步，順序不能顛倒：
#
#   1. 先把註解去掉。⚠ 這一步是整條規矩的重點：一句
#      `// Say("… #109 …")` 在第 2 步眼裡與真的那一行**一模一樣**。
#   2. 只留下輸出敘述（Say / Err / Log / printf / fprintf / puts）那些行，
#      以及緊接在它後面的字串字面值續行（訊息常常斷成好幾行）。
#
# ⚠ 行尾註解是整段砍掉的。字串裡真的出現 `//`（例如網址）時這會砍過頭 ——
#   而砍過頭的方向是**更嚴**（該說的話沒被算到 → 紅），不是更鬆。
#   這道閘寧可誤紅，也不可以誤綠：誤紅有人看得到，誤綠沒有。
announce_visible_text() {
  awk '
    {
      line = $0
      if (line ~ /^[[:space:]]*\/\//) next
      i = index(line, "//")
      if (i > 0) line = substr(line, 1, i - 1)
      if (emit) {
        if (line ~ /^[[:space:]]*"/) {
          print line
          if (line ~ /;[[:space:]]*$/) emit = 0
          next
        }
        emit = 0
      }
      if (line ~ /(Say|Err|Log|Trace|printf|fprintf|puts|fputs)[[:space:]]*\(/) {
        print line
        if (line !~ /;[[:space:]]*$/) emit = 1
      }
    }
  ' "$1"
}

# 放行可以，靜默不行：每一張被用到的單號都要逐字出現在**那一行**上。
if [ -n "${ANNOUNCE}" ]; then
  if [ ! -f "${ANNOUNCE}" ]; then
    red "--announce-in 指到的檔案不存在: ${ANNOUNCE}"
    FAILED=$((FAILED + 1))
  else
    # ⚠ 存成檔案再 grep，不用 printf 接管線：這棵樹在 set -o pipefail 底下
    #   被 SIGPIPE（141）咬過五次。
    ANNOUNCE_VISIBLE="$(mktemp)"
    announce_visible_text "${ANNOUNCE}" > "${ANNOUNCE_VISIBLE}"
    if [ ! -s "${ANNOUNCE_VISIBLE}" ]; then
      # 抽不出東西 = 抽法對不上這個檔案了。這種情況下每一張單號都會
      # 「沒說出來」，而那個紅字指的是完全錯的方向 —— 明著講清楚。
      red "從 ${ANNOUNCE} 抽不出任何一行會被印出來的敘述。
  這道閘的抽法（announce_visible_text）對不上這個檔案了，不是「沒有說出來」。"
      FAILED=$((FAILED + 1))
    else
      for id in ${GAPS_USED}; do
        t="$(gap_row "${id}" | cut -f3)"
        c="$(grep -c -- "${t}" "${ANNOUNCE_VISIBLE}" || true)"
        if [ "${c}" -lt 1 ]; then
          red "缺口 '${id}' 被放行了，而它的單號 ${t} 在 ${ANNOUNCE} 裡
  **沒有出現在任何一行會被印出來的敘述上**（註解不算 —— 註解不會進
  使用者的 service.log）。
  放行的缺口必須在使用者看得到的地方說出來。
  不說出來的放行，與沒有這道閘沒有分別。"
          FAILED=$((FAILED + 1))
        else
          ok "缺口 '${id}' 的單號 ${t} 有在 ${ANNOUNCE} 會被印出來的那一行上"
        fi
      done
    fi
    rm -f "${ANNOUNCE_VISIBLE}"
  fi
fi

if [ "${FAILED}" -ne 0 ]; then
  red "方案元件檢查沒過（${FAILED} 項）"
  exit 1
fi

if [ -n "${GAPS_USED}" ]; then
  info "方案元件檢查通過 ✓（${N_NAMES} 個元件，其中有登記在案的已知缺口 —— 見上）"
else
  ok "${N_NAMES} 個元件，${PLATFORM} 的二進位檔全部問得到"
fi
exit 0
