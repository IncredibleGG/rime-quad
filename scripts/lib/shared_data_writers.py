#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
shared_data_writers.py — 掃出「會寫進 core/data/shared 或 core/data/user」的腳本。

為什麼需要這支
─────────────────────────────────────────────────────────────────────────────
`core/data/shared` 與 `core/data/user` 是 **產物**，而且在 .gitignore 裡。
於是 `git diff -- core/data` 對它們**永遠是空的** —— 改了四端共用的執行期資料
卻一個字都 diff 不出來。verify_core_data_fanout.sh 就是這樣對 cand 那一批
(把 `menu/page_size` 從 5 改成 9，四端的候選窗全部跟著變)整關沉默的。

真正的改動在**產生器**裡。所以那一關要看的不只是產物，還有產生器 ——
而產生器的清單**不可以寫死**：下一支寫進 core/data/shared 的腳本一出現，
寫死的清單會安靜地漏掉它，那正是這支腳本要消滅的失敗模式。

判準
─────────────────────────────────────────────────────────────────────────────
一個版控裡的腳本算「產生器」，若它有任何一行**同時**滿足：

  (1) 指到 core/data/shared 或 core/data/user
      —— 直接寫路徑，或透過同一個檔案裡指向那裡的變數（含指到
         `core/data` 再接 `/shared`、`/user` 的兩段式寫法）；
  (2) 那一行是在**寫**它，不是在讀它。

「寫」認得的形狀列在 `_WRITE_RULES`。**讀**的形狀刻意不算：
`cp core/data/shared <別處>`(把它複製走)、`adb push`(推上裝置)、
`[ -d ... ]`(檢查存在)、`--opencc core/data/shared/opencc`(當輸入餵給別人)。
把讀也算進去的話，改 publish_desktop.sh 就會要求四條車道全跑 —— 一道會為了
無關的改動亮紅燈的守門，教會的是「略過它」。

⚠ 這支腳本會漏。它認的是形狀，不是語意。所以呼叫端(verify_core_data_fanout.sh)
  還釘了一條**下界**：已知的產生器一個都不許掃不到，掃不到就是這支壞了。
  下界之外它會自己長 —— 新的產生器只要用得上這幾種形狀就會被撿到。

用法
─────────────────────────────────────────────────────────────────────────────
    shared_data_writers.py --root <repo>          # 一行一個相對路徑
    shared_data_writers.py --root <repo> --why    # 附上是哪一行讓它中選
    shared_data_writers.py --self-test            # 反向測試:分不分得出讀與寫
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys

# ── 目標路徑 ────────────────────────────────────────────────────────────────
_GEN = r'(?:shared|user)'
# 直接寫出來的路徑。
_LIT = re.compile(r'core/data/' + _GEN + r'(?![A-Za-z0-9_])')
# 指到 core/data 本身(後面才接 /shared、/user)的變數。
_BASE_VAL = re.compile(r'core/data(?![A-Za-z0-9_/])')

_ASSIGN = re.compile(
    r'^[ \t]*(?:local[ \t]+|export[ \t]+)?([A-Za-z_][A-Za-z0-9_]*)=(.*)$', re.M
)

# ── 「這一行在寫」的形狀 ────────────────────────────────────────────────────
#
# 每一條都是 (名字, 樣板)。樣板裡的 {T} 會換成「目標路徑」的 regex。
# ⚠ 動詞一律要求在**命令位置**(行首、`;`/`&&`/`||`/`|`/`(` 之後、
#   `then`/`do`/`else` 之後、`$(` 之內)。否則
#   `fail "  cp core/data/schemas/x core/data/shared/"` 這種**訊息字串**
#   會被當成真的在複製 —— verify_syllables.sh 裡就有兩句長這樣。
_CMD = r'(?:^|[;&|(]|`|\$\(|\bthen\b|\bdo\b|\belse\b)[ \t]*'

