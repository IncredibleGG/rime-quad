#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
candbar_geom.py — 算出候選列右端**那一顆**方鍵的座標，以及一列排得下幾個。

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

2026-08-13:右端從「最多兩顆」變成「最多一顆」
─────────────────────────────────────────────────────────────────────────────
規範草稿 §8.6.6.4:**本頁還有畫不出來的候選時,⛔ 不得提供「下一頁」。**
翻頁鍵的語義是「本頁我看完了」;本頁沒看完就給翻頁,等於讓使用者跳過他
從未看見的候選,而畫面完全正常。

於是右端那一格是三選一(對照 `keyboard/CandidateDensity.kt` 的 `RightEnd`):

    本頁候選數 == 0            → 一顆都不畫(候選列現在畫的是工具列)
    畫得出來的 < 本頁候選數    → **展開 `∨`**,翻頁移進展開面板內部
    畫得出來的 >= 本頁候選數   → **翻頁 `‹ ›`**(各自畫不畫仍看頁次)

「畫得出來幾個」因此變成這支腳本**必須**算得出來的東西,不再是外部參數:
算不出來就不知道右端畫的是哪一顆,而點錯一顆的症狀正是上面那段假綠燈。

⚠ 這裡的密度公式與 `keyboard/CandidateDensity.kt` 是**兩份實作**,會漂移。
  兩道防線:(a) `--self-test` 用與 `CandidateDensityTest` 完全相同的數字
  (一格 56 dp、節距 60 dp、411.43 dp 上 6 個);(b) verify_candbar.sh 最終
  斷言的是**畫面上真的長什麼樣**,算錯了那邊會紅。

排版模型(對照 KeyboardView.CandidateBar)
─────────────────────────────────────────────────────────────────────────────
那一列是 `Row(Modifier.fillMaxWidth())`,沒有水平 padding,所以右緣就是螢幕
右緣。由左而右:候選(weight(1f))、然後右端那一格。
每一顆控制鍵都是 `CANDIDATE_BAR_BUTTON_DP` dp 寬、`fillMaxHeight()`。

用法
─────────────────────────────────────────────────────────────────────────────
    candbar_geom.py --root <repo> --theme default-light \\
                    --screen 1080x2400 --density 420 \\
                    --page-no 0 --last-page 0 --page-count 9
        → 一行一顆:`<名字> <x0> <x1> <中心x>`(像素,螢幕絕對座標)

    candbar_geom.py --root … --which expand      → 只印那一顆的中心 x
    candbar_geom.py --root … --which visible     → 印「一列畫得出幾個」
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

# 與 CandidateDensity.kt 的同名常數對齊。
BASELINE_TEXT_SIZE = 20.0
BASELINE_TEXT_CHARS = 2


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


def _load_theme_chain(root: str, theme: str, depth: int = 0) -> dict:
    """把 `inherits` 那條鏈**由父到子**合併起來。

    ⚠ 不合併的話,`intl-ios-light` 讀出來的 `item.padding_h` 是 None,
      而 None 會被當成「用預設值」—— 那個預設值恰好與父主題不同時,
      算出來的密度是憑空的。
    """
    if depth > 8:
        raise SystemExit('主題 %s 的 inherits 太深或有環' % theme)
    path = os.path.join(root, 'core', 'themes', '%s.yaml' % theme)
    try:
        with open(path, 'r', encoding='utf-8') as fh:
            doc = yaml.safe_load(fh) or {}
    except OSError as exc:
        raise SystemExit('讀不到主題 %s:%s' % (path, exc))
    parent_id = doc.get('inherits')
    if not parent_id:
        return doc
    parent = _load_theme_chain(root, str(parent_id), depth + 1)
    return _deep_merge(parent, doc)


def _deep_merge(base: dict, over: dict) -> dict:
    out = dict(base)
    for k, v in over.items():
        if isinstance(v, dict) and isinstance(out.get(k), dict):
            out[k] = _deep_merge(out[k], v)
        else:
            out[k] = v
    return out


