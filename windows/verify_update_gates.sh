#!/usr/bin/env bash
#
# windows/verify_update_gates.sh — 線上更新那幾條規則的**反向測試**
#
# ══ 這支腳本為什麼存在 ═════════════════════════════════════════════
#
# 「改完把修正拿掉跑一次,確認會紅,再放回去」——這件事如果靠人記得做,
# 它就會在第二次之後停止發生。這裡把它自動化:複製一棵樹、**真的**把
# 規則改壞、重編、要求指定的那條測試變紅。
#
# ⚠ 每一條都同時斷言「紅了」**與**「紅在該紅的地方」。只斷言結束碼是不夠的:
#   一個打錯字造成的編譯失敗也是非零結束,而那什麼都沒有證明。
#
# 只編更新這條線需要的十個檔案,所以一輪幾秒鐘 —— 慢的守門會被關掉。
#
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
CXX="${CXX:-g++}"
WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

SRCS_REL=(
  common/net_policy.cc common/net_gate_core.cc common/ui_strings.cc
  common/sha256.cc common/update_manifest.cc common/update_flow.cc
  tests/test_main.cc tests/test_sha256.cc tests/test_update_manifest.cc
  tests/test_update_flow.cc
)

build_and_run() {  # $1 = 樹的根(windows/ 的複本);印出測試輸出,回傳結束碼
  local tree="$1" out="$1/rime_update_tests" i srcs=()
  for i in "${SRCS_REL[@]}"; do srcs+=("${tree}/${i}"); done
  if ! "${CXX}" -std=c++17 -O0 -w -I"${tree}/common" "${srcs[@]}" -o "${out}" \
       2> "${tree}/build.log"; then
    echo "BUILD_FAILED"
    sed 's/^/    /' "${tree}/build.log" | head -20
    return 99
  fi
  "${out}" 2>&1
}

fresh_tree() {  # 印出一棵乾淨複本的路徑
  local t; t="$(mktemp -d "${WORK}/tree.XXXXXX")"
  cp -r "${SCRIPT_DIR}/common" "${SCRIPT_DIR}/tests" "${t}/"
  echo "${t}"
}

fail=0
pass=0

# ⚠ **恆假防護**:沒有植入任何東西時必須是綠的。少了這一條,底下每一條
#   都可能是因為「反正一直都紅」而通過。
base_tree="$(fresh_tree)"
base_out="$(build_and_run "${base_tree}")"
if [ $? -ne 0 ]; then
  printf '\033[1;31m[FAIL]\033[0m 沒有植入任何東西,測試卻是紅的 —— 底下的反向測試全部不算數\n' >&2
  printf '%s\n' "${base_out}" | tail -20 >&2
  exit 1
fi
printf '  \033[1;32mok\033[0m   基準:沒有植入時是綠的(%s)\n' \
  "$(printf '%s' "${base_out}" | grep -o '共 [0-9]* 個斷言' | head -1)"

mutate() {  # 名稱 檔案 舊字串 新字串 期望變紅的測試名
  local name="$1" rel="$2" old="$3" new="$4" want="$5"
  local t; t="$(fresh_tree)"
  OLD="${old}" NEW="${new}" python3 - "${t}/${rel}" <<'PY'
import io, os, sys
p = sys.argv[1]
s = io.open(p, encoding="utf-8").read()
old, new = os.environ["OLD"], os.environ["NEW"]
if old not in s:
    sys.stderr.write("植入失敗:找不到要改的那一段\n")
    sys.exit(3)
io.open(p, "w", encoding="utf-8").write(s.replace(old, new, 1))
PY
  if [ $? -ne 0 ]; then
    printf '\033[1;31m  !! %s:植入不進去(程式碼改過了?)\033[0m\n' "${name}" >&2
    fail=$((fail + 1)); return
  fi
  local out rc
  out="$(build_and_run "${t}")"; rc=$?
  if [ "${rc}" -eq 0 ]; then
    printf '\033[1;31m  !! %s:植入之後測試竟然還是綠的 —— 這條規則沒有人在守\033[0m\n' \
           "${name}" >&2
    fail=$((fail + 1)); return
  fi
  if [ "${rc}" -eq 99 ]; then
    printf '\033[1;31m  !! %s:編不過。非零結束不算數,它什麼都沒有證明\033[0m\n' \
           "${name}" >&2
    printf '%s\n' "${out}" | head -8 >&2
    fail=$((fail + 1)); return
  fi
  if ! printf '%s' "${out}" | grep -q "FAIL ${want}"; then
    printf '\033[1;31m  !! %s:紅了,但不是紅在 %s —— 換了個地方壞掉不算守住\033[0m\n' \
           "${name}" "${want}" >&2
    printf '%s' "${out}" | grep '^FAIL' | head -5 >&2
    fail=$((fail + 1)); return
  fi
  printf '  \033[1;32mok\033[0m   %s → %s 變紅\n' "${name}" "${want}"
  pass=$((pass + 1))
}

