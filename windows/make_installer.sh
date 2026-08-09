#!/usr/bin/env bash
#
# windows/make_installer.sh — 產生 RimeQuad-Setup-x64.exe
#
# ── 這支腳本在做什麼 ──────────────────────────────────────────────
#
#   1. 把要裝到使用者機器上的東西擺成一棵樹(payload)
#   2. **逐項點名檢查那棵樹**  ← 這一步是重點,見下
#   3. 呼叫 Inno Setup 的 ISCC 把它壓成一個 .exe
#
# 打包工具選 Inno Setup 而不是 WiX 的理由寫在 windows/installer/rimequad.iss 檔頭。
#
# ── 為什麼第 2 步是重點 ───────────────────────────────────────────
#
# 這個專案最貴的失敗模式一直都是「每一步都成功,而使用者裝上去打不出字」。
# 安裝程式這一格的具體長相是:**忘了把 core/data/shared 包進去**。
# 那樣的話 —— 安裝程式編得出來、簽章正確、安裝成功、服務起得來、
# 輸入法出現在語言列上、切過去也有反應 —— 然後一個字都打不出來,
# 而且沒有任何錯誤訊息。
#
# 所以資料缺漏必須在**這裡**就炸掉,不可以出得了門。
# `--self-check` 會拿一棵故意缺東西的樹跑一次檢查,要求它真的紅。
#
# 用法(Git Bash):
#   windows/make_installer.sh              產生安裝程式
#   windows/make_installer.sh --self-check 只驗「檢查會紅」,不產生安裝程式
#
# 產出:
#   third_party/build/windows-x64/installer/RimeQuad-Setup-x64.exe
#
set -euo pipefail

log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[warn]\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[error]\033[0m %s\n' "$*" >&2; exit 1; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
ARCH="${ARCH:-x64}"
BUILD_ROOT="${ROOT}/third_party/build/windows-${ARCH}"
BIN="${BUILD_ROOT}/ime/bin"
WORK="${BUILD_ROOT}/installer"
PAYLOAD="${WORK}/payload"
OUT_NAME="RimeQuad-Setup-${ARCH}.exe"

SELF_CHECK=0
LINT=0
case "${1:-}" in
  --self-check) SELF_CHECK=1 ;;
  --lint)       LINT=1 ;;
  "")           ;;
  *)            die "未知參數: $1(用 --self-check 或 --lint)" ;;
esac

