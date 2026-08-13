#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
candbar_geom.py — 算出候選列右端那幾顆方鍵(上一頁／下一頁／展開)的座標。

為什麼需要這支
─────────────────────────────────────────────────────────────────────────────
`scripts/verify_candbar.sh` 從前是這樣找翻頁鍵的：

    NEXT_X=$(( SCREEN_W - $(dp 20) ))     # 「最右邊那顆 40dp 方塊的中心」

那一行在展開鍵存在之前是對的。展開鍵搬進候選列右端之後，**最右端那一點
變成了展開鍵** —— 於是那支腳本整輪點的都是展開鍵，而它的正控
(「點下去不上屏任何東西」)**照樣是綠的**,因為展開鍵也不上屏。
鐵證是它自己的 artifact:檔名叫 `2-page2.png`,畫面上卻是第 1 頁的展開面板。

所以座標要**算**出來,而算的依據必須是產品自己的兩份真相:

  · 方鍵寬度   android/.../keyboard/KeyboardView.kt 的 `CANDIDATE_BAR_BUTTON_DP`
  · 誰會出現   主題的 `candidates.page_indicator` 與 `candidates.bar.scroll`
               / `expand_button.show`,加上「這一頁是第幾頁、是不是最後一頁」

⚠ 讀不到就**報錯**,不給預設值。給了預設值就等於「產品改了而腳本不知道」,
  而那正是這一支要消滅的那種綠燈。

排版模型(對照 KeyboardView.CandidateBar)
─────────────────────────────────────────────────────────────────────────────
那一列是 `Row(Modifier.fillMaxWidth())`,沒有水平 padding,所以右緣就是螢幕
右緣。由左而右:候選(weight(1f))、PageArrows(‹ ›)、ExpandButton(⌄)。
每一顆都是 `CANDIDATE_BAR_BUTTON_DP` dp 寬、`fillMaxHeight()`。

⚠ **按不動的那一顆不畫**(見 [Pager.State] 的檔頭):第 1 頁沒有「‹」,
  最後一頁沒有「›」,而第一頁又是最後一頁時整組都不畫。展開鍵在候選為 0 時
  也不畫。所以位置**取決於當下的頁況**,不是固定的。

用法
─────────────────────────────────────────────────────────────────────────────
    candbar_geom.py --root <repo> --theme default-light \\
                    --screen 1080x2400 --density 420 \\
                    --page-no 0 --last-page 0
        → 一行一顆:`<名字> <x0> <x1> <中心x>`(像素,螢幕絕對座標)

    candbar_geom.py --root … --which next        → 只印那一顆的中心 x
    candbar_geom.py --self-test                  → 反向測試