echo "==> 逐條植入違規,要求指定的那條測試變紅"

mutate "交棒前不再檢查摘要" common/update_flow.cc \
  '  if (!pre.sha256_verified) {
    out = UpdateFailure::kSha256Mismatch;
    return false;
  }' '  // 植入:拿掉摘要那一關' \
  update_flow_never_hands_off_an_unverified_file

mutate "兩種失敗共用一句紅字" common/update_flow.cc \
  'case UpdateFailure::kFileLocked: return UiString::kUpdateErrFileLocked;' \
  'case UpdateFailure::kFileLocked: return UiString::kUpdateErrNotInstalled;' \
  update_flow_every_failure_says_something_different

mutate "和解時把兩種沒裝成併成一種" common/update_flow.cc \
  'out = queued_in_install_dir ? UpdateFailure::kFileLocked
                              : UpdateFailure::kNotInstalled;' \
  'out = UpdateFailure::kNotInstalled;' \
  update_flow_reconcile_tells_locked_apart_from_unknown

mutate "有一格不顯示信任錨" common/update_flow.cc \
  '    case UpdateStage::kReady:
      c.status = UiString::kUpdateStatusReady;' \
  '    case UpdateStage::kReady:
      c.trust = UiString::kUiStringCount;
      c.status = UiString::kUpdateStatusReady;' \
  update_card_always_carries_the_trust_anchor

mutate "沒驗過也給「現在更新」" common/update_flow.cc \
  'if (s.file_verified) c.action = UiString::kUpdateInstallNowButton;' \
  'c.action = UiString::kUpdateInstallNowButton;' \
  update_card_always_carries_the_trust_anchor

mutate "下載網址不先驗自己的 scheme" common/update_manifest.cc \
  'if (!LocationSchemeAllowed(raw_url)) {' 'if (false) {' \
  update_manifest_required_fields_reject_the_whole_thing

mutate "選用欄位格式不對就整份拒收" common/update_manifest.cc \
  '  const std::string app_id = Trim(TopString(root, "app_id"));
  if (LooksLikeAppId(app_id)) m.app_id = app_id;' \
  '  const std::string app_id = Trim(TopString(root, "app_id"));
  if (!app_id.empty() && !LooksLikeAppId(app_id)) {
    r.error = "app_id 格式不對";
    return r;
  }
  m.app_id = app_id;' \
  update_manifest_bad_optional_fields_are_treated_as_absent

# ⚠ 原本這裡植入的是「把 version_code 的 >0 檢查拿掉」,而**它不會紅** ——
#   因為 valid() 本來就是 version_code > 0,那個植入在語意上等價。
#   反向測試抓到的是我挑錯了要破壞的東西,不是測試有洞。換成真的會壞的:
#   本機不知道自己是誰時,若判成「換了產品」,所有還沒有 version.txt 的
#   舊安裝就**從此再也更新不了**,而且畫面上會叫他們去手動搬家。
mutate "本機不知道自己是誰時判成換了產品" common/update_manifest.cc \
  'if (Trim(remote).empty() || Trim(installed).empty()) return AppIdVerdict::kUnknown;' \
  'if (Trim(remote).empty()) return AppIdVerdict::kUnknown;' \
  update_manifest_app_id_shape_and_comparison

mutate "sha256 的常數表錯一個位元" common/sha256.cc \
  '0x428a2f98u' '0x428a2f99u' sha256_nist_vectors

mutate "兩邊都沒有摘要就算相符" common/sha256.cc \
  '  if (x.empty() || y.empty()) return false;' '  // 植入:空的也算' \
  sha256_compare_ignores_case_and_padding_but_not_content

echo
if [ "${fail}" -ne 0 ]; then
  printf '\033[1;31m反向測試:%d 條會紅,%d 條不會 —— 不會紅的那幾條等於沒有人在守\033[0m\n' \
         "${pass}" "${fail}" >&2
  exit 1
fi
printf '\033[1;32m反向測試全部通過:%d 條植入的違規都被指名抓到\033[0m\n' "${pass}"