# ---------------------------------------------------------------- payload 檢查
#
# 「安裝程式編出來了」不等於「裝上去能用」。這裡逐項點名。
# 回傳非零 = 這棵樹不能出貨。
verify_payload() {
  local root="$1"
  local missing=0 f

  # 二進位四件。少 rime_ime_setup.exe 的話安裝程式會在註冊那一步失敗;
  # 少 rime_service.exe 的話輸入法註冊得上但每一顆鍵都不會有反應。
  #
  # ⚠ rime_console.exe 這一輪從「驗證工具」變成**要出貨的東西**。
  #   理由是分層診斷:它完全不經過 TSF、不經過管道,直接驅動 librime + 資料。
  #   使用者說「不能用」的時候,第一刀就是問它 —— 它打得出「你好」就代表
  #   引擎、詞庫、方案都是好的,問題必定在 TSF 或 IPC 那一側,反過來也一樣。
  #   `rime_ime_setup.exe doctor` 的第 7 節就是呼叫它。
  #   少了它,那一刀就切不下去,而我們又回到「來回好幾輪才問得出資訊」。
  for f in rime_tsf.dll rime_service.exe rime_ime_setup.exe rime_console.exe; do
    if [ -f "${root}/${f}" ]; then
      printf '    ✓ %s (%s bytes)\n' "${f}" "$(stat -c%s "${root}/${f}" 2>/dev/null || echo ?)"
    else
      printf '    !! 缺少 %s\n' "${f}" >&2; missing=1
    fi
  done

  # 執行期資料。**這一段就是本腳本存在的理由。**
  #
  # 清單與 scripts/collect_data.sh 的產出對齊:
  #   default.yaml            基礎配置;少了它 librime 部署直接失敗
  #   四個 .schema.yaml       default.custom.yaml 的 schema_list 列的那四個
  #   三個 .dict.yaml         那些方案實際引用的詞庫
  #   essay.txt               語言模型;少了它候選排序會爛掉但不會報錯
  #   opencc/*.ocd2           簡繁與臺灣字形轉換
  #   data/user/default.custom.yaml
  #                           把 schema_list 限縮成我們真的有詞庫的四個方案。
  #                           少了它,librime 會照上游 default.yaml 去部署
  #                           cangjie5 / quick5 等我們沒有的方案 —— 部署噴錯,
  #                           而使用者看到的是「有些方案切過去一個候選都沒有」。
  for f in \
    data/shared/default.yaml \
    data/shared/luna_pinyin_tw.schema.yaml \
    data/shared/bopomofo_tw.schema.yaml \
    data/shared/luna_pinyin.schema.yaml \
    data/shared/t9_pinyin.schema.yaml \
    data/shared/luna_pinyin.dict.yaml \
    data/shared/terra_pinyin.dict.yaml \
    data/shared/stroke.dict.yaml \
    data/shared/essay.txt \
    data/user/default.custom.yaml
  do
    if [ -f "${root}/${f}" ]; then
      printf '    ✓ %s\n' "${f}"
    else
      printf '    !! 缺少 %s\n' "${f}" >&2; missing=1
    fi
  done

  # `|| true`:一個都沒有時 ls 非零結束,配上 pipefail 會讓腳本在印出
  # 診斷訊息**之前**就死掉 —— 反而看不到真正的原因。
  local ocd2
  ocd2="$( (ls "${root}"/data/shared/opencc/*.ocd2 2>/dev/null || true) | wc -l | tr -d ' ')"
  if [ "${ocd2}" -gt 0 ]; then
    printf '    ✓ opencc: %s 個 .ocd2\n' "${ocd2}"
  else
    printf '    !! data/shared/opencc 底下沒有任何 .ocd2 —— 簡繁與臺灣字形轉換會失效\n' >&2
    missing=1
  fi

  return "${missing}"
}

# ---------------------------------------------------------------- 反向測試
#
# 「測試是綠的,因為它沒在測」是這個專案抓過最多次的失敗模式。
# 上面那個檢查若哪天被改壞(例如把清單寫成空的、或把回傳值吃掉),
# 它會在**資料真的缺席時**照樣印一片綠,而那正是它要擋的事。
# 所以這裡拿一棵故意殘缺的樹去跑它,要求它真的紅。
self_check() {
  local tmp="${WORK}/selfcheck"
  rm -rf "${tmp}"
  mkdir -p "${tmp}/data/shared/opencc" "${tmp}/data/user"

  log "反向測試 1/3:完全空的 payload"
  if verify_payload "${tmp}" > /dev/null 2>&1; then
    die "空的 payload 竟然通過檢查 —— 這道檢查沒有在檢查,上面的綠燈都不算數"
  fi
  log "  ✓ 紅了"

  # 造一棵「幾乎完整、只少了 default.custom.yaml」的樹。
  # 這一種最危險:它會裝得起來、跑得起來、部署也不會整個失敗,
  # 只是有些方案沒有候選。純粹靠人眼永遠不會發現。
  log "反向測試 2/3:只少了 data/user/default.custom.yaml"
  : > "${tmp}/rime_tsf.dll"
  : > "${tmp}/rime_service.exe"
  : > "${tmp}/rime_ime_setup.exe"
  local f
  for f in default.yaml luna_pinyin_tw.schema.yaml bopomofo_tw.schema.yaml \
           luna_pinyin.schema.yaml t9_pinyin.schema.yaml luna_pinyin.dict.yaml \
           terra_pinyin.dict.yaml stroke.dict.yaml essay.txt; do
    : > "${tmp}/data/shared/${f}"
  done
  : > "${tmp}/data/shared/opencc/t2s.ocd2"
  if verify_payload "${tmp}" > /dev/null 2>&1; then
    die "少了 default.custom.yaml 竟然通過檢查"
  fi
  log "  ✓ 紅了"

  log "反向測試 3/3:補齊之後必須轉綠(否則這道檢查是恆假的)"
  : > "${tmp}/data/user/default.custom.yaml"
  if ! verify_payload "${tmp}" > /dev/null 2>&1; then
    die "補齊之後仍然紅 —— 這道檢查恆假,同樣不算在檢查"
  fi
  log "  ✓ 綠了"

  rm -rf "${tmp}"
  log "反向測試通過:payload 檢查會在該紅的時候紅、該綠的時候綠 ✓"
}

