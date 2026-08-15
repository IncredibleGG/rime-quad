#!/usr/bin/env bash
#
# windows/make_installer.sh — 產生安裝程式(檔名見 product.env 的
#                             CI_ARTIFACT_WINDOWS_SETUP)
#
# ── 這支腳本在做什麼 ──────────────────────────────────────────────
#
#   1. 把要裝到使用者機器上的東西擺成一棵樹(payload)
#   2. **逐項點名檢查那棵樹**  ← 這一步是重點,見下
#   3. 呼叫 Inno Setup 的 ISCC 把它壓成一個 .exe
#
# 打包工具選 Inno Setup 而不是 WiX 的理由寫在 .iss 檔頭。
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
#   third_party/build/windows-x64/installer/<CI_ARTIFACT_WINDOWS_SETUP>.exe
#
set -euo pipefail

log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[warn]\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[error]\033[0m %s\n' "$*" >&2; exit 1; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
# 產品識別:值的唯一來源是 scripts/lib/product.env。
# ⚠ 這支腳本裡不准出現產品名的字面值 —— 見 windows/product_win.sh 檔頭。
# shellcheck disable=SC1091
. "${SCRIPT_DIR}/product_win.sh"

ARCH="${ARCH:-x64}"
BUILD_ROOT="${ROOT}/third_party/build/windows-${ARCH}"
BIN="${BUILD_ROOT}/ime/bin"
WORK="${BUILD_ROOT}/installer"
PAYLOAD="${WORK}/payload"
# ⚠ 產物名 = CI 的 artifact 名 = scripts/publish_desktop.sh 要抓的名字。
#   三者各寫一份的話,改名之後發布會**安靜地**抓不到東西(下載頁上那個
#   版本就是不會更新,而 CI 全綠)。所以一律從 product.env 來。
#   ${ARCH} 不參與:artifact 名本身已經帶了架構。
OUT_NAME="${RS_WIN_SETUP_EXE}"

# ── 方案元件檢查的三個參數 ──────────────────────────────────────
#
# 正式打包時一律用這組預設值：問 payload 裡那一支 rime_console.exe、
# 平台是 windows、缺口清冊是版控裡那一份。
# `--self-check` 會把它們指到暫時的替身（見 self_check），因為那棵樹裡的
# rime_console.exe 是一個空檔案，而它要驗的是**這道閘本身**會不會紅。
COMPONENT_CONSOLE=""          # 空 = 用 <payload>/rime_console.exe
COMPONENT_PLATFORM="windows"
COMPONENT_GAPS="${ROOT}/scripts/lib/component_gaps.tsv"
COMPONENT_ANNOUNCE="${SCRIPT_DIR}/service/main.cc"

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
  # ⚠ version.txt 進了這張清單:少了它,安裝出來的那一套**永遠查不到更新**
  #   (app 端會說「查不出你現在裝的是哪一版」)。那是一個安靜的失敗 ——
  #   輸入法完全正常,只是更新這條路從此不存在。
  for f in rime_tsf.dll rime_service.exe rime_ime_setup.exe rime_console.exe \
           version.txt; do
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

  # ── 打包進去的方案，它用到的元件必須在**這一端的二進位檔裡**問得到 ──
  #
  # ⚠ 這是本腳本檔頭那句「每一步都成功，而使用者裝上去打不出字」的第二種
  #   形狀，而且比第一種更難發現：東西**有**打包進去，只是引擎裡沒有那個
  #   元件。librime 的處置是只記一行錯就跳過（src/rime/engine.cc:310-315 的
  #   continue）——安裝程式編得出來、安裝成功、服務起得來、候選也出得來，
  #   只是那一層過濾整層不存在。工單 #109 就是這件事：我們把字集守門那一條
  #   `lua_filter` 處方打進了安裝包，而 Windows 的 librime 沒有編 librime-lua。
  #   （處方的原文與出處請看 scripts/lib/component_gaps.tsv；這個檔案裡不准
  #     出現產品識別碼的字面值，見 windows/product_win.sh 檔頭。）
  #
  # ⚠ 判準刻意**不是**一張手寫的「這個平台有哪些元件」清單 —— 那種清單會在
  #   下一次有人改建置選項時安靜地過期，而過期的方向剛好是綠燈。這裡是拿
  #   要出貨的那一支 rime_console.exe 去問 librime 的 Registry。
  #
  # 已知缺口怎麼放行（有工單、有到期日、會被印出來，而且不是一個開關），
  # 見 scripts/lib/component_gaps.tsv 的檔頭。
  local console="${COMPONENT_CONSOLE:-${root}/rime_console.exe}"
  printf '    -- 方案元件（問 %s）\n' "$(basename "${console}")"
  if ! "${ROOT}/scripts/verify_schema_components.sh" \
         --data "${root}/data" \
         --console "${console}" \
         --platform "${COMPONENT_PLATFORM}" \
         --gaps "${COMPONENT_GAPS}" \
         --announce-in "${COMPONENT_ANNOUNCE}"; then
    missing=1
  fi

  return "${missing}"
}