_WRITE_RULES = [
    # 導向:  > 目標  /  >> 目標
    ('redirect', r'>>?[ \t]*[\'"]?{T}'),
    # 就地改內容 / 建立 / 刪除 / 連結 / 分流
    ('mkdir',    _CMD + r'mkdir\b[^;&|]*{T}'),
    ('rm',       _CMD + r'rm\b[^;&|]*{T}'),
    ('touch',    _CMD + r'touch\b[^;&|]*{T}'),
    ('ln',       _CMD + r'ln\b[^;&|]*{T}'),
    ('tee',      r'\|[ \t]*tee\b[^;&|]*{T}'),
    ('sed -i',   _CMD + r'sed\b[^;&|]*-i\b[^;&|]*{T}'),
    ('unzip',    _CMD + r'unzip\b[^;&|]*-d[ \t]*[\'"]?{T}'),
    ('tar -C',   _CMD + r'tar\b[^;&|]*-C[ \t]*[\'"]?{T}'),
    # 輸出旗標
    ('--out',    r'(?:--out|--output|--outdir|--dest|-o)[ \t=][ \t]*[\'"]?{T}'),
    # Python(`[^\n]*?` 而不是 `[^)]*`:路徑常常包在 os.path.join(...) 裡,
    # 用 `[^)]` 會在那個右括號就停住)
    ('open(w)',  r'open\([^\n]{0,200}?{T}[^\n]{0,80}?[\'"][wax]'),
    ('shutil',   r'shutil\.(?:copy\w*|move|rmtree)\([^)]*{T}'),
    ('makedirs', r'(?:os\.makedirs|os\.mkdir)\([^)]*{T}'),
    ('write_*',  r'{T}[^\n]*\)\.write_(?:text|bytes)\('),
]

# cp / mv / install / rsync 的**最後一個** token 才是目的地。
# `cp core/data/shared <別處>` 是把它複製走(讀)，不是寫它。
_DEST_LAST = re.compile(_CMD + r'(?:cp|mv|install|rsync)\b(?P<args>[^;&|]*)')

_SCRIPT_EXT = ('.sh', '.py', '.bash', '.zsh')


def _tracked_files(root: str) -> list[str]:
    out = subprocess.run(
        ['git', '-C', root, 'ls-files', '-z'],
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, check=False,
    ).stdout.decode('utf-8', 'replace')
    return [p for p in out.split('\0') if p]


def _looks_like_script(root: str, rel: str) -> bool:
    if rel.endswith(_SCRIPT_EXT):
        return True
    path = os.path.join(root, rel)
    try:
        with open(path, 'rb') as fh:
            return fh.read(2) == b'#!'
    except OSError:
        return False


def _strip_comment(line: str) -> str:
    """去掉行末註解。引號沒配對就整行留著(寧可多看，不要少看)。"""
    stripped = line.lstrip()
    if stripped.startswith('#'):
        return ''
    idx = 0
    while True:
        idx = line.find('#', idx)
        if idx <= 0:
            return line
        if line[idx - 1] in ' \t':
            head = line[:idx]
            if head.count('"') % 2 == 0 and head.count("'") % 2 == 0:
                return head
        idx += 1


def _target_regex(text: str) -> str:
    """這個檔案裡，「指到 core/data/shared|user」長什麼樣。"""
    alts = [r'core/data/' + _GEN + r'(?![A-Za-z0-9_])']
    direct, base = set(), set()
    for m in _ASSIGN.finditer(text):
        name, val = m.group(1), m.group(2)
        val = _strip_comment(val)
        if _LIT.search(val):
            direct.add(name)
        elif _BASE_VAL.search(val):
            base.add(name)
    for n in sorted(direct):
        alts.append(r'\$\{?' + re.escape(n) + r'\}?')
    for n in sorted(base):
        alts.append(r'\$\{?' + re.escape(n) + r'\}?/' + _GEN + r'(?![A-Za-z0-9_])')
    return '(?:' + '|'.join(alts) + ')'


def writes_shared_data(text: str) -> tuple[bool, str]:
    """(會不會寫, 是哪一行讓它中選)。"""
    tgt = _target_regex(text)
    rules = [(name, re.compile(pat.replace('{T}', tgt))) for name, pat in _WRITE_RULES]
    tgt_re = re.compile(tgt)
    for raw in text.splitlines():
        line = _strip_comment(raw)
        if not tgt_re.search(line):
            continue
        for name, rx in rules:
            if rx.search(line):
                return True, '%s: %s' % (name, raw.strip())
        m = _DEST_LAST.search(line)
        if m:
            args = m.group('args').split()
            # 目的地 = 最後一個非旗標 token。
            dest = ''
            for tok in reversed(args):
                if not tok.startswith('-'):
                    dest = tok
                    break
            if dest and tgt_re.search(dest):
                return True, 'cp/mv(目的地): %s' % raw.strip()
    return False, ''