mkdir -p "${WORK}"
if [ "${SELF_CHECK}" -eq 1 ]; then
  self_check
  exit 0
fi

# ---------------------------------------------------------------- 前置
command -v cygpath >/dev/null 2>&1 || die "必須在 Git Bash / MSYS2 下執行"
w() { cygpath -w "$1"; }

# --lint 只驗 .iss 本身,用的是假的 payload —— 它刻意要能在**沒有** librime
# 產物、也沒有執行期資料的快速 job 上跑。那正是它四分鐘就有答案的原因。
if [ "${LINT}" -eq 0 ]; then
[ -d "${BIN}" ] || die "找不到 ${BIN};先跑 windows/build.sh ime"
[ -d "${ROOT}/core/data/shared" ] \
  || die "缺少 core/data/shared。先跑 scripts/fetch_rime_data.sh 與 scripts/collect_data.sh
  少了它,安裝程式會裝出一個「註冊得上但一個字都打不出來」的輸入法。"
fi

# ---------------------------------------------------------------- 版本
#
# 由 HEAD 的 commit 時間推導,與 Android 端 versionCode 同精神(單調就好)。
#
# ⚠ VersionInfoVersion 的每一段上限是 **65535**,而這種「把日期塞進版本號」
#   的寫法很容易撞到它:yyMMdd 是六位數,yy+一年中的第幾天是五位數(最大
#   99366)—— 兩種都會溢出,而 ISCC 的錯誤訊息只會說值不合法,不會說原因。
#   所以用「距 2020-01-01 的天數」:今天大約 2400,一年加 365,
#   要一百多年才會滿。版本號只需要單調,不需要讀得出日期 ——
#   真正的身分是 AppVersion 裡帶的時間戳與 commit。
COMMIT_EPOCH="$(git -C "${ROOT}" log -1 --format=%ct 2>/dev/null || echo 0)"
if [ "${COMMIT_EPOCH}" = "0" ]; then
  warn "取不到 commit 時間(不是 git 工作區?),版本號退回 0.1.0.0"
  V3=0; V4=0; STAMP="unknown"
else
  V3="$(( (COMMIT_EPOCH - 1577836800) / 86400 ))"
  [ "${V3}" -ge 0 ] && [ "${V3}" -le 65535 ] || die "版本號第三段 ${V3} 超出 0..65535"
  V4="$((10#$(date -u -d "@${COMMIT_EPOCH}" +%H%M)))"
  STAMP="$(date -u -d "@${COMMIT_EPOCH}" +%Y%m%d-%H%M)"
fi
SHA="$(git -C "${ROOT}" rev-parse --short HEAD 2>/dev/null || echo nogit)"
VERSION_INFO="0.1.${V3}.${V4}"
APP_VERSION="0.1.0+${STAMP}.${SHA}"
log "版本: ${APP_VERSION}  (VersionInfoVersion=${VERSION_INFO})"

# ---------------------------------------------------------------- ISCC
find_iscc() {
  if [ -n "${ISCC:-}" ]; then echo "${ISCC}"; return; fi
  if command -v ISCC.exe >/dev/null 2>&1; then command -v ISCC.exe; return; fi
  local p
  for p in \
    "/c/Program Files (x86)/Inno Setup 6/ISCC.exe" \
    "/c/Program Files/Inno Setup 6/ISCC.exe" \
    "/c/ProgramData/chocolatey/lib/InnoSetup/tools/ISCC.exe" \
    "/c/ProgramData/Chocolatey/bin/ISCC.exe"
  do
    [ -x "${p}" ] && { echo "${p}"; return; }
  done
  echo ""
}

ISCC_EXE="$(find_iscc)"
[ -n "${ISCC_EXE}" ] || die "找不到 ISCC.exe(Inno Setup 的編譯器)。
  windows-latest runner 內建 Inno Setup 6;本機請自行安裝,或設 ISCC=<路徑>。"
