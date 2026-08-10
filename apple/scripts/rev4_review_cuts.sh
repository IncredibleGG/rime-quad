#!/usr/bin/env bash
#
# rev4_review_cuts.sh — 覆核者自己的植入,**刻意不用實作者登記過的那 30 格**
#
# 為什麼要另外寫一份:run_kit_tests.sh 的 30 格證明的是
# 「**這 30 刀**砍下去會紅」。覆核者要問的是另一個問題 ——
# 換幾刀沒有被登記過的,守門還說不說得出話。
#
# 三種拆法各至少一刀(交接文件要求的那三種):
#   核心行為改壞 / 資料流切斷 / 呼叫點刪掉但定義留著
#
# ⚠ 替換一律交給 python,不經 bash 的字串展開 —— run_kit_tests.sh 的檔頭
#   記了 `$` 與 `|` 兩個坑,這裡直接繞開。
#
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PKG="${ROOT}/apple/LuminaKey"
cd "${PKG}" || exit 1

log() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }

# 與 run_kit_tests.sh 同一個比對器(兩個平台的失敗行格式不同)。
group_red_pattern() { printf 'error: (-\[[A-Za-z0-9_]+\.)?%s[ .]' "$1"; }
group_went_red() { grep -qE -- "$(group_red_pattern "$1")" <<< "$2"; }

# 自我測試:比對器認得兩種格式、不把通過算成紅、不把別組的紅算到自己頭上。
{
  d="/a/F.swift:1: error: -[LuminaKeyKitTests.KeyMapperTests testX] : XCTAssertEqual failed:"
  l="/a/F.swift:1: error: KeyMapperTests.testX : XCTAssertEqual failed:"
  p="Test Case '-[LuminaKeyKitTests.KeyMapperTests testX]' passed (0.001 seconds)"
  o="/a/F.swift:9: error: -[LuminaKeyKitTests.ThemeParserTests testY] : failed"
  bad=0
  group_went_red KeyMapperTests "$d" || bad=1
  group_went_red KeyMapperTests "$l" || bad=1
  group_went_red KeyMapperTests "$p" && bad=1
  group_went_red KeyMapperTests "$o" && bad=1
  [ "$bad" -eq 0 ] || { echo "!! 比對器自我測試失敗" >&2; exit 1; }
  echo "  ✓ 比對器自我測試通過"
}

# ---------------------------------------------------------------- 正向
log "先確認未植入的樹是綠的"
OUT="$(swift test 2>&1)"; RC=$?
if [ "${RC}" -ne 0 ]; then
  echo "!! 未植入就已經是紅的,底下每一刀都不算數" >&2
  grep -E "error:|XCTAssert" <<< "${OUT}" | sed -n '1,40p' >&2
  exit 1
fi
COUNT="$(awk '/Executed [0-9]+ test/ { n=$2 } END { print n+0 }' <<< "${OUT}")"
echo "  ✓ 基準線綠,已執行 ${COUNT} 項"

# ---------------------------------------------------------------- 植入
# 格式:代號 :: 類別 :: 檔案 :: 原字串 :: 替換字串 :: 應該紅的那一組
CUTS_FILE="$(mktemp)"
cat > "${CUTS_FILE}" <<'CUTS'
A::呼叫點刪掉、定義留著::Sources/LuminaKeyKit/KeyMapper.swift::guard ModifierGate.shouldForward(keyCode: keyCode, flags: flags) else { return nil }::_ = ModifierGate.shouldForward(keyCode: keyCode, flags: flags)::ModifierGateTests
B::核心行為改壞::Sources/LuminaKeyKit/InputModeSwitch.swift::public static let blockingFlags: MacModifierFlags = [.command, .control, .option]::public static let blockingFlags: MacModifierFlags = [.command, .control, .option, .capsLock]::ModifierGateTests
C::資料流切斷::Sources/LuminaKeyKit/KeyMapper.swift::defer { lastFlags = flags }::defer { if ModifierGate.shouldForward(keyCode: keyCode, flags: flags) { lastFlags = flags } }::ModifierGateTests
D::核心行為改壞::Sources/LuminaKeyKit/EffectiveSchemaList.swift::if !patched.isEmpty { return EffectiveSchemaList(ids: patched, source: .custom) }::if !patched.isEmpty { return EffectiveSchemaList(ids: installedIds.map { have in patched.filter { one in have.contains(one) } } ?? patched, source: .custom) }::EffectiveSchemaListTests
E::呼叫點刪掉、定義留著::Sources/LuminaKeyKit/EffectiveSchemaList.swift::return resolve(customText: custom, baseText: base, installedIds: installed)::return resolve(customText: custom, baseText: base)::EffectiveSchemaListTests
F::核心行為改壞::Tests/LuminaKeyKitTests/SwiftSourceScanner.swift::return SwiftSource.bracedBlock(in: text, from: r.upperBound)::return text::SwiftSourceTests
G::呼叫點刪掉、定義留著::AppSources/LuminaKeyInputController.swift::guard let snap = engine.snapshot() else { return }
        deliver(snap, to: sender)
    }

    private func selectCandidate::guard let snap = engine.snapshot() else { return }
        _ = snap
    }

    private func selectCandidate::PagingWiringTests