def scan(root: str) -> list[tuple[str, str]]:
    found = []
    for rel in _tracked_files(root):
        if not _looks_like_script(root, rel):
            continue
        try:
            with open(os.path.join(root, rel), 'r', encoding='utf-8', errors='replace') as fh:
                text = fh.read()
        except OSError:
            continue
        hit, why = writes_shared_data(text)
        if hit:
            found.append((rel, why))
    found.sort()
    return found


# ── 反向測試 ────────────────────────────────────────────────────────────────
#
# 一支只會印出東西的掃描器沒有辦法讓人相信。這裡拿**寫**與**讀**兩組合成的
# 片段跑同一段判斷:寫的必須全中，讀的必須一個都不中。
_MUST_HIT = [
    ('直接 rm', 'rm -rf "$ROOT/core/data/shared"'),
    ('變數 mkdir', 'OUT="$ROOT/core/data/shared"\nmkdir -p "$OUT/opencc"'),
    ('變數 cp 目的地', 'OUT="$ROOT/core/data/shared"\ncp "$T9" "$OUT/"'),
    ('sed -i', 'OUT="$ROOT/core/data/shared"\nsed -i \'s/a/b/\' "$OUT/default.yaml"'),
    ('導向', 'OUT_USER="$ROOT/core/data/user"\nprintf x > "$OUT_USER/default.custom.yaml"'),
    ('附加', 'O="$ROOT/core/data/shared"\ncat "$P" >> "$O/luna_pinyin.custom.yaml"'),
    ('兩段式變數', 'D="$ROOT/core/data"\nmkdir -p "$D/shared/lua"'),
    ('python', 'open(os.path.join(root, "core/data/shared/x.yaml"), "w")'),
]
_MUST_MISS = [
    ('複製走', 'cp -R "$ROOT/core/data/shared" "$PKG/data/shared"'),
    ('adb push', '"$ADB" push "$ROOT/core/data/shared" "$DEV/shared"'),
    ('存在檢查', '[ -d "$ROOT/core/data/shared" ] || die "先跑 collect_data.sh"'),
    ('當輸入', 'python3 x.py --opencc "$ROOT/core/data/shared/opencc"'),
    ('訊息字串', 'fail "  cp core/data/schemas/a.yaml core/data/shared/"'),
    ('註解', '# 這裡會寫進 core/data/shared —— 但這只是註解'),
    ('列檔案', 'find "$ROOT/core/data/shared" -type f | wc -l'),
    ('丟到別處', 'OUT="$ROOT/core/data/shared"\ntar -czf /tmp/x.tgz "$OUT"'),
]


def self_test() -> int:
    bad = 0
    for name, src in _MUST_HIT:
        hit, _ = writes_shared_data(src)
        if hit:
            print('  [PASS] 認得出「%s」是在寫' % name)
        else:
            print('  [FAIL] 「%s」是在寫，卻沒被認出來' % name, file=sys.stderr)
            bad += 1
    for name, src in _MUST_MISS:
        hit, why = writes_shared_data(src)
        if not hit:
            print('  [PASS] 沒有把「%s」誤判成寫' % name)
        else:
            print('  [FAIL] 「%s」只是在讀，卻被判成寫(%s)' % (name, why), file=sys.stderr)
            bad += 1
    print()
    print('═══ 產生器掃描器自我測試:%d 項失敗 ═══' % bad)
    return 1 if bad else 0


def main() -> int:
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument('--root', default='.')
    ap.add_argument('--why', action='store_true')
    ap.add_argument('--self-test', action='store_true')
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    for rel, why in scan(os.path.abspath(args.root)):
        print('%s\t%s' % (rel, why) if args.why else rel)
    return 0


if __name__ == '__main__':
    sys.exit(main())