# ---------------------------------------------------------------- .iss 的區段標籤
#
# ⚠ ISCC 是**逐行**判斷區段標籤的,而且它會先去掉行首的空白。
#
# 也就是說 [Code] 裡一行縮排之後以 `[` 開頭 —— 例如把陣列參數斷行寫成
#
#       SuppressibleMsgBox(FmtMessage(CustomMessage('X'),
#                                     [SomeVar]), mbInformation, MB_OK, IDOK);
#
# —— 那個 `[SomeVar]),` 會被當成一個區段標籤,而錯誤訊息是
#   「PreprocessingError ... Invalid section tag」,**而且它指的行號還是別的地方**。
#
# 實測:CI run #62 就是這樣紅的。ISCC 只在 Windows 上跑得動,所以這個純文字
# 檢查是**唯一一個在 Linux 開發機上就抓得到它**的關卡 —— 那是它存在的理由。
# (--lint 那一步四分鐘,已經比二十分鐘的正式建置好很多;這一支是零秒。)
KNOWN_SECTIONS='Setup|Types|Components|Tasks|Dirs|Files|Icons|INI|InstallDelete|Languages|Messages|CustomMessages|LangOptions|Registry|Run|UninstallDelete|UninstallRun|Code'
check_iss_section_tags() {
  local iss="$1"
  local bad
  # 去掉行首空白之後以 [ 開頭、但不是已知區段名的行。
  bad="$(grep -nE '^[[:space:]]*\[' "${iss}" \
         | grep -vE "^[0-9]+:\[(${KNOWN_SECTIONS})\][[:space:]]*$" || true)"
  if [ -n "${bad}" ]; then
    echo "  !! .iss 裡有幾行去掉縮排之後以 [ 開頭,ISCC 會把它們當成區段標籤:" >&2
    printf '%s\n' "${bad}" | sed 's/^/     /' >&2
    echo "     把那個 [ 挪到不在行首的位置(例如先把值存進一個變數)。" >&2
    return 1
  fi
  return 0
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

  # ── 方案元件那道閘在自我測試裡要用替身 ──────────────────────────
  #
  # 這棵假樹裡的 rime_console.exe 是一個**空檔案**（下面那幾行 `: >` 造的），
  # 執行它只會拿到一個錯誤；而這裡要驗的是「payload 檢查會不會在該紅的時候
  # 紅」，不是 librime。所以換上一支只認得一份名單的假 console。
  # 平台名與缺口清冊也一起換掉：版控裡那份清冊登記著 windows:lua_filter，
  # 而這棵假樹根本沒有 lua_filter —— 用真的那一份，第 3 組「補齊之後必須
  # 轉綠」會因為「登記了卻沒缺」而紅在完全無關的地方。
  local stub="${tmp}/stub_console.sh"
  cat > "${stub}" <<'STUBEOF'
#!/usr/bin/env bash
# 假的 rime_console：只認得下面這份名單。用法與真的那一支相同：
#   $1 = --has-component  $2 = shared  $3 = user  $4 = 逗號分隔的元件名
KNOWN=",uniquifier,simplifier,script_translator,"
rc=0
IFS=',' read -r -a NS <<< "$4"
for n in "${NS[@]}"; do
  case "${KNOWN}" in
    *",${n},"*) printf '%s\tyes\n' "${n}" ;;
    *)          printf '%s\tno\n'  "${n}"; rc=1 ;;
  esac