def theme_bar(root: str, theme: str) -> dict:
    doc = _load_theme_chain(root, theme)
    cands = (doc.get('candidates') or {})
    bar = (cands.get('bar') or {})
    pi = _deep_merge(cands.get('page_indicator') or {}, bar.get('page_indicator') or {})
    eb = (bar.get('expand_button') or {})
    item = _deep_merge(cands.get('item') or {}, bar.get('item') or {})
    return {
        # §8.6.5 / §8.6.6 的預設值(主題沒寫時)。
        'indicator_show': bool(pi.get('show', True)),
        'indicator_style': str(pi.get('style', 'arrows')),
        'scroll': str(bar.get('scroll', 'expandable')),
        'expand_show': bool(eb.get('show', True)),
        # §8.6.4.2 的密度。預設值與 ThemeParser.parseBar / ThemeDefaults 相同。
        'bar_padding_h': float(bar.get('padding_h', 4)),
        'reserved_end': float(bar.get('reserved_end', 40)),
        'item_padding_h': float(item.get('padding_h', 6)),
        'item_spacing': float(item.get('spacing', 4)),
        'item_min_width': float(item.get('min_width', 0)),
    }


def visible_count(bar: dict, screen_w_px: int, density: int,
                  button_dp_v: int, page_no: int = 0) -> int:
    """基準情境下一列**完整**畫得出幾個(對照 CandidateDensity.baselineVisible)。

    基準情境 = `text.size: 20`、兩字 CJK、無序號無註解 —— 與規範 §10 第 40 條
    以及 `CandidateDensityTest` 用的是同一組數。
    """
    width_dp = screen_w_px * 160.0 / density
    item_w = max(
        BASELINE_TEXT_CHARS * BASELINE_TEXT_SIZE + 2 * bar['item_padding_h'],
        bar['item_min_width'],
    )
    reserved = bar['reserved_end']
    # 第 2 頁以後翻頁那一支會畫兩顆(上一頁＋下一頁),量測時先扣掉。
    if page_no > 0 and bar['indicator_show'] and bar['indicator_style'] != 'none':
        reserved += button_dp_v
    usable = width_dp - 2 * bar['bar_padding_h'] - reserved
    if usable <= 0:
        return 0
    n = 0
    used = 0.0
    while True:
        nxt = item_w if n == 0 else used + bar['item_spacing'] + item_w
        if nxt > usable:
            break
        used = nxt
        n += 1
    return n


