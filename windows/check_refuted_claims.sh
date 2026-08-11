#!/usr/bin/env bash
#
# windows/check_refuted_claims.sh — 已推翻的主張不可以在樹上以現況陳述留著
#
# ══ 為什麼有這一支 ═════════════════════════════════════════════════
#
# 一句沒有出處的推測(「TSF 不把純修飾鍵交給 key event sink,所以修 Shift
# 要掛低階鍵盤 hook」)被抄進八個地方,在 docs/product-gaps.md 裡長成一個
# **排序判準**,再被 docs/coordination.md 引用成跨端結論。2026-08-12 在真的
# TSF 上量掉了它,那一輪自己說「六處全部改了」—— 而那是假的,§4.3 與 §5.3
# 兩處一個字都沒動,兩處都正好是排序判準。
#
# 也就是說:推翻之後靠人掃一次,掃不乾淨。
#
# ⚠ **這一支刻意不是通用守門。** 通用版(「宣稱平台行為的註解必須附出處」)
#   上一輪量過:windows/ 底下 10012 行註解裡命中 105 行,約 95% 誤報,所以
#   沒有寫 —— 而這個專案有兩個案例說明會亂叫的守門會被關掉。這一支只追
#   docs/refuted-claims.tsv 裡**逐字登記過**的那幾句,集合封閉,誤報率是 0。
#   它的天花板是**漏報**:推翻了卻忘記登記就看不到,而那一步沒有守門。
#
# ══ 判準 ═══════════════════════════════════════════════════════════
#
# 樹上每一個命中,前後 ANNOT_WINDOW 行內必須有一行帶更正記號。沒有就紅,
# 而且指名 檔案:行號 與那一行的內容。
#
# ⚠ **不要用刪除讓它變綠。** 規矩是保留原句 + 標成已推翻 + 附出處
#   (docs/product-gaps.md §4.1.1 是做對的樣子)。
#
# 用法:
#   windows/check_refuted_claims.sh              # 掃整棵樹
#   windows/check_refuted_claims.sh --self-check # 證明它會紅,也證明它會變綠
#   windows/check_refuted_claims.sh --root DIR   # 掃別的目錄(自我檢查用)
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
REGISTRY="${REPO_ROOT}/docs/refuted-claims.tsv"

SCAN_ROOT="${REPO_ROOT}"
SELF_CHECK=0
while [ $# -gt 0 ]; do
  case "$1" in
    --self-check) SELF_CHECK=1; shift ;;
    --root) SCAN_ROOT="$2"; shift 2 ;;
    *) echo "未知的參數:$1" >&2; exit 2 ;;
  esac
done

scan() {
  REGISTRY="${REGISTRY}" SCAN_ROOT="$1" PYTHONIOENCODING=utf-8 python3 - <<'PY'
import io, os, re, sys

# ⚠ Windows runner 上 python 的 stdout 預設是 **cp1252**,而這支印的每一行
#   都有中文 —— 第一個命中就會在 print 那裡 UnicodeEncodeError 炸掉。
#   炸掉的樣子很難看:整支非零結束,所以 --self-check 看到「紅了」,
#   但 out1.txt 裡一個字都沒有,於是它報「紅了,但沒有指名 RC-001」。
#   也就是**守門紅得對不對本身驗不到**。實測 CI run 31539475582 的
#   logic-x64 第 10 步(這支腳本第一次上 CI 就是這樣紅的)。
#   外面已經給了 PYTHONIOENCODING,這裡再補一層:兩者任一生效即可,
#   而 reconfigure 對「已經被別的東西設過 encoding」的情況也有效。
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8")
    except Exception:
        pass

REGISTRY = os.environ["REGISTRY"]
ROOT = os.environ["SCAN_ROOT"]

# 前後幾行算「附近」。§4.1.1 那種寫法(原句一段、更正另起一段)最遠約 8 行,
# 取 14 留餘裕 —— 這個數字寬一點只會讓漏報多一點,不會製造誤報。
ANNOT_WINDOW = 14

# 一行帶了這些其中之一,就算是「這裡已經標成推翻了」。
#
# ⚠ 這張清單是**跑出來的,不是想出來的**。第一版只有「更正 / 推翻 / 是假的 /
#   已過期 / 舊的論證」,跑全樹當場叫了兩處,而兩處其實都已經標對了,
#   只是用了別的詞:common/shift_tap.h 寫「一句假話」「量掉了它」,
#   tests/tsf_host_main.cc 寫「已經答過一次了 —— 收得到」。
#   兩處都補進來了。**清單要靠實跑長出來** —— 憑空多加詞只會讓漏報變多。
ANNOT = re.compile(
    "更正|推翻|是假的|假話|不成立|已過期|舊的論證|舊的說法|舊版的論證|"
    "實測|量掉|量到|收得到|答過|~~"
)

SKIP_DIRS = {".git", "third_party", "build", "node_modules", ".gradle", "out"}
# 只讀文字檔。副檔名白名單比 heuristics 可靠(二進位檔進來只會噪音)。
EXTS = {".md", ".txt", ".tsv", ".sh", ".py", ".cc", ".h", ".hpp", ".kt",
        ".java", ".swift", ".yml", ".yaml", ".rc", ".iss", ".env"}

rules = []
with io.open(REGISTRY, encoding="utf-8") as f:
    for ln, line in enumerate(f, 1):
        line = line.rstrip("\n")
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) < 3:
            sys.stderr.write("registry:%d 欄位不足(要 id/pattern/說明,用 TAB 分)\n" % ln)
            sys.exit(2)
        rid, pat, note = parts[0].strip(), parts[1], "\t".join(parts[2:]).strip()
        try:
            rules.append((rid, re.compile(pat), note, pat))
        except re.error as e:
            sys.stderr.write("registry:%d %s 的 pattern 編不起來:%s\n" % (ln, rid, e))
            sys.exit(2)