done
exit "${rc}"
STUBEOF
  chmod +x "${stub}"
  COMPONENT_CONSOLE="${stub}"
  COMPONENT_PLATFORM="selfcheck"
  COMPONENT_GAPS="${tmp}/gaps.tsv"
  : > "${COMPONENT_GAPS}"

  # 抽取器自己的形狀測試（每一種 patch 鍵各驗一次）。漏掉任何一種，那一行
  # 就會安靜地不被檢查 —— 那是這道閘最可能的假綠來源。
  log "方案元件抽取器的自我測試"
  "${ROOT}/scripts/verify_schema_components.sh" --self-test \
    || die "抽取器的自我測試沒過,見上。這道閘要檢查的東西會漏掉。"

  log "反向測試 1/5:完全空的 payload"
  if verify_payload "${tmp}" > /dev/null 2>&1; then
    die "空的 payload 竟然通過檢查 —— 這道檢查沒有在檢查,上面的綠燈都不算數"
  fi
  log "  ✓ 紅了"

  # 造一棵「幾乎完整、只少了 default.custom.yaml」的樹。
  # 這一種最危險:它會裝得起來、跑得起來、部署也不會整個失敗,
  # 只是有些方案沒有候選。純粹靠人眼永遠不會發現。
  log "反向測試 2/5:只少了 data/user/default.custom.yaml"
  : > "${tmp}/rime_tsf.dll"
  : > "${tmp}/rime_service.exe"
  : > "${tmp}/rime_ime_setup.exe"
  # ⚠ 這一行忘了加的話,反向測試的第 3 步(「補齊之後必須轉綠」)會紅,
  #   而錯誤訊息是「這道檢查恆假」—— 指向完全錯的地方。
  #   實測:rime_console.exe 變成必需品的那一輪,CI 就紅在這裡。
  : > "${tmp}/rime_console.exe"
  : > "${tmp}/version.txt"
  local f
  for f in default.yaml luna_pinyin_tw.schema.yaml bopomofo_tw.schema.yaml \
           luna_pinyin.schema.yaml t9_pinyin.schema.yaml luna_pinyin.dict.yaml \
           terra_pinyin.dict.yaml stroke.dict.yaml essay.txt; do
    : > "${tmp}/data/shared/${f}"
  done
  # ⚠ 那幾份 .schema.yaml 不可以留成空的:方案元件那道閘在「一份方案檔都
  #   掃不到」與「掃到了卻抽不出任何元件」時都會紅(那兩種情況下的綠燈不算
  #   數),於是第 3 組「補齊之後必須轉綠」會紅在完全無關的地方。
  #   所以給它們一份**最小而真實**的 engine 區段,而假 console 認得 uniquifier。
  for f in luna_pinyin_tw.schema.yaml bopomofo_tw.schema.yaml \
           luna_pinyin.schema.yaml t9_pinyin.schema.yaml; do
    printf 'engine:\n  filters:\n    - uniquifier\n' > "${tmp}/data/shared/${f}"
  done
  : > "${tmp}/data/shared/opencc/t2s.ocd2"
  if verify_payload "${tmp}" > /dev/null 2>&1; then
    die "少了 default.custom.yaml 竟然通過檢查"
  fi
  log "  ✓ 紅了"

  log "反向測試 3/5:補齊之後必須轉綠(否則這道檢查是恆假的)"
  : > "${tmp}/data/user/default.custom.yaml"
  if ! verify_payload "${tmp}" > /dev/null 2>&1; then
    die "補齊之後仍然紅 —— 這道檢查恆假,同樣不算在檢查"
  fi
  log "  ✓ 綠了"

  # ⚠ 第 4 條是這一輪加的,而它擋的是一個**沒有人會回報**的失敗:
  #   少了 version.txt,輸入法完全正常 —— 只是它從此查不到更新
  #   (app 端會說「查不出你現在裝的是哪一版」)。沒有這一條的話,
  #   哪天有人把 write_version_txt 那一行拿掉,每一關都會是綠的。
  log "反向測試 4/5:只少了 version.txt(輸入法會正常,但永遠查不到更新)"
  rm -f "${tmp}/version.txt"
  if verify_payload "${tmp}" > /dev/null 2>&1; then
    die "少了 version.txt 竟然通過檢查 —— 那會出一個永遠更新不了的安裝包"
  fi
  log "  ✓ 紅了"
  : > "${tmp}/version.txt"

  # ⚠ 第 5 組是 #109 那一輪加的,而它擋的是**第二種**「每一步都成功而使用者
  #   打不出字」:東西有打包進去,只是引擎裡沒有那個元件。librime 只記一行錯
  #   就跳過(engine.cc:314 的 continue),所以現有的每一關都看不見它。
  #   四個子步驟一起才算數 —— 少了 5c,那份豁免清冊就會變成一個永久的綠燈。
  local probe="${tmp}/data/shared/probe.custom.yaml"
  log "反向測試 5/5 之 a:方案用到引擎裡沒有的元件 —— 必須紅"
  cat > "${probe}" <<'PROBEEOF'