def buttons(bar: dict, page_no: int, last_page: bool,
            has_candidates: bool = True, visible: int = 0, page_count: int = 0):
    """
    由右而左列出會被畫出來的方鍵。回 [(名字, 由右數來第幾顆)]。

    §8.6.6.4：**右端最多一種控制**。展開與翻頁解決的是同一個問題,
    兩顆一起出現是同一份資訊的第二份。
    """
    if not has_candidates or page_count <= 0:
        return []
    expand_available = bar['expand_show'] and bar['scroll'] == 'expandable'
    indicator = bar['indicator_show'] and bar['indicator_style'] != 'none'

    if visible < page_count:
        # ⛔ 本頁還有畫不出來的候選 —— 不得提供翻頁。出口是展開面板。
        if expand_available:
            return [('expand', 0)]
        # 主題自己關掉展開鍵時退回翻頁(把使用者鎖死在第 1 頁更糟)。
        # 隨附主題不該走到這裡,`ThemeDensityTest` 逐份擋著。
    order = []
    if indicator and not (page_no <= 0 and last_page):
        if not last_page:
            order.append('next')
        if page_no > 0:
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

    # 改動後的隨附主題值。
    bar = {'indicator_show': True, 'indicator_style': 'arrows',
           'scroll': 'expandable', 'expand_show': True,
           'bar_padding_h': 4.0, 'reserved_end': 40.0,
           'item_padding_h': 8.0, 'item_spacing': 4.0, 'item_min_width': 48.0}

    # ── 密度:與 CandidateDensityTest 用同一組數 ────────────────────────
    # emulator-5558:1080×2400 @420dpi = 411.43 dp。
    check('411.43 dp 上畫得出幾個', visible_count(bar, 1080, 420, 40), 6)
    # 360 dp @160dpi。
    check('360 dp 上畫得出幾個', visible_count(bar, 360, 160, 40), 5)
    # 改動前的那組值必須少一個 —— 否則這條公式什麼都沒在守。
    before = dict(bar, item_padding_h=10.0, item_min_width=0.0, reserved_end=80.0)
    check('改動前(內距 10、右端兩顆)只有 5 個', visible_count(before, 1080, 420, 40), 5)

    # ── 右端那一顆 ─────────────────────────────────────────────────────
    # 九宮格實況:引擎一頁 9 個,畫得出 6 個 → 只有展開鍵。
    g = dict((n, cx) for n, _, _, cx in
             geometry(1080, 420, 40,
                      buttons(bar, page_no=0, last_page=False, visible=6, page_count=9)))
    check('本頁看不完時右端是展開鍵', g.get('expand'), 1027)
    check('⛔ 本頁看不完時不得出現翻頁鍵', 'next' in g, False)

    # 本頁只有 4 個、畫得出 6 個 = 本頁看完了 → 翻頁鍵,而且它在最右端。
    g2 = dict((n, cx) for n, _, _, cx in
              geometry(1080, 420, 40,
                       buttons(bar, page_no=0, last_page=False, visible=6, page_count=4)))
    check('本頁看得完時右端是下一頁', g2.get('next'), 1027)
    check('這時候沒有展開鍵', 'expand' in g2, False)
    check('第 1 頁沒有上一頁鍵', 'prev' in g2, False)

    # 第 2 頁:上一頁＋下一頁兩顆。
    g3 = dict((n, cx) for n, _, _, cx in
              geometry(1080, 420, 40,
                       buttons(bar, page_no=1, last_page=False, visible=6, page_count=4)))
    check('第 2 頁的下一頁鍵中心', g3.get('next'), 1027)
    check('第 2 頁的上一頁鍵中心', g3.get('prev'), 922)

    # 只有一頁又看得完:一顆都不畫。
    g4 = buttons(bar, page_no=0, last_page=True, visible=6, page_count=4)
    check('只有一頁又看得完時右端是空的', g4, [])

    # 沒有候選時:整列畫的是工具列,什麼都不畫。
    check('沒有候選時一顆都不畫',
          buttons(bar, page_no=0, last_page=False, has_candidates=False,
                  visible=0, page_count=0), [])

    # 常數真的讀得到(讀不到會 SystemExit,那也是一種紅)。
    dp = button_dp(root)
    check('KeyboardView.kt 的 CANDIDATE_BAR_BUTTON_DP 讀得到且 > 0', dp > 0, True)

    # 主題真的讀得到,而且是**合併過 inherits 的那一份**。
    t = theme_bar(root, 'intl-ios-light')
    check('intl-ios-light 繼承到 default-light 的 item.padding_h', t['item_padding_h'], 8.0)
    check('intl-ios-light 繼承到 default-light 的 item.min_width', t['item_min_width'], 48.0)

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
    ap.add_argument('--page-count', type=int, default=9,
                    help='本頁候選總數(引擎的 page_size)')
    ap.add_argument('--no-candidates', action='store_true')
    ap.add_argument('--which', default='')
    ap.add_argument('--self-test', action='store_true')
    args = ap.parse_args()

    root = os.path.abspath(args.root)
    if args.self_test:
        return self_test(root)

    dp = button_dp(root)
    bar = theme_bar(root, args.theme)
    screen_w = int(args.screen.split('x')[0])
    vis = visible_count(bar, screen_w, args.density, dp, args.page_no)
    if args.which == 'visible':
        print(vis)
        return 0
    order = buttons(bar, args.page_no, bool(args.last_page),
                    not args.no_candidates, vis, args.page_count)
    rows = geometry(screen_w, args.density, dp, order)
    if args.which:
        for name, _x0, _x1, cx in rows:
            if name == args.which:
                print(cx)
                return 0
        sys.stderr.write(
            '這個頁況下畫不出「%s」(頁次 %d、%s最後一頁、本頁 %d 個、畫得出 %d 個、主題 %s)。\n'
            % (args.which, args.page_no,
               '是' if args.last_page else '不是', args.page_count, vis, args.theme)
        )
        return 3
    for name, x0, x1, cx in rows:
        print('%s %d %d %d' % (name, x0, x1, cx))
    return 0


if __name__ == '__main__':
    sys.exit(main())
