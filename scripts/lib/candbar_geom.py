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
    candbar_geom.py --root … --which reserved    → 印「右端實際佔多少 dp」
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
    text = _deep_merge(cands.get('text') or {}, bar.get('text') or {})
    label = _deep_merge(cands.get('label') or {}, bar.get('label') or {})
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
        # §8.6.1.1 的序號:`verify_selection_digit.sh` 要從高亮塊的**寬度**
        # 看得出「這一格有沒有畫序號」,所以字級也要讀出來。
        'text_size': float(text.get('size', 20)),
        'label_size': float(label.get('size', 12)),
        'label_show': bool(label.get('show', True)),
    }


# 與 CandidateDensity.LATIN_EM / GAP_DP 是同一個數。
LATIN_EM = 0.55
GAP_DP = 3.0


def item_width_dp(bar: dict, text_chars: int, label_chars: int = 0) -> float:
    """一格候選的量測寬(對照 CandidateDensity.itemWidthDp)。"""
    w = text_chars * bar['text_size'] + 2 * bar['item_padding_h']
    if label_chars > 0:
        w += label_chars * bar['label_size'] * LATIN_EM + GAP_DP
    return max(w, bar['item_min_width'])


def _item_width(bar: dict) -> float:
    """基準情境的一格寬(對照 CandidateDensity.itemWidthDp)。"""
    return max(
        BASELINE_TEXT_CHARS * BASELINE_TEXT_SIZE + 2 * bar['item_padding_h'],
        bar['item_min_width'],
    )


def _fits(bar: dict, screen_w_px: int, density: int, reserved_dp: float) -> int:
    width_dp = screen_w_px * 160.0 / density
    item_w = _item_width(bar)
    usable = width_dp - 2 * bar['bar_padding_h'] - reserved_dp
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


def _pager_names(bar: dict, page_no: int, last_page: bool, page_count: int):
    """翻頁那一組**會被畫出來的**幾顆,由右而左。與 Pager.state 同一條規則。"""
    if page_count <= 0:
        return []
    if not (bar['indicator_show'] and bar['indicator_style'] != 'none'):
        return []
    if page_no <= 0 and last_page:
        return []          # 兩顆都是死的,整組不畫
    order = []
    if not last_page:
        order.append('next')
    if page_no > 0:
        order.append('prev')
    return order


def _reserved_dp(bar: dict, button_dp_v: int, count: int) -> float:
    """`n` 顆控制鍵佔多寬。**一顆都不畫就是 0** —— 對照 CandidateDensity.reservedDp。"""
    if count <= 0:
        return 0.0
    return bar['reserved_end'] + (count - 1) * button_dp_v


def solve(bar: dict, screen_w_px: int, density: int, button_dp_v: int,
          page_no: int = 0, last_page: bool = False, page_count: int = 0,
          has_candidates: bool = True, panel_open: bool = False):
    """一次算出「畫幾個、右端畫什麼、右端佔多寬」。

    ⚠ **這裡與 `keyboard/CandidateDensity.kt` 的 `barLayout()` 是同一個解法**,
      不是兩份各自為政的公式:量測扣掉的寬度必須就是實際會畫的那幾顆。
      上一版這裡逐字複製了 Kotlin 那邊的 `page_no > 0 → +button` 分支,
      而那個分支在 11 種頁況裡有 5 種與畫面對不上。

    ⚠ 2026-08-13 第三次改動,兩件事:

      (甲)**`panel_open` 從缺**。Kotlin 的 `rightEnd()` 有一支
           `panelOpen && expandAvailable -> EXPAND`(面板自己沒有關閉鍵,
           右端換成翻頁就等於把一片蓋住鍵盤的浮層留在畫面上而沒有出口),
           而這支腳本的模型裡**根本沒有這個參數** —— 於是「面板開著又在
           第 2 頁以後」那幾格,兩份實作對不上而沒有任何斷言涵蓋得到。

      (乙)**判準改成「這一顆真的畫得出來」**。上一版在
           `v_pager >= page_count` 時無條件回 `pager`,不看
           `_pager_names()` 是不是空的。主題寫 `page_indicator.show: false`
           時那一組一顆都不畫 → 右端空白、使用者鎖死在第 1 頁。

    回 (visible, right_end, reserved_dp, pager_names)。
    """
    if not has_candidates or page_count <= 0:
        return _fits(bar, screen_w_px, density, 0.0), 'none', 0.0, []

    expand_available = bar['expand_show'] and bar['scroll'] == 'expandable'
    pager = _pager_names(bar, page_no, last_page, page_count)
    pager_drawable = len(pager) > 0
    pager_dp = _reserved_dp(bar, button_dp_v, len(pager))
    expand_dp = _reserved_dp(bar, button_dp_v, 1)

    def as_expand():
        return _fits(bar, screen_w_px, density, expand_dp), 'expand', expand_dp, []

    def as_none():
        return _fits(bar, screen_w_px, density, 0.0), 'none', 0.0, []

    # 面板開著 → 那一顆**必須**是收合鍵。
    if panel_open and expand_available:
        return as_expand()

    v_pager = _fits(bar, screen_w_px, density, pager_dp)
    if v_pager < page_count:
        # ⛔ 本頁還有畫不出來的候選 —— 不得提供翻頁。出口是展開面板;
        #    主題關掉展開鍵時退回翻頁(鎖死在第 1 頁比跳過幾個候選更糟),
        #    而退路自己也要畫得出來。
        if expand_available:
            return as_expand()
        return (v_pager, 'pager', pager_dp, pager) if pager_drawable else as_none()
    # 本頁看得完 → 翻頁才誠實,而且它得真的畫得出來。
    if pager_drawable:
        return v_pager, 'pager', pager_dp, pager
    # 翻頁那一組畫不出來而後面還有頁 → 展開鍵是剩下的唯一出口。
    if (not last_page) and expand_available:
        return as_expand()
    return as_none()