if not rules:
    sys.stderr.write("registry 裡一條規則都沒有 —— 這支等於沒在守。\n")
    sys.exit(2)

reg_abs = os.path.abspath(REGISTRY)
bad = []
hits_total = 0
scanned = 0

for dirpath, dirnames, filenames in os.walk(ROOT):
    dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
    for fn in filenames:
        p = os.path.join(dirpath, fn)
        # registry 自己列的就是那些錯句子,不掃它。
        if os.path.abspath(p) == reg_abs:
            continue
        # 這支腳本自己的檔頭也引用了那些句子。
        if os.path.basename(p) == "check_refuted_claims.sh":
            continue
        if os.path.splitext(fn)[1] not in EXTS:
            continue
        try:
            lines = io.open(p, encoding="utf-8").read().split("\n")
        except (UnicodeDecodeError, OSError):
            continue
        scanned += 1
        for i, text in enumerate(lines):
            for rid, rx, note, pat in rules:
                if not rx.search(text):
                    continue
                hits_total += 1
                lo = max(0, i - ANNOT_WINDOW)
                hi = min(len(lines), i + ANNOT_WINDOW + 1)
                if any(ANNOT.search(lines[j]) for j in range(lo, hi)):
                    continue
                bad.append((rid, os.path.relpath(p, ROOT), i + 1,
                            text.strip()[:150], note))

print("掃了 %d 個檔案、%d 條登記過的已推翻主張,命中 %d 行。"
      % (scanned, len(rules), hits_total))

if bad:
    print("")
    print("✗ 下面這些地方以**現況陳述**留著一句已經被推翻的主張,")
    print("  而且前後 %d 行內沒有任何更正記號:" % ANNOT_WINDOW)
    for rid, rel, ln, text, note in bad:
        print("")
        print("  %s  %s:%d" % (rid, rel, ln))
        print("      %s" % text)
        print("      為什麼是錯的:%s" % note)
    print("")
    print("  ⚠ 修法是**加註**,不是刪句子:保留原句 + 標成已推翻 + 附出處")
    print("    (docs/product-gaps.md §4.1.1 是做對的樣子)。")
    print("  ⚠ 如果這一處其實是在**引用**那句話並說明它為什麼錯,")
    print("    把更正記號寫進附近那 %d 行內就好。" % ANNOT_WINDOW)
    sys.exit(1)

print("✓ 每一個命中都在更正的射程內。")
PY
}

if [ "${SELF_CHECK}" = "1" ]; then
  # ⚠ 自我檢查要證明**兩個方向**:沒有更正記號會紅、補上就綠。
  #   只證前者的話,一支永遠紅的守門也會通過自我檢查 —— 而永遠紅的守門會被關掉。
  tmp="$(mktemp -d)"
  trap 'rm -rf "${tmp}"' EXIT

  echo "==> 自我檢查 1/2:植入一句沒有更正記號的已推翻主張,必須紅"
  printf '%s\n' \
    '這裡隨便寫幾行。' \
    '修 Shift 要掛低階鍵盤 hook,所以我們決定先做別的。' \
    '後面也隨便寫幾行。' > "${tmp}/planted.md"
  if scan "${tmp}" > "${tmp}/out1.txt" 2>&1; then
    echo "✗ 自我檢查失敗:植入了沒有更正記號的 RC-001,它卻是綠的。"
    cat "${tmp}/out1.txt"
    exit 1
  fi
  grep -q "RC-001" "${tmp}/out1.txt" || {
    echo "✗ 自我檢查失敗:紅了,但沒有指名 RC-001。"; cat "${tmp}/out1.txt"; exit 1; }
  grep -q "planted.md:2" "${tmp}/out1.txt" || {
    echo "✗ 自我檢查失敗:紅了,但沒有指名 planted.md:2。"; cat "${tmp}/out1.txt"; exit 1; }
  echo "    ✓ 紅了,而且指名了 RC-001 planted.md:2"

  echo "==> 自我檢查 2/2:同一句加上更正記號,必須綠"
  printf '%s\n' \
    '這裡隨便寫幾行。' \
    '修 Shift 要掛低階鍵盤 hook,所以我們決定先做別的。' \
    '⚠ [2026-08-12 更正]上面那一句是假的,已實測推翻(出處:CI run 31511075812)。' \
    '後面也隨便寫幾行。' > "${tmp}/planted.md"
  if scan "${tmp}" > "${tmp}/out2.txt" 2>&1; then
    echo "    ✓ 綠了"
  else
    echo "✗ 自我檢查失敗:加了更正記號還是紅的 —— 這支會變成永遠紅的守門。"
    cat "${tmp}/out2.txt"
    exit 1
  fi

  echo "==> 自我檢查通過(兩個方向都證了)"
  exit 0
fi

scan "${SCAN_ROOT}"