patch:
  "engine/filters/@before last": this_component_does_not_exist
  "engine/translators/@next": this_component_does_not_exist
  "engine/processors/@after selector": this_component_does_not_exist
  "engine/segmentors/@first": this_component_does_not_exist
PROBEEOF
  if verify_payload "${tmp}" > /dev/null 2>&1; then
    die "方案裡寫著一個引擎裡沒有的元件,payload 檢查竟然通過 ——
  那正是 #109 的形狀:安裝程式編得出來、裝得起來,而那一層過濾整層不存在。"
  fi
  log "  ✓ 紅了"

  log "反向測試 5/5 之 b:登記成已知缺口(有工單、有到期日)—— 必須綠,而且要印出單號"
  printf 'selfcheck:this_component_does_not_exist\t2099-01-01\t#109\t自我測試用的假缺口\n' \
    > "${COMPONENT_GAPS}"
  local out5
  out5="$(verify_payload "${tmp}" 2>&1)" || {
    printf '%s\n' "${out5}" >&2
    die "登記過的已知缺口竟然還是紅的 —— 那樣這一版根本發不出去,而缺口也
  沒有任何一條路可以被具名地放行。"
  }
  # ⚠ 不用 `printf … | grep -q`:這棵樹在 pipefail 底下被 SIGPIPE(141) 咬過
  #   五次(命中反而變成失敗)。先存成變數再用 grep -c。
  local n5
  n5="$(printf '%s\n' "${out5}" | grep -c '#109' || true)"
  [ "${n5}" -ge 1 ] || die "缺口被放行了,而建置輸出裡一個字都沒說 ——
  「有單號、會被印出來」是這個放行機制的全部價值,靜默的放行等於沒有閘。"
  log "  ✓ 綠了,而且輸出裡有單號"

  log "反向測試 5/5 之 c:缺口補上了、豁免卻還留著 —— 必須紅"
  rm -f "${probe}"
  if verify_payload "${tmp}" > /dev/null 2>&1; then
    die "元件已經不缺了,豁免卻還留在清冊裡而檢查是綠的 ——
  那份清冊會就這樣爛在那裡,而下一次真的缺了它的時候這道閘會直接放行。"
  fi
  log "  ✓ 紅了"

  log "反向測試 5/5 之 d:把豁免也刪掉 —— 必須綠"
  : > "${COMPONENT_GAPS}"
  if ! verify_payload "${tmp}" > /dev/null 2>&1; then
    die "植入與豁免都拿掉之後仍然紅 —— 這道閘恆真,同樣不算在檢查"
  fi
  log "  ✓ 綠了"

  # ── ⚠ 5/5 之 e 與 f:「不靜默」到底綁在哪一行 ──────────────────
  #
  #   --announce-in 的意思是「被放行的缺口必須在使用者看得到的地方說
  #   出來」。舊的判準是「這個單號在檔案裡出現過」—— 而 main.cc 的
  #   **註解**裡本來就有三個同一張單號。覆核者實測:把那句 Say(...)
  #   整句刪掉、註解一個字不動,閘照樣 exit 0 並且印「有說出來」。
  #
  #   這很要命:這一版能帶著一個「守門整層是死的」缺口出貨,靠的就是
  #   清冊那條「放行可以,靜默不行」—— 而擔保「不靜默」的是一行註解。
  #
  #   所以反向測試必須**分得出這兩種情況**,而且兩個方向都要:
  #     e. 刪掉那句話、註解不動 → 必須紅
  #     f. 只動註解、那句話留著 → 必須綠(不然改寫註解會收到假紅字,
  #        而假紅字最後一定是被關掉,不是被修好)
  local ann_saved="${COMPONENT_ANNOUNCE}"
  cat > "${probe}" <<'PROBEEOF'