log "ISCC = ${ISCC_EXE}"

# ── UTF-8 BOM ───────────────────────────────────────────────────
#
# ⚠ Inno Setup 6 只有在 .iss **帶 UTF-8 BOM** 時才會當成 UTF-8 讀,
#   否則走系統 ANSI 代碼頁。runner 是英文的(cp1252),沒有 BOM 的話
#   .iss 裡的中文會在**編譯階段**就變成亂碼 —— 而安裝程式照樣編得出來,
#   使用者看到的是一整個亂碼的安裝精靈。
#   版控裡那一份保持乾淨的 UTF-8(不帶 BOM,免得每個編輯器都要處理它),
#   BOM 在這裡加。
ISS_SRC="${SCRIPT_DIR}/installer/rimequad.iss"
ISS="${WORK}/rimequad.iss"
[ -f "${ISS_SRC}" ] || die "找不到 ${ISS_SRC}"
printf '\xEF\xBB\xBF' > "${ISS}"
cat "${ISS_SRC}" >> "${ISS}"
head -c 3 "${ISS}" | od -An -tx1 | tr -d ' \n' | grep -q 'efbbbf' \
  || die "BOM 沒有加上去 —— 中文會在編譯階段變成亂碼"

# ── 呼叫 ISCC ───────────────────────────────────────────────────
#
# ⚠ 參數前面的雙斜線是刻意的。MSYS/Git Bash 會把看起來像 POSIX 路徑的參數
#   (以 / 開頭)自動換成 Windows 路徑,`/DPayloadDir=…` 會被改得面目全非。
#   `//X` 會被還原成 `/X`。這是本倉庫既有的慣例(見 check_binaries.sh 的
#   `dumpbin.exe //exports` 與 build.sh 的 `cmd //c`)。
iscc_once() {
  # $1 = 架構指示詞, $2 = payload 目錄, $3 = 輸出目錄, $4 = 日誌
  "${ISCC_EXE}" //Qp \
    "//DPayloadDir=$(w "$2")" \
    "//DAppVersion=${APP_VERSION}" \
    "//DVersionInfo=${VERSION_INFO}" \
    "//DArchDirective=$1" \
    "//O$(w "$3")" \
    "$(w "${ISS}")" \
    > "$4" 2>&1
}

# ── x64 的指示詞在 Inno 6.3 改了名字 ────────────────────────────
#
# 6.3 起是 `x64compatible`,之前是 `x64`。兩者都寫死不得 ——
# 換一種 runner 映像就會掛,而錯誤訊息(「Unknown value」)完全看不出
# 這是版本差異。
#
# 刻意**不去解析 `ISCC /?` 的版本字串**:那個輸出的格式本身就不是穩定介面
# (協調端才被同一類問題咬過一次 —— apksigner 在兩台機器上印不一樣的字,
#  於是簽得完全正確的 APK 被判成沒有簽章者)。改成直接試,試不過換另一個。
compile_installer() {
  # $1 = payload 目錄, $2 = 輸出目錄, $3 = 日誌前綴
  local cand
  ARCH_DIRECTIVE=""
  for cand in x64compatible x64; do
    if iscc_once "${cand}" "$1" "$2" "$3-${cand}.log"; then
      ARCH_DIRECTIVE="${cand}"
      cp "$3-${cand}.log" "$3.log"
      log "ArchitecturesAllowed=${cand} ✓"
      return 0
    fi
    log "  ${cand} 不成立,換下一個"
  done
  for cand in x64compatible x64; do
    echo "--- $(basename "$3")-${cand}.log ---"
    tail -40 "$3-${cand}.log" 2>/dev/null || true
  done
  return 1
}

