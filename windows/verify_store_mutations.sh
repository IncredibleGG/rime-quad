#!/usr/bin/env bash
#
# windows/verify_store_mutations.sh — 反向驗證方案市集那一組測試
#
# 「測試是綠的,因為它沒在測」這件事在這個專案發生過不只一次。這支腳本
# 把**修正拿掉**(在一棵複製出來的樹上植入違規),然後要求測試變紅。
# 每一條植入的都是一句真的會傷到使用者的行為改變,不是換個變數名。
#
# ⚠ 它沒有接進 run_logic_tests.sh:每一條都要重編一次整組測試,
#   十條就是十次編譯(這台機器上約三分鐘)。開發時每一輪都跑它不划算,
#   而它要抓的東西(有人把某一道防線拿掉)也不是每一輪都會發生。
#   **改 windows/common/ 底下市集那七個檔案的人請自己跑一次。**
#
#   windows/verify_store_mutations.sh
#
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

PASS=0
FAIL=0

red()  { printf '\033[1;31m[FAIL]\033[0m %s\n' "$*" >&2; FAIL=$((FAIL+1)); }
ok()   { printf '  \033[1;32mok\033[0m   %s\n' "$*"; PASS=$((PASS+1)); }
info() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }

# 先確認**沒有植入時是綠的**。這一步不能省:底下每一條都是
# 「它現在紅了」,而那句話只有在「本來是綠的」的前提下才有意義。
info "先跑一次沒有植入的版本(必須全綠)"
if ! "${SCRIPT_DIR}/run_logic_tests.sh" >/tmp/store-mut-base.log 2>&1; then
  printf '\033[1;31m[error]\033[0m 沒有植入就已經是紅的 —— 下面的反向測試全部沒有意義\n' >&2
  tail -20 /tmp/store-mut-base.log >&2
  exit 1
fi
ok "基準線是綠的"

# 一條植入 = 一個檔案 + 一段要被換掉的字串 + 換成什麼。
# 用 python 做精確字串取代(sed 對多行與特殊字元太脆弱)。
mutate_and_expect_red() {
  local name="$1" file="$2" old="$3" new="$4"
  local tmp
  tmp="$(mktemp -d)"
  cp -r "${ROOT}/windows" "${tmp}/windows"
  # 只要 core/include(rime_shell.h);core/ 其餘是幾百 MB 的詞典。
  mkdir -p "${tmp}/core"
  cp -r "${ROOT}/core/include" "${tmp}/core/include"

  if ! python3 - "${tmp}/windows/${file}" "${old}" "${new}" <<'PY'
import sys
path, old, new = sys.argv[1], sys.argv[2], sys.argv[3]
s = open(path, encoding='utf-8').read()
if old not in s:
    sys.stderr.write("植入點找不到:%s\n" % old[:80])
    sys.exit(2)
open(path, 'w', encoding='utf-8').write(s.replace(old, new, 1))
PY
  then
    red "${name}:植入點對不上原始碼(這條反向測試已經失效)"
    rm -rf "${tmp}"
    return
  fi

  if RIMEWIN_ROOT="${tmp}" "${tmp}/windows/run_logic_tests.sh" >"${tmp}/out.log" 2>&1; then
    red "${name}:植入了違規,測試卻是綠的"
    grep -E '跑了' "${tmp}/out.log" >&2 || true
  else
    local n
    n="$(grep -c '!!' "${tmp}/out.log" || true)"
    ok "${name} → 變紅(${n} 個失敗的斷言)"
  fi
  rm -rf "${tmp}"
}

# ── 1. 沒有 lua 卻放行 lua 方案 ────────────────────────────────
# 傷害:使用者裝了雾凇/魔然,部署成功,畫面上沒有任何錯誤,打不出任何字。
mutate_and_expect_red "lua 方案不再被擋下" "common/schema_preflight.cc" \
  '  if (!lua_supported) {' \
  '  if (false) {'

# ── 2. 解壓沒有硬上限 ─────────────────────────────────────────
# 傷害:宣告 1KB、實際灌 8GB 的 zip 炸彈把記憶體吃光。
mutate_and_expect_red "解壓的輸出上限失效" "common/zip_reader.cc" \
  '    if (static_cast<int64_t>(out_->size()) >= max_out_) {' \
  '    if (false) {'

