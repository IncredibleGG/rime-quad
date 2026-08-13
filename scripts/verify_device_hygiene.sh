#!/usr/bin/env bash
#
# verify_device_hygiene.sh — 「這支腳本會打在哪一台機器上」的**可重跑**守門
#
# ── 為什麼是一支腳本而不是一次 grep ────────────────────────────────────
# 這件事已經修過兩輪,每一輪的收尾都是「全樹再 grep 一次,沒有別的了」,
# 而每一輪都漏:
#
#   第一輪  漏了 `scripts/verify_ime.sh` —— 它 source 了 lib/device.sh、
#           把 `SERIAL` 改成空字串、加了 `--serial`,註解寫著「現在三個入口
#           都有」,而**全檔沒有一處呼叫 `rs_pick_serial`**。帶齊 RIME_SERIAL
#           跑起來:第 1 關過,然後 `adb -s "" shell` 讓 `set -e` 當場結束,
#           RC=1 而一個字都沒印。
#   第二輪  漏了 `scripts/verify_variant_persistence.sh` —— `SERIAL="emulator-5558"`
#           寫死,而它會 `adb -s "$SERIAL" shell rm -rf …`。
#
# 一次性的 grep 不會被下一個人重跑,而且「有沒有 source」與「有沒有真的用」
# 是兩件事。這一支把五條規則釘成會紅的斷言,跑得起來、跑得快、不需要裝置。
#
# 五條規則
# ─────────────────────────────────────────────────────────────────────
#   A. **不得寫死 `emulator-NNNN`**(註解與說明文字不算)。port 不是身分。
#   B. source 了 lib/device.sh 就**必須真的呼叫** `rs_pick_serial`,
#      而且必須把回傳值**接起來、後面真的用到**。
#   C. 會做破壞性動作(`pm clear` / `uninstall` / `ime set` / `ime enable` /
#      `settings put` / `shell rm -rf`)的檔案,必須呼叫 `rs_assert_destructive_ok`。
#   D. 用 adb 卻**不** source lib/device.sh 的檔案,必須列在下面的白名單裡
#      並附理由。
#   E. 白名單本身不得腐爛:列著的檔案必須存在、必須仍然用 adb、
#      而且必須仍然沒有 source lib/device.sh(補上了就要從白名單移除)。
#   F. 有 `--serial` 旗標的腳本**必須**走 `rs_select_device`。旗標只寫進
#      區域變數,閘看不到它 —— 帶了 `--serial` 反而保證失敗。
#
# ⚠ 掃描是**遞迴**的(`scripts/` 全樹)。從前寫死兩層,`scripts/schema_store/`
#   底下那一支會 `adb shell rm -rf` 的檔案從沒被掃過,而守門照樣報「全部通過」。
#
# 用法
#   scripts/verify_device_hygiene.sh              # 掃全樹
#   scripts/verify_device_hygiene.sh --self-test  # 反向驗證:五條各植一個違規
#
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

SELF_TEST=0
while [ $# -gt 0 ]; do
  case "$1" in
    --self-test) SELF_TEST=1; shift ;;
    -h|--help) sed -n '2,/^set -uo pipefail$/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "未知參數:$1" >&2; exit 2 ;;
  esac
done

export RS_SELF_TEST="$SELF_TEST"
python3 - "$ROOT" <<'PY'
import io, os, re, sys, tempfile, shutil

ROOT = sys.argv[1]
SELF_TEST = os.environ.get("RS_SELF_TEST") == "1"