patch:
  "engine/filters/@before last": this_component_does_not_exist
PROBEEOF
  printf 'selfcheck:this_component_does_not_exist\t2099-01-01\t#109\t自我測試用的假缺口\n' \
    > "${COMPONENT_GAPS}"

  log "反向測試 5/5 之 e:把那句話整句刪掉、註解一個字不動 —— 必須紅"
  local ann_no_say="${tmp}/announce_no_say.cc"
  python3 - "${ann_saved}" "${ann_no_say}" <<'PY_NO_SAY'
import io, sys
src = io.open(sys.argv[1], encoding='utf-8').read().split('\n')
out, i, cut = [], 0, 0
while i < len(src):
    ln = src[i]
    if ln.lstrip().startswith('//') or 'Say(' not in ln or '#109' not in ln:
        out.append(ln)
        i += 1
        continue
    # 整個敘述丟掉:訊息常常斷成好幾行,只刪第一行會留下語意殘骸,
    # 而那不是這個反向測試要問的東西。
    while i < len(src):
        cut += 1
        done = src[i].rstrip().endswith(';')
        i += 1
        if done:
            break
assert cut > 0, '植入對不上 main.cc 那句 Say 的寫法了 —— 反向測試會變成假綠'
io.open(sys.argv[2], 'w', encoding='utf-8').write('\n'.join(out))
PY_NO_SAY
  COMPONENT_ANNOUNCE="${ann_no_say}"
  if verify_payload "${tmp}" > /dev/null 2>&1; then
    COMPONENT_ANNOUNCE="${ann_saved}"
    die "把那句 Say 整句刪掉(註解一個字不動)之後,這道閘竟然還是綠的 ——
  「放行可以,靜默不行」就只剩一行註解在擔保,而註解不會進 service.log。"
  fi
  log "  ✓ 紅了"

  log "反向測試 5/5 之 f:只把註解裡的單號改掉、那句話留著 —— 必須綠"
  local ann_no_comment="${tmp}/announce_no_comment.cc"
  python3 - "${ann_saved}" "${ann_no_comment}" <<'PY_NO_COMMENT'
import io, sys
src = io.open(sys.argv[1], encoding='utf-8').read().split('\n')
out, cut = [], 0
for ln in src:
    if ln.lstrip().startswith('//') and '#109' in ln:
        cut += 1
        out.append(ln.split('#109')[0] + '(這一輪把註解裡的單號改寫掉了)')
        continue
    out.append(ln)
assert cut > 0, '植入對不上 main.cc 註解裡那幾個單號了 —— 反向測試會變成假綠'
io.open(sys.argv[2], 'w', encoding='utf-8').write('\n'.join(out))
PY_NO_COMMENT
  COMPONENT_ANNOUNCE="${ann_no_comment}"
  if ! verify_payload "${tmp}" > /dev/null 2>&1; then
    COMPONENT_ANNOUNCE="${ann_saved}"
    die "只改了註解、那句 Say 一個字都沒動,這道閘卻紅了 —— 它綁錯了東西。
  下一次有人改寫註解就會收到一個指不到任何問題的紅字,而假紅字最後
  一定是被關掉,不是被修好。"
  fi
  COMPONENT_ANNOUNCE="${ann_saved}"
  log "  ✓ 綠了"

  rm -f "${probe}"
  : > "${COMPONENT_GAPS}"

  rm -rf "${tmp}"
  log "反向測試通過:payload 檢查會在該紅的時候紅、該綠的時候綠 ✓"

  # ⚠ 這一項與 payload 無關,但它掛在這裡是刻意的:--self-check 是本腳本
  #   唯一**不需要 Windows** 的入口,而這個檢查也不需要 Windows。
  #   掛在這裡,開發機上一行指令就驗得到。
  log "檢查 .iss 的區段標籤"
  check_iss_section_tags "${SCRIPT_DIR}/${RS_WIN_ISS_REL}" \
    || die ".iss 的區段標籤有問題,見上。ISCC 會以一個指向別處的行號失敗。"
  log "  ✓ 沒有會被誤認成區段標籤的行"

  # 反向測試的反向測試:植入一行,要求上面那個檢查真的紅。
  local probe="${WORK}/iss-probe.iss"
  cp "${SCRIPT_DIR}/${RS_WIN_ISS_REL}" "${probe}"
  printf '    [ThisLineLooksLikeASectionTag]\n' >> "${probe}"
  if check_iss_section_tags "${probe}" > /dev/null 2>&1; then
    die "植入了一行縮排的 [ ,區段標籤檢查竟然通過 —— 它沒有在檢查"
  fi
  rm -f "${probe}"
  log "  ✓ 植入一行縮排的 [ 之後它會紅"
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