# ── 3. 回溯距離不檢查 ─────────────────────────────────────────
# 傷害:讀到輸出緩衝區前面的記憶體(--asan 會叫,但這裡要的是它先變紅)。
mutate_and_expect_red "DEFLATE 的回溯距離不再檢查" "common/zip_reader.cc" \
  '      if (static_cast<int64_t>(dist) > static_cast<int64_t>(out_->size())) {' \
  '      if (dist < 0) {'

# ── 4. Win32 保留裝置名不擋 ───────────────────────────────────
# 傷害:套件裡一個叫 con.yaml 的檔案在 Win32 上會開到主控台裝置。
mutate_and_expect_red "保留裝置名不再被擋" "common/archive_guard.cc" \
  '      if (stem == r) {' \
  '      if (false && stem == r) {'

# ── 5. local header 與中央目錄不一致時照樣解 ──────────────────
# 傷害:檢查看中央目錄、解壓看 local header —— 繞過檢查的標準作法。
mutate_and_expect_red "兩份 zip 中繼資料不一致時不再擋" "common/zip_reader.cc" \
  '  if (b.compare(lo + 30, lname_len, e.name) != 0) {' \
  '  if (false) {'

# ── 6. sha256 對不上卻照樣安裝 ────────────────────────────────
# 傷害:一份下載到一半被截斷的詞典會落地,而之後**每一次**部署都失敗。
# ⚠ common/sha256.cc 是**線上更新那一條線**的檔案,市集只是用它。
#   植入點放在市集自己的呼叫端,免得那邊一改這條反向測試就失效。
mutate_and_expect_red "sha256 對不上卻照樣安裝" "common/store_engine.cc" \
  '    if (!Sha256HexEqual(got, pkg->sha256)) {' \
  '    if (false) {'

# ── 7. 預檢改到動完檔案之後才跑 ───────────────────────────────
# 傷害:缺一本詞典就會留下一份被改過的 default.custom.yaml。
mutate_and_expect_red "預檢不再擋下 schema_list 的修改" "common/store_engine.cc" \
  '    const std::vector<PreflightMissing> blocking = rep.Blocking();' \
  '    const std::vector<PreflightMissing> blocking;'

# ── 8. 開關關著與連不上壓成同一種失敗 ─────────────────────────
# 傷害:待辦 #62 本身 —— 使用者看到「連線失敗」,去檢查網路,
# 而真正的原因是他自己把開關關著(那應該給一顆開啟開關的按鈕)。
mutate_and_expect_red "開關關著被說成連線失敗" "common/store_engine.cc" \
  '        r.blocked ? StoreFailure::kSwitchOff : StoreFailure::kNetwork, r.message,' \
  '        StoreFailure::kNetwork, r.message,'

# ── 9. 循環相依被當成錯誤 ─────────────────────────────────────
# 傷害:luna-pinyin 與 stroke 互相 requires,當成錯誤 = 這兩個最常用的
# 套件永遠裝不了(Android 端踩過)。
mutate_and_expect_red "循環相依被當成錯誤" "common/store_index.cc" \
  '          result.plan.cycles.push_back(id);' \
  '          result.status = DependencyResult::Status::kUnknownPackage; result.missing = id; return false;'

# ── 10. 缺主詞典與缺次要詞典分不開 ────────────────────────────
# 傷害:Android 端那次事故 —— 98 個方案裡 20 個被自己的預檢擋死。
mutate_and_expect_red "所有缺檔一律當成擋下(那次事故本身)" "common/schema_preflight.cc" \
  '          p.top_key == "translator" ? PreflightSeverity::kBlocking' \
  '          true ? PreflightSeverity::kBlocking'

# ── 11. 部署失敗不回滾 ────────────────────────────────────────
mutate_and_expect_red "部署失敗後不把 schema_list 改回去" "common/store_engine.cc" \
  '    if (!deps.fs->WriteUserFile(kDefaultCustom, yaml)) {' \
  '    if (false) {'

echo
if [ "${FAIL}" -eq 0 ]; then
  printf '\033[1;32m==> %d 條植入全部變紅 —— 上面那些綠燈才算數\033[0m\n' "${PASS}"
  exit 0
fi
printf '\033[1;31m==> %d 條植入沒有被抓到\033[0m\n' "${FAIL}"
exit 1