# ── D 的白名單:用 adb 卻不 source lib/device.sh,而且**理由成立** ────────
#    ⚠ 新增一筆之前先問「它能不能改成走 rs_pick_serial」。
#      白名單是逃生口,不是預設。
ALLOW = {
    "scripts/emu.sh":
        "模擬器的**啟動者**。它有權決定 port(裝置還不存在時 rs_pick_serial "
        "問不到任何東西);而且已經有它自己的 fail-closed:有裝置在線時要求"
        "明著給 RIME_EMU_PORT,不猜。",
    "scripts/layout_drive.py":
        "自己實作了同一條 fail-closed(讀 RIME_SERIAL/ANDROID_SERIAL,"
        "沒有就中止並說出理由)。device.sh 是 bash,python 這一側 source 不進來。",
    "scripts/publish_apk.sh":
        "發布端,不碰裝置;`adb` 只出現在說明文字。",
    "scripts/verify_no_sigpipe_probe.sh":
        "純 host 端探針(驗 `logcat | grep -q` 的 SIGPIPE 行為),不連裝置。",
    "scripts/verify_product_ids.sh":
        "只讀 repo 裡的檔案比對 id;`adb` 出現在說明文字。",
    "scripts/lib/shared_data_writers.py":
        "只寫 host 端檔案,`adb` 出現在註解裡。",
    "scripts/verify_script_readonly.sh":
        "它**把 adb 換成 shim** —— 那正是它的斷言:唯讀路徑碰了外部工具就紅。"
        "shim 是 mktemp 出來的假腳本,一個位元組都不會送到真的裝置上。",
    "scripts/schema_store/verify.py":
        "`--serial` 是 **required=True** 的參數 —— 它自己就是 fail-closed:"
        "沒有人指名它連 argparse 都過不了,不可能落到「猜一台」。而破壞性動作"
        "(`rm -rf /data/local/tmp/rimestore`)的閘在唯一的呼叫端 "
        "`scripts/build_schema_store.sh`(rs_pick_serial ＋ rs_assert_destructive_ok)。"
        "device.sh 是 bash,python 這一側 source 不進來。",
}

# ⛔ 這裡從前是
#       SCAN_DIRS = [("scripts", ...), ("scripts/lib", ...)]
#    —— 兩層寫死,於是 `scripts/schema_store/`、`scripts/lua_sandbox/`、
#    `scripts/charset_guard/` 底下的檔案**一個都沒掃到**,而守門仍然報
#    「全樹掃描:全部通過」。實測:違規檔放進 `scripts/schema_store/` 完全抓不到。
#    真的漏掉的那一條:`scripts/schema_store/verify.py` 會
#    `adb shell rm -rf /data/local/tmp/rimestore`。
#    改成**遞迴**,新增子目錄自動納入。
SCAN_ROOT = "scripts"
SCAN_EXTS = (".sh", ".py")
SCAN_SKIP_DIRS = {"__pycache__"}
SKIP = {"scripts/lib/device.sh", "scripts/verify_device_hygiene.sh"}

ADB_USE = re.compile(r'(\$\{?ADB\}?|(?<![\w-])adb(?![\w-]))')
HARD_SERIAL = re.compile(r'emulator-\d+')
# ⚠ 規則 C 要的是**真的送出去的那一行**,不是「字串裡出現 adb」。
#   `verify_product_ids.sh` 有一行 `printf 'adb shell ime set …'` 在寫測試
#   資料 —— 把它判紅就是在教下一個人「這支守門會亂叫」,而會亂叫的守門
#   在第三次之後就沒有人看了。
ADB_CALL = re.compile(
    r'(\$\{?ADB\}?|(?<![\w./-])adb(?![\w-]))"?'
    r'(\s+-s\s+\S+)?\s+(shell|uninstall|install|exec-out|push|pull)\b')
QUOTED_DEMO = re.compile(r'^\s*(printf|echo|cat)\b')
DESTRUCTIVE = re.compile(
    r'\bpm\s+clear\b|\buninstall\b|\bime\s+set\b|\bime\s+enable\b'
    r'|\bsettings\s+put\b|\brm\s+-rf\b'
)

# ── A 的豁免:`emulator-NNNN` 是**測試資料**,不是要打的機器 ──────────────
#    ⚠ 只有一種理由成立:那個序號字串不會被送到真的 adb 上。
HARD_SERIAL_ALLOW = {
    "scripts/verify_device_lib.sh":
        "`lib/device.sh` 的行為測試。序號是餵給**假 adb**(mktemp 出來的腳本,"
        "`devices` 的輸出由 FAKE_DEVICES 決定)的測試資料 —— 要驗「指名的那一台"
        "不在線就中止」「--serial 與環境變數指到不同機器就中止」,就非得寫出"
        "具體的序號不可。全檔沒有一處碰真的 adb。",
}

# ── C 的豁免:自己就是「決定要打哪一台」的那一支 ────────────────────────
DESTRUCTIVE_ALLOW = {
    "scripts/emu.sh":
        "模擬器的啟動者兼唯一的 `ime-enable` 入口。它本來就是被叫來改這台"
        "機器的狀態的,而它自己有 fail-closed(有裝置在線時要求明著給 "
        "RIME_EMU_PORT)。呼叫端(verify_ime.sh 等)在轉呼叫它之前已經過閘。",
}