H::資料流切斷::AppSources/CandidateView.swift::if step != .none { onChangePage?(step) }::if step != .none { onChangePage?(.next) }::PagingWiringTests
CUTS

BACKUP="$(mktemp -d)"
restore() {
  for f in "${BACKUP}"/*.bak; do
    [ -e "$f" ] || continue
    cp "$f" "${PKG}/$(cat "${f%.bak}.path")"
  done
  rm -rf "${BACKUP}"
}
trap restore EXIT INT TERM

fails=0
i=0
# 用 python 逐筆讀,避免 bash 對 :: 分隔的多行欄位動手腳。
python3 - "${CUTS_FILE}" > /tmp/cuts.tsv <<'PY'
import sys, json
raw = open(sys.argv[1], encoding="utf-8").read()
# 每一筆以行首的「代號::」開頭
import re
starts = [m.start() for m in re.finditer(r'(?m)^[A-Z]::', raw)]
starts.append(len(raw))
for a, b in zip(starts, starts[1:]):
    rec = raw[a:b].rstrip("\n")
    parts = rec.split("::")
    assert len(parts) == 6, parts[:2]
    print("\t".join(json.dumps(p) for p in parts))
PY

while IFS=$'\t' read -r jid jkind jfile jfrom jto jgroup; do
  i=$((i + 1))
  id="$(python3 -c 'import json,sys;print(json.loads(sys.argv[1]))' "$jid")"
  kind="$(python3 -c 'import json,sys;print(json.loads(sys.argv[1]))' "$jkind")"
  file="$(python3 -c 'import json,sys;print(json.loads(sys.argv[1]))' "$jfile")"
  group="$(python3 -c 'import json,sys;print(json.loads(sys.argv[1]))' "$jgroup")"

  log "刀 ${id}(${kind}): ${file} —— 應該打紅 ${group}"

  cp "${PKG}/${file}" "${BACKUP}/c${i}.bak"
  printf '%s' "${file}" > "${BACKUP}/c${i}.path"

  if ! python3 - "${PKG}/${file}" "$jfrom" "$jto" <<'PY'
import sys, json
path = sys.argv[1]
frm = json.loads(sys.argv[2])
to = json.loads(sys.argv[3])
s = open(path, encoding="utf-8").read()
if frm not in s:
    sys.stderr.write("!! 錨點對不上,這一刀不算數:\n%r\n" % frm)
    sys.exit(1)
open(path, "w", encoding="utf-8").write(s.replace(frm, to, 1))
PY
  then
    echo "  ✗ ${id}: 錨點對不上 —— **這一刀不算數**(不是守門的問題)" >&2
    fails=$((fails + 1))
    cp "${BACKUP}/c${i}.bak" "${PKG}/${file}"
    continue
  fi

  MOUT="$(swift test 2>&1)"; MRC=$?
  cp "${BACKUP}/c${i}.bak" "${PKG}/${file}"

  if [ "${MRC}" -eq 0 ]; then
    echo "  ✗ ${id}: **植入之後測試仍然全綠 —— ${group} 沒有牙齒**" >&2
    fails=$((fails + 1))
  elif ! group_went_red "${group}" "${MOUT}"; then
    echo "  ~ ${id}: 紅了,但不是 ${group}。實際紅的:" >&2
    grep -E "error:" <<< "${MOUT}" | sed -n '1,6p' >&2
    fails=$((fails + 1))
  else
    echo "  ✓ ${id}: ${group} 如預期變紅"
  fi
done < /tmp/cuts.tsv

echo
if [ "${fails}" -ne 0 ]; then
  echo "!! ${fails}/${i} 刀沒有被抓到(或紅錯地方)" >&2
  exit 1
fi
echo "覆核者的 ${i} 刀全部被抓到,而且紅在對的那一組 ✓"