# ── 線上更新用的版本號 ──────────────────────────────────────────
#
# ⚠ **單調就好,不必讀得出日期。** V3 是距 2020-01-01 的天數、V4 是 commit
#   時間的 HHMM(0..2359),所以 V3*10000+V4 隨 commit 時間嚴格遞增。
#   應用內更新只拿它比大小(common/update_manifest.h:version_code 是
#   **唯一**用來判斷新舊的欄位,version_name 只給人看)。
#
# ⚠ 取不到 commit 時間時它是 0,而 0 在 app 端等於「不知道自己是哪一版」——
#   那條路會**停用更新**並照實說,不會拿一個假的 0 去跟線上比大小
#   (見 ParseInstalledVersion 與 MayHandOff 的 kOwnVersionUnknown)。
VERSION_CODE="$(( V3 * 10000 + V4 ))"

# AppId 的唯一來源是 .iss。⚠ **不在這裡再寫一份** —— 兩份會漂移,
#   而漂移的症狀是「更新器以為產品換了身分,於是所有人都更新不了」。
#   撈不到就死:沒有退路,理由與 .iss 裡那幾個 #error 相同。
read_app_id() {  # $1 = .iss 路徑
  # Inno 的 `AppId={{GUID}` 裡,開頭那個 `{` 是跳脫;真正的值是 `{GUID}`。
  sed -n 's/^AppId={\({[0-9A-Fa-f-]\{36\}}\)$/\1/p' "$1" | head -1
}
APP_ID="$(read_app_id "${SCRIPT_DIR}/${RS_WIN_ISS_REL}")"
[ -n "${APP_ID}" ] || die "從 ${RS_WIN_ISS_REL} 撈不到 AppId。
  它是線上更新用來判斷「線上那一版還是不是同一個產品」的依據 ——
  撈不到就不能出貨,而不是寫一個猜的值進去。"

log "版本: ${APP_VERSION}  (VersionInfoVersion=${VERSION_INFO}, version_code=${VERSION_CODE})"
log "AppId: ${APP_ID}(從 ${RS_WIN_ISS_REL} 讀出來的)"

# 寫進 payload,由安裝程式放進安裝目錄。服務進程讀它來回答
# 「我是哪一版」。⚠ 為什麼是檔案而不是編進 exe 的版本資源:
# 版本號是**打包時**才算得出來的(它由 commit 時間推導),而執行檔在那之前
# 就編好了。要編進去就得讓每一次打包都重編一次,而重編出來的那一份
# 沒有被任何一關測過。
write_version_txt() {  # $1 = 目的地目錄
  cat > "$1/version.txt" <<VTXT
# ${RS_PRODUCT_NAME} —— 這一份是安裝時寫下的,給應用內更新讀。
# 由 windows/make_installer.sh 產生。手動改它只會讓更新判斷變成錯的。
version_code=${VERSION_CODE}
version_name=${APP_VERSION}
app_id=${APP_ID}
commit=${SHA}
VTXT
}

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
ISS_SRC="${SCRIPT_DIR}/${RS_WIN_ISS_REL}"
ISS="${WORK}/$(basename "${RS_WIN_ISS_REL}")"
[ -f "${ISS_SRC}" ] || die "找不到 ${ISS_SRC}"
check_iss_section_tags "${ISS_SRC}" \
  || die ".iss 的區段標籤有問題,見上。"