def scan_files(root):
    """`scripts/` 底下每一個 .sh/.py 的 repo 相對路徑(遞迴、排序、去重)。"""
    out = []
    base = os.path.join(root, SCAN_ROOT)
    for dirpath, dirnames, filenames in os.walk(base):
        dirnames[:] = sorted(d for d in dirnames if d not in SCAN_SKIP_DIRS)
        for name in sorted(filenames):
            if not name.endswith(SCAN_EXTS):
                continue
            rel = os.path.relpath(os.path.join(dirpath, name), root).replace(os.sep, "/")
            if rel in SKIP:
                continue
            if not os.path.isfile(os.path.join(root, rel)):
                continue
            out.append(rel)
    return out


def code_lines(path):
    """(行號, 內容) —— 去掉整行註解與 heredoc 之外的說明段。"""
    out = []
    for i, raw in enumerate(io.open(path, encoding="utf-8", errors="replace"), 1):
        line = raw.rstrip("\n")
        if re.match(r'^\s*#', line):
            continue
        out.append((i, line))
    return out

def check(root):
    problems = []
    for rel in scan_files(root):
            path = os.path.join(root, rel)
            lines = code_lines(path)
            body = "\n".join(l for _, l in lines)
            uses_adb = bool(ADB_USE.search(body))
            sources_dev = "lib/device.sh" in body

            # ── A:寫死 emulator-NNNN ──────────────────────────────────
            for n, l in lines:
                if rel in HARD_SERIAL_ALLOW:
                    break
                if HARD_SERIAL.search(l):
                    problems.append(
                        "A %s:%d 寫死了 `%s` —— port 不是身分,這台機器上"
                        "長期有三到四台模擬器,哪一天它換人這支腳本就打在別人身上。\n"
                        "     %s" % (rel, n, HARD_SERIAL.search(l).group(0), l.strip()))

            # ── B:source 了就要真的呼叫,而且回傳值要用得到 ────────────
            if sources_dev:
                picks = [(n, l) for n, l in lines if "rs_pick_serial" in l]
                selects = [(n, l) for n, l in lines if "rs_select_device" in l]
                if not picks and not selects:
                    problems.append(
                        "B %s source 了 lib/device.sh,卻**一次都沒有呼叫** "
                        "`rs_pick_serial` / `rs_select_device` —— RIME_SERIAL 帶了也沒用,"
                        "而症狀是 `adb -s \"\" shell` 之下的靜默 RC=1。" % rel)
                if picks:
                    assigned = set()
                    for n, l in picks:
                        m = re.search(r'(\w+)=\s*"?\$\(\s*rs_pick_serial', l)
                        if m:
                            assigned.add(m.group(1))
                    if not assigned:
                        problems.append(
                            "B %s 呼叫了 `rs_pick_serial`,但**沒有把回傳值接起來** "
                            "(它把序號印在 stdout)。" % rel)
                    for var in sorted(assigned):
                        uses = [n for n, l in lines
                                if re.search(r'\$\{?%s\b' % re.escape(var), l)]
                        if not uses:
                            problems.append(
                                "B %s 把 `rs_pick_serial` 的結果存進 `%s`,"
                                "而 `%s` 後面**一次都沒被用到**。" % (rel, var, var))
                if selects and not re.search(r'\$\{?RS_SERIAL\b', body):
                    # `rs_select_device` **不印 stdout**,它設 RS_SERIAL。
                    # 照 `rs_pick_serial` 的寫法套上去(命令替換)會拿到空字串。
                    problems.append(
                        "B %s 呼叫了 `rs_select_device`,卻**沒有讀 `$RS_SERIAL`** ——"
                        "那一支不印 stdout,寫成 `X=\"$(rs_select_device ...)\"` 會拿到空字串。"
                        % rel)

            # ── F:有 `--serial` 就必須走 rs_select_device ──────────────
            # ⛔ 2026-08-13 實測:七支腳本都有 `--serial`,而它只寫進區域變數,
            #    `rs_assert_destructive_ok` 的閘只看環境變數 —— 於是
            #    `--serial emulator-5558` 一律 RC=2,訊息說「那台是自動選來的」
            #    而它正是命令列指名的那一台。守門查的是「有沒有呼叫」,
            #    查不到「呼叫端指名的那一條路有沒有接上」。
            if sources_dev and re.search(r'--serial\)', body):
                if "rs_select_device" not in body:
                    problems.append(
                        "F %s 有 `--serial` 旗標,卻沒有走 `rs_select_device` ——"
                        "旗標只寫進區域變數,破壞性動作的閘看不到它,於是帶 `--serial` "
                        "保證 RC=2(訊息還會說「那台是自動選來的」)。" % rel)

            # ── C:破壞性動作要過閘 ────────────────────────────────────
            hits = [(n, l) for n, l in lines
                    if DESTRUCTIVE.search(l) and ADB_CALL.search(l)
                    and not QUOTED_DEMO.match(l)]
            if (hits and rel not in DESTRUCTIVE_ALLOW
                    and "rs_assert_destructive_ok" not in body):
                n, l = hits[0]
                problems.append(
                    "C %s:%d 會做破壞性動作卻沒有 `rs_assert_destructive_ok` ——"
                    "打在別條線的模擬器上,那條線接下來整輪都是紅的而且查不出為什麼。\n"
                    "     %s" % (rel, n, l.strip()))

            # ── D:用 adb 卻不 source device.sh ────────────────────────
            if uses_adb and not sources_dev and rel not in ALLOW:
                problems.append(
                    "D %s 用了 adb 卻沒有 source lib/device.sh,也不在白名單裡。"
                    "要嘛改成走 `rs_pick_serial`,要嘛把它連同理由加進"
                    "`verify_device_hygiene.sh` 的 ALLOW。" % rel)

    # ── E:白名單不得腐爛 ──────────────────────────────────────────────
    for rel, why in sorted(DESTRUCTIVE_ALLOW.items()):
        path = os.path.join(root, rel)
        if not os.path.isfile(path):
            problems.append("E 破壞性動作白名單列著 %s,但那個檔案不存在了。" % rel)
            continue
        body = "\n".join(l for _, l in code_lines(path))
        if not any(DESTRUCTIVE.search(l) and ADB_CALL.search(l) and not QUOTED_DEMO.match(l)
                   for _, l in code_lines(path)):
            problems.append(
                "E 破壞性動作白名單列著 %s,但它現在已經沒有破壞性動作了 ——"
                "請刪掉那一筆。" % rel)
        if not why.strip():
            problems.append("E 破壞性動作白名單裡的 %s 沒有寫理由。" % rel)
    for rel, why in sorted(HARD_SERIAL_ALLOW.items()):
        path = os.path.join(root, rel)
        if not os.path.isfile(path):
            problems.append("E 寫死序號的白名單列著 %s,但那個檔案不存在了。" % rel)
            continue
        if not any(HARD_SERIAL.search(l) for _, l in code_lines(path)):
            problems.append(
                "E 寫死序號的白名單列著 %s,但它現在已經沒有寫死的序號了 ——"
                "請刪掉那一筆。" % rel)
        if not why.strip():
            problems.append("E 寫死序號的白名單裡的 %s 沒有寫理由。" % rel)
    for rel, why in sorted(ALLOW.items()):
        path = os.path.join(root, rel)
        if not os.path.isfile(path):
            problems.append("E 白名單列著 %s,但那個檔案不存在了 —— 請刪掉那一筆。" % rel)
            continue
        body = "\n".join(l for _, l in code_lines(path))
        if "lib/device.sh" in body:
            problems.append(
                "E 白名單列著 %s,但它現在**已經** source 了 lib/device.sh ——"
                "豁免過期了,請從 ALLOW 移除。" % rel)
        elif not ADB_USE.search(body):
            problems.append(
                "E 白名單列著 %s,但它現在根本沒用 adb —— 請刪掉那一筆,"
                "免得白名單變成一份沒有人看得懂的清單。" % rel)
        if not why.strip():
            problems.append("E 白名單裡的 %s 沒有寫理由。" % rel)
    return problems