# ---------------------------------------------------------------- --lint
#
# 只驗「這份 .iss 編不編得過」,用一棵**假的** payload。
#
# 為什麼要有這一步:ISCC 是逐段解析的,前面一段語法錯了就中止,
# 後面的 [Messages] / [Code] 根本沒被看過。而真正的建置在 core-x64 的最後面 ——
# 一輪二十分鐘,一次只能發現一個錯。
# 這一步不需要 librime、不需要執行期資料,放在快速 job 裡四分鐘就有答案。
#
# 假的 payload 是刻意的:這裡驗的是**腳本本身**,內容對不對由 verify_payload
# 與 windows/verify_installer.sh 負責 —— 兩件事不要混在一起。
if [ "${LINT}" -eq 1 ]; then
  LINT_DIR="${WORK}/lint"
  rm -rf "${LINT_DIR}"
  mkdir -p "${LINT_DIR}/payload/data/shared/opencc" "${LINT_DIR}/payload/data/user" \
           "${LINT_DIR}/out"
  for f in rime_tsf.dll rime_service.exe rime_ime_setup.exe rime_console.exe; do
    : > "${LINT_DIR}/payload/${f}"
  done
  : > "${LINT_DIR}/payload/data/shared/default.yaml"
  : > "${LINT_DIR}/payload/data/shared/opencc/t2s.ocd2"
  : > "${LINT_DIR}/payload/data/user/default.custom.yaml"

  log "lint:用假的 payload 編一次,只驗 .iss 本身"
  compile_installer "${LINT_DIR}/payload" "${LINT_DIR}/out" "${WORK}/iscc-lint" \
    || die "安裝程式腳本編不過,見上。
  (這一步用的是假的 payload —— 錯的是 windows/installer/rimequad.iss 本身,
   不是要裝的內容。)"
  [ -f "${LINT_DIR}/out/${OUT_NAME}" ] \
    || die "lint 編過了卻沒有產生 ${OUT_NAME} —— OutputBaseFilename 或 //O 不對"
  log "安裝程式腳本編得過 ✓(產物已丟棄,那不是真的安裝程式)"
  rm -rf "${LINT_DIR}"
  exit 0
fi

# ---------------------------------------------------------------- payload
log "組裝 payload: ${PAYLOAD}"
rm -rf "${PAYLOAD}"
mkdir -p "${PAYLOAD}/data"

cp "${BIN}/rime_tsf.dll"       "${PAYLOAD}/"
cp "${BIN}/rime_service.exe"   "${PAYLOAD}/"
cp "${BIN}/rime_ime_setup.exe" "${PAYLOAD}/"

# rime_console.exe 建在 console/bin,不在 ime/bin(它是 build.sh console 那一步的
# 產物)。路徑寫死在這裡而不是猜:找不到就明確地死,不要靜靜地出一個
# 少了診斷工具的安裝包。
CONSOLE_EXE="${BUILD_ROOT}/console/bin/rime_console.exe"
[ -f "${CONSOLE_EXE}" ] || die "找不到 ${CONSOLE_EXE};先跑 windows/build.sh console
  它現在是要出貨的東西(使用者的分層自我診斷靠它),不再只是 CI 的驗證工具。"
cp "${CONSOLE_EXE}" "${PAYLOAD}/"

# rime_probe.exe / rime_tests.exe / rime_tsf_host.exe **刻意不裝**:
# 那些是驗證用的東西,不是使用者機器上該有的。
# 安裝目錄裡多一支執行檔就多一個要解釋的東西。
cp -r "${ROOT}/core/data/shared" "${PAYLOAD}/data/shared"
cp -r "${ROOT}/core/data/user"   "${PAYLOAD}/data/user"

log "檢查 payload(缺任何一項都不出貨)"
verify_payload "${PAYLOAD}" || die "payload 不完整,見上。
  這道檢查擋下的是本專案最貴的一種失敗:每一步都成功,而使用者裝上去打不出字。"
log "payload 檢查通過 ✓  ($(find "${PAYLOAD}" -type f | wc -l | tr -d ' ') 個檔案)"

rm -f "${WORK}/${OUT_NAME}"
log "編譯安裝程式"
compile_installer "${PAYLOAD}" "${WORK}" "${WORK}/iscc" \
  || die "ISCC 兩種架構指示詞都失敗,見上"

[ -f "${WORK}/${OUT_NAME}" ] || {
  tail -40 "${WORK}/iscc.log"
  die "ISCC 以 0 結束,但沒有產生 ${OUT_NAME} —— OutputBaseFilename 或 //O 不對"
}

log "完成 ✓  ${WORK}/${OUT_NAME} ($(stat -c%s "${WORK}/${OUT_NAME}" 2>/dev/null || echo ?) bytes)"