printf '\xEF\xBB\xBF' > "${ISS}"
cat "${ISS_SRC}" >> "${ISS}"
head -c 3 "${ISS}" | od -An -tx1 | tr -d ' \n' | grep -q 'efbbbf' \
  || die "BOM 沒有加上去 —— 中文會在編譯階段變成亂碼"

# ── 首次整理字詞要多久:值只有一個來源 ──────────────────────────
#
# 這個數字以前在四個地方各寫一份,其中**兩份是使用者讀得到的**,而那兩份
# 都寫著「一到數分鐘」——實測是十幾秒。見 windows/common/first_run_timing.h
# 的檔頭。那份標頭現在是唯一的來源;.iss 是 Pascal,include 不了 C++,
# 所以在這裡把數字讀出來,用 ISCC 的 /D 傳進去。
#
# ⚠ 讀不到就**死**。給一個預設值等於「傳參壞掉時安靜地用回一個寫死的
#   舊數字」,而那正是這件事本身。
# ⚠ 另一頭也是硬的:.iss 裡寫的是 `{#FirstDeployLowSec}`,ISPP 遇到未定義
#   的識別字是**當場失敗**,不是展開成空字串。
TIMING_H="${SCRIPT_DIR}/common/first_run_timing.h"
[ -f "${TIMING_H}" ] || die "找不到 ${TIMING_H} —— 完成頁那句話的數字沒有來源了"
read_timing() {   # read_timing <常數名>
  local v
  v="$(sed -n "s/^inline constexpr int $1 = \([0-9][0-9]*\);.*/\1/p" \
        "${TIMING_H}" | head -1)"
  [ -n "${v}" ] || die "從 ${TIMING_H} 讀不出 $1(名字改了?)"
  printf '%s' "${v}"
}
FIRST_DEPLOY_LOW="$(read_timing kFirstDeployTypicalLowSec)"
FIRST_DEPLOY_HIGH="$(read_timing kFirstDeployTypicalHighSec)"
PROFILE_VISIBLE="$(read_timing kProfileVisibleObservedSec)"
log "首次整理字詞 ${FIRST_DEPLOY_LOW}~${FIRST_DEPLOY_HIGH} 秒;"\
"清單可見約 ${PROFILE_VISIBLE} 秒(取自 common/first_run_timing.h)"

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
    "//DProductName=${RS_PRODUCT_NAME}" \
    "//DProductNameZh=${RS_PRODUCT_NAME_ZH}" \
    "//DProductIdRoot=${RS_PRODUCT_ID_ROOT}" \
    "//DSetupBaseName=${RS_WIN_SETUP_BASE}" \
    "//DFirstDeployLowSec=${FIRST_DEPLOY_LOW}" \
    "//DFirstDeployHighSec=${FIRST_DEPLOY_HIGH}" \
    "//DProfileVisibleSec=${PROFILE_VISIBLE}" \
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
  : > "${LINT_DIR}/payload/version.txt"
  : > "${LINT_DIR}/payload/data/shared/default.yaml"
  : > "${LINT_DIR}/payload/data/shared/opencc/t2s.ocd2"
  : > "${LINT_DIR}/payload/data/user/default.custom.yaml"

  log "lint:用假的 payload 編一次,只驗 .iss 本身"
  compile_installer "${LINT_DIR}/payload" "${LINT_DIR}/out" "${WORK}/iscc-lint" \
    || die "安裝程式腳本編不過,見上。
  (這一步用的是假的 payload —— 錯的是 ${RS_WIN_ISS_REL} 本身,
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

write_version_txt "${PAYLOAD}"

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

# 輸出目錄也放一份 version.txt。
#
# ⚠ 它**不是**給使用者的(使用者那一份在安裝程式裡面);它是給
#   scripts/publish_desktop.sh 讀的:線上版本資訊的 version_code 必須與
#   安裝進去的那一份**逐字相同**,否則使用者裝完之後會發現自己永遠
#   比線上舊(或永遠比線上新),而兩種都會讓更新這條路壞掉。
#   所以那個值只算一次,由這裡帶出去,發布端不重算。
cp "${PAYLOAD}/version.txt" "${WORK}/version.txt"

log "完成 ✓  ${WORK}/${OUT_NAME} ($(stat -c%s "${WORK}/${OUT_NAME}" 2>/dev/null || echo ?) bytes)"