def run(root, label):
    p = check(root)
    if p:
        print("── %s:%d 項不合格 ──" % (label, len(p)))
        for x in p:
            print("  [FAIL] %s" % x)
    else:
        print("── %s:全部通過 ──" % label)
    return p

if not SELF_TEST:
    problems = run(ROOT, "全樹掃描")
    print()
    if problems:
        print("✗ 裝置選擇守門:%d 項不合格" % len(problems))
        sys.exit(1)
    print("✓ 裝置選擇守門:每一支碰裝置的腳本都問得出「我打在哪一台」")
    sys.exit(0)

# ── 反向驗證:五條規則各植一個違規,必須各自被抓到 ─────────────────────
# 沒有這一段,「掃過了、沒事」與「掃了個寂寞」長得一模一樣 ——
# 而那正是這一輪抓到的兩條缺陷的形狀。
print("=== --self-test:把五種違規逐一植進暫存副本 ===")
bad = 0
PLANTS = [
    ("A", "scripts/_plant_a.sh",
     '#!/usr/bin/env bash\n. "$HERE/lib/device.sh"\nSERIAL="emulator-5558"\n'
     'SERIAL="$(rs_pick_serial "$ADB")"\n"$ADB" -s "$SERIAL" shell true\n'),
    ("B", "scripts/_plant_b.sh",
     '#!/usr/bin/env bash\n. "$HERE/lib/device.sh"\nSERIAL=""\n'
     '"$ADB" -s "$SERIAL" shell true\n'),
    ("B", "scripts/_plant_b2.sh",
     '#!/usr/bin/env bash\n. "$HERE/lib/device.sh"\nrs_pick_serial "$ADB"\n'
     '"$ADB" -s "$SERIAL" shell true\n'),
    ("C", "scripts/_plant_c.sh",
     '#!/usr/bin/env bash\n. "$HERE/lib/device.sh"\n'
     'SERIAL="$(rs_pick_serial "$ADB")"\n"$ADB" -s "$SERIAL" shell pm clear org.x\n'),
    ("D", "scripts/_plant_d.sh",
     '#!/usr/bin/env bash\nADB=adb\n"$ADB" -s "$X" shell true\n'),
    # 遞迴:違規檔放在**子目錄**裡也要抓得到(從前完全掃不到)。
    ("A", "scripts/schema_store/_plant_deep.sh",
     '#!/usr/bin/env bash\n. "$HERE/lib/device.sh"\nSERIAL="emulator-5558"\n'
     'SERIAL="$(rs_pick_serial "$ADB")"\n"$ADB" -s "$SERIAL" shell true\n'),
    # F:有 --serial 卻沒走 rs_select_device。
    ("F", "scripts/_plant_f.sh",
     '#!/usr/bin/env bash\n. "$HERE/lib/device.sh"\nSERIAL=""\n'
     'case "$1" in --serial) SERIAL="$2";; esac\n'
     'SERIAL="$(rs_pick_serial "$ADB")"\n"$ADB" -s "$SERIAL" shell true\n'),
]
for code, rel, content in PLANTS:
    tmp = tempfile.mkdtemp(prefix="rs-hygiene-")
    try:
        shutil.copytree(os.path.join(ROOT, "scripts"), os.path.join(tmp, "scripts"))
        io.open(os.path.join(tmp, rel), "w", encoding="utf-8").write(content)
        got = check(tmp)
        mine = [x for x in got if x.startswith(code + " ") and os.path.basename(rel) in x]
        if mine:
            print("  [PASS] 規則 %s 抓到了 %s" % (code, os.path.basename(rel)))
        else:
            print("  [FAIL] 規則 %s **沒有**抓到 %s —— 那一條什麼都沒在守"
                  % (code, os.path.basename(rel)), file=sys.stderr)
            bad += 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

# E:把一筆白名單指向不存在的檔案
tmp = tempfile.mkdtemp(prefix="rs-hygiene-")
try:
    shutil.copytree(os.path.join(ROOT, "scripts"), os.path.join(tmp, "scripts"))
    os.remove(os.path.join(tmp, "scripts/emu.sh"))
    got = check(tmp)
    if any(x.startswith("E ") and "emu.sh" in x for x in got):
        print("  [PASS] 規則 E 抓到了「白名單指向不存在的檔案」")
    else:
        print("  [FAIL] 規則 E 沒有抓到過期的白名單", file=sys.stderr)
        bad += 1
finally:
    shutil.rmtree(tmp, ignore_errors=True)

print()
print("═══ 裝置選擇守門的反向驗證:%d 項失敗 ═══" % bad)
sys.exit(1 if bad else 0)
PY