"""

from __future__ import annotations

import argparse
import os
import re
import sys

try:
    import yaml
except ImportError:  # pragma: no cover
    sys.stderr.write("需要 PyYAML：pip3 install pyyaml\n")
    raise

KT_REL = "android/app/src/main/java/org/luminakey/ime/keyboard/KeyboardView.kt"
_BUTTON_DP_RE = re.compile(
    r'^\s*(?:internal\s+|private\s+)?const\s+val\s+CANDIDATE_BAR_BUTTON_DP\s*=\s*(\d+)\s*$',
    re.M,
)


def button_dp(root: str) -> int:
    """方鍵寬度的唯一真相:KeyboardView.kt 的那個常數。"""
    path = os.path.join(root, KT_REL)
    try:
        with open(path, 'r', encoding='utf-8') as fh:
            text = fh.read()
    except OSError as exc:
        raise SystemExit('讀不到 %s:%s' % (path, exc))
    m = _BUTTON_DP_RE.search(text)
    if not m:
        raise SystemExit(
            '在 %s 裡找不到 `const val CANDIDATE_BAR_BUTTON_DP = <數字>`。\n'
            '  候選列右端那幾顆鍵的寬度是這支腳本算座標的依據 —— 讀不到就不能猜,\n'
            '  猜出來的座標會點在別的東西上,而輸出看起來一切正常。' % path
        )
    return int(m.group(1))


def theme_bar(root: str, theme: str) -> dict:
    path = os.path.join(root, 'core', 'themes', '%s.yaml' % theme)
    try:
        with open(path, 'r', encoding='utf-8') as fh:
            doc = yaml.safe_load(fh) or {}
    except OSError as exc:
        raise SystemExit('讀不到主題 %s:%s' % (path, exc))
    cands = (doc.get('candidates') or {})
    bar = (cands.get('bar') or {})
    pi = (cands.get('page_indicator') or {})
    eb = (bar.get('expand_button') or {})
    return {
        # §8.6.5 / §8.6.6 的預設值(主題沒寫時)。
        'indicator_show': bool(pi.get('show', True)),
        'indicator_style': str(pi.get('style', 'arrows')),
        'scroll': str(bar.get('scroll', 'expandable')),
        'expand_show': bool(eb.get('show', True)),
    }


def buttons(bar: dict, page_no: int, last_page: bool, has_candidates: bool = True):
    """
    由右而左列出會被畫出來的方鍵。回 [(名字, 由右數來第幾顆)]。

    對照 KeyboardView：Row 的順序是 候選 → PageArrows(‹ ›) → ExpandButton，
    所以由右而左是 expand、next、prev。
    """
    order = []
    if has_candidates and bar['expand_show'] and bar['scroll'] == 'expandable':
        order.append('expand')
    indicator = (
        bar['indicator_show']
        and bar['indicator_style'] != 'none'
        and has_candidates
        # 第一頁又是最後一頁 = 兩顆都是死的,整組不畫。
        and not (page_no <= 0 and last_page)
    )
    if indicator and not last_page:
        order.append('next')
    if indicator and page_no > 0:
        order.append('prev')
    return [(name, i) for i, name in enumerate(order)]


def geometry(screen_w_px: int, density: int, dp: int, order):
    """由右而左第 i 顆的 (x0, x1, cx),像素。"""
    w = int(round(dp * density / 160.0))
    out = []
    for name, i in order:
        x1 = screen_w_px - i * w
        x0 = x1 - w
        out.append((name, x0, x1, (x0 + x1) // 2))
    return out


# ── 反向測試 ────────────────────────────────────────────────────────────────
def self_test(root: str) -> int:
    bad = 0

    def check(label, got, want):
        nonlocal bad
        if got == want:
            print('  [PASS] %s = %s' % (label, got))
        else:
            print('  [FAIL] %s = %s,應該是 %s' % (label, got, want), file=sys.stderr)
            bad += 1

    bar = {'indicator_show': True, 'indicator_style': 'arrows',
           'scroll': 'expandable', 'expand_show': True}

    # emulator-5558:1080×2400 @420dpi,40dp → 105px。
    # 第 1 頁(非最後一頁):由右而左是 展開、下一頁。
    g = dict((n, cx) for n, _, _, cx in
             geometry(1080, 420, 40, buttons(bar, page_no=0, last_page=False)))
    check('第 1 頁的展開鍵中心', g.get('expand'), 1027)
    check('第 1 頁的下一頁鍵中心', g.get('next'), 922)
    check('第 1 頁沒有上一頁鍵', 'prev' in g, False)

    # ⚠ 這一條就是這支腳本存在的理由:最右端**不是**翻頁鍵。
    check('最右端那一點(1080-40dp/2=1027)不是翻頁鍵', g.get('next') != 1027, True)

    # 第 2 頁:三顆都在。
    g2 = dict((n, cx) for n, _, _, cx in
              geometry(1080, 420, 40, buttons(bar, page_no=1, last_page=False)))
    check('第 2 頁的下一頁鍵中心', g2.get('next'), 922)
    check('第 2 頁的上一頁鍵中心', g2.get('prev'), 817)

    # 最後一頁沒有「下一頁」。
    g3 = dict((n, cx) for n, _, _, cx in
              geometry(1080, 420, 40, buttons(bar, page_no=1, last_page=True)))
    check('最後一頁沒有下一頁鍵', 'next' in g3, False)
    check('最後一頁的上一頁鍵貼著展開鍵', g3.get('prev'), 922)

    # 只有一頁:兩顆都是死的,整組不畫,展開鍵移到最右端。
    g4 = dict((n, cx) for n, _, _, cx in
              geometry(1080, 420, 40, buttons(bar, page_no=0, last_page=True)))
    check('只有一頁時不畫翻頁鍵', ('next' in g4 or 'prev' in g4), False)
    check('只有一頁時展開鍵仍在最右端', g4.get('expand'), 1027)

    # 主題關掉展開鍵時,翻頁鍵**才**在最右端 —— 也就是舊寫法只在這種主題下對。
    bar2 = dict(bar, expand_show=False)
    g5 = dict((n, cx) for n, _, _, cx in
              geometry(1080, 420, 40, buttons(bar2, page_no=0, last_page=False)))
    check('主題關掉展開鍵時,下一頁鍵才在最右端', g5.get('next'), 1027)

    # 沒有候選時:整列畫的是工具列,兩種鍵都不畫。
    g6 = buttons(bar, page_no=0, last_page=False, has_candidates=False)
    check('沒有候選時一顆都不畫', g6, [])

    # 常數真的讀得到(讀不到會 SystemExit,那也是一種紅)。
    dp = button_dp(root)
    check('KeyboardView.kt 的 CANDIDATE_BAR_BUTTON_DP 讀得到且 > 0', dp > 0, True)

    print()
    print('═══ 候選列座標自我測試:%d 項失敗 ═══' % bad)
    return 1 if bad else 0


def main() -> int:
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument('--root', default='.')
    ap.add_argument('--theme', default='default-light')
    ap.add_argument('--screen', default='1080x2400')
    ap.add_argument('--density', type=int, default=420)
    ap.add_argument('--page-no', type=int, default=0)
    ap.add_argument('--last-page', type=int, default=0)
    ap.add_argument('--no-candidates', action='store_true')
    ap.add_argument('--which', default='')
    ap.add_argument('--self-test', action='store_true')
    args = ap.parse_args()

    root = os.path.abspath(args.root)
    if args.self_test:
        return self_test(root)

    dp = button_dp(root)
    bar = theme_bar(root, args.theme)
    order = buttons(bar, args.page_no, bool(args.last_page), not args.no_candidates)
    screen_w = int(args.screen.split('x')[0])
    rows = geometry(screen_w, args.density, dp, order)
    if args.which:
        for name, _x0, _x1, cx in rows:
            if name == args.which:
                print(cx)
                return 0
        sys.stderr.write(
            '這個頁況下畫不出「%s」(頁次 %d、%s最後一頁、主題 %s)。\n'
            % (args.which, args.page_no,
               '是' if args.last_page else '不是', args.theme)
        )
        return 3
    for name, x0, x1, cx in rows:
        print('%s %d %d %d' % (name, x0, x1, cx))
    return 0


if __name__ == '__main__':
    sys.exit(main())