def dead_end(right_end: str, visible: int, page_count: int, last_page: bool) -> bool:
    """⛔ 後面還有候選,而右端一顆出口都畫不出來 —— 設計錯誤,不是頁況。

    與 `CandidateDensity.deadEnd()` 是同一條規則。
    """
    if page_count <= 0:
        return False
    return (visible < page_count or not last_page) and right_end == 'none'


def visible_count(bar: dict, screen_w_px: int, density: int,
                  button_dp_v: int, page_no: int = 0,
                  last_page: bool = False, page_count: int = 0) -> int:
    """基準情境下一列**完整**畫得出幾個(對照 CandidateDensity.barLayout)。

    基準情境 = `text.size: 20`、兩字 CJK、無序號無註解 —— 與規範草稿 §8.6.4.2
    以及 `CandidateDensityTest` 用的是同一組數。
    """
    if page_count <= 0:
        # 呼叫端只想問密度(不指定頁況)時,用「右端一顆」這個最常見的情況。
        return _fits(bar, screen_w_px, density, _reserved_dp(bar, button_dp_v, 1))
    return solve(bar, screen_w_px, density, button_dp_v,
                 page_no, last_page, page_count)[0]


def buttons(bar: dict, page_no: int, last_page: bool,
            has_candidates: bool = True, visible: int = 0, page_count: int = 0,
            screen_w_px: int = 0, density: int = 0, button_dp_v: int = 0,
            panel_open: bool = False):
    """
    由右而左列出會被畫出來的方鍵。回 [(名字, 由右數來第幾顆)]。

    §8.6.6.4：**右端最多一種控制**。展開與翻頁解決的是同一個問題,
    兩顆一起出現是同一份資訊的第二份。
    """
    if not has_candidates or page_count <= 0:
        return []
    if screen_w_px and density and button_dp_v:
        _v, end, _dp, pager = solve(bar, screen_w_px, density, button_dp_v,
                                    page_no, last_page, page_count, has_candidates,
                                    panel_open)
        if end == 'expand':
            return [('expand', 0)]
        if end == 'none':
            return []
        return [(name, i) for i, name in enumerate(pager)]

    # 沒有螢幕資訊時退回「呼叫端已經算好 visible」那條路(self-test 用)。
    expand_available = bar['expand_show'] and bar['scroll'] == 'expandable'
    if panel_open and expand_available:
        return [('expand', 0)]
    names = _pager_names(bar, page_no, last_page, page_count)
    if visible < page_count and expand_available:
        return [('expand', 0)]
    if not names and (not last_page) and expand_available:
        return [('expand', 0)]
    return [(name, i) for i, name in enumerate(names)]


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

    # 面板開著:右端**必須**是收合鍵,不管頁況(面板自己沒有關閉鍵)。
    g5 = buttons(bar, page_no=1, last_page=False, visible=6, page_count=4,
                 panel_open=True)
    check('面板開著時右端是收合鍵', g5, [('expand', 0)])

    # ── ⛔ 量測扣掉的 == 畫出來的那幾顆 ────────────────────────────────
    # 上一版這裡逐字複製了 Kotlin 的 `page_no > 0 → +button` 分支,而那個
    # 分支在 11 種頁況裡有 5 種與畫面對不上(量測扣 80、實際畫 40)。
    #
    # ⚠ `panel_open` 那一欄是 2026-08-13 補上的:Kotlin 的 `rightEnd()` 一直
    #   有 `panelOpen && expandAvailable -> EXPAND` 這一支,而這裡的 `solve()`
    #   參數表**根本沒有這個參數** —— 於是「面板開著又在第 2 頁以後」那幾格
    #   一條斷言都涵蓋不到,而那正是缺陷 2 原本對不上的頁況之一。
    for page_no, last, count, panel, want_end, want_dp in (
            (0, True,  4, False, 'none',   0.0),   # 翻頁整組不畫,後面也沒東西
            (1, False, 9, False, 'expand', 40.0),  # 第 2 頁、本頁看不完
            (1, True,  9, False, 'expand', 40.0),  # 第 2 頁又是最後一頁、看不完
            (1, True,  3, False, 'pager',  40.0),  # 只有「上一頁」一顆
            (1, False, 3, False, 'pager',  80.0),  # 上一頁＋下一頁
            (0, False, 9, False, 'expand', 40.0),  # 第 1 頁、本頁看不完
            # ── 面板開著:右端**必定**是收合鍵一顆 ──────────────────────
            (0, False, 9, True,  'expand', 40.0),  # 第 1 頁、看不完
            (1, False, 9, True,  'expand', 40.0),  # 第 2 頁、看不完
            (1, False, 3, True,  'expand', 40.0),  # 第 2 頁、**看得完**
            (1, True,  3, True,  'expand', 40.0),  # 第 2 頁又是最後一頁、看得完
            (0, True,  3, True,  'expand', 40.0),  # 只有一頁、看得完
    ):
        _v, end, dp, names = solve(bar, 1080, 420, 40, page_no, last, count,
                                   True, panel)
        check('頁況(第 %d 頁,%s最後一頁,本頁 %d 個,面板%s)的右端'
              % (page_no + 1, '是' if last else '不是', count,
                 '開著' if panel else '收著'), end, want_end)
        check('  同一格量測扣掉的寬度', dp, want_dp)
        drawn = 0 if end == 'none' else (1 if end == 'expand' else len(names))
        check('  量測扣的顆數 == 畫出來的顆數',
              dp, 0.0 if drawn == 0 else bar['reserved_end'] + (drawn - 1) * 40)
        check('  這一格不是死路', dead_end(end, _v, count, last), False)

    # ── ⛔ 主題關掉 page_indicator:右端不得變成空白 ─────────────────────
    # 上一版在 `v_pager >= page_count` 時無條件回 'pager',不看那一組畫不畫得
    # 出來。`PageArrows` 在 `show == false` 時直接 return —— 於是「本頁排得下、
    # 後面還有頁」＋ `page_indicator.show: false` = 右端空白、鎖死在第 1 頁。
    no_pi = dict(bar, indicator_show=False)
    _v, end, dp, _n = solve(no_pi, 1080, 420, 40, 0, False, 3)
    check('關掉 page_indicator 而本頁看得完時右端是展開鍵', end, 'expand')
    check('  它真的佔一顆的寬度', dp, 40.0)
    check('  這一格不是死路', dead_end(end, _v, 3, False), False)

    # 兩條出口同時關掉 = 死路,必須看得見。
    trapped = dict(bar, indicator_show=False, expand_show=False)
    _v, end, dp, _n = solve(trapped, 1080, 420, 40, 0, False, 3)
    check('兩條出口都關掉時右端是空的', end, 'none')
    check('⛔ 而且那是死路(dead_end 必須抓到)', dead_end(end, _v, 3, False), True)
    # style: none 與 show: false 是同一件事。
    trapped2 = dict(bar, indicator_style='none', expand_show=False)
    _v2, end2, _dp2, _n2 = solve(trapped2, 1080, 420, 40, 0, False, 3)
    check('page_indicator.style: none 走同一條', dead_end(end2, _v2, 3, False), True)

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
    ap.add_argument('--panel-open', action='store_true',
                    help='展開面板現在開著(右端那一顆必須是收合鍵)')
    ap.add_argument('--which', default='')
    ap.add_argument('--item-width', type=int, default=0,
                    help='印出「N 字候選、無序號 / 有序號」兩種量測寬(dp),空白分隔')
    ap.add_argument('--self-test', action='store_true')
    args = ap.parse_args()

    root = os.path.abspath(args.root)
    if args.self_test:
        return self_test(root)

    dp = button_dp(root)
    bar = theme_bar(root, args.theme)
    screen_w = int(args.screen.split('x')[0])
    if args.item_width > 0:
        print('%.4f %.4f' % (item_width_dp(bar, args.item_width, 0),
                             item_width_dp(bar, args.item_width, 1)))
        return 0
    vis = solve(bar, screen_w, args.density, dp, args.page_no,
                bool(args.last_page), args.page_count,
                not args.no_candidates, args.panel_open)[0]
    if args.which == 'visible':
        print(vis)
        return 0
    if args.which == 'reserved':
        print(solve(bar, screen_w, args.density, dp, args.page_no,
                    bool(args.last_page), args.page_count,
                    not args.no_candidates, args.panel_open)[2])
        return 0
    order = buttons(bar, args.page_no, bool(args.last_page),
                    not args.no_candidates, vis, args.page_count,
                    screen_w, args.density, dp, args.panel_open)
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
