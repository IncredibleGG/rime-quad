#!/usr/bin/env python3
"""兩張截圖在同一塊矩形區域裡差了多少像素。

用法:region_changed.py <a.png> <b.png> <x0> <y0> <x1> <y1> [out.png]
輸出一行:`<不同的像素數> <總像素數> <千分比>`

為什麼需要它
─────────────────────────────────────────────────────────────────────────
`verify_candbar.sh` 要分辨候選列右端那一點是**翻頁鍵**還是**展開鍵**。
兩者都「點下去不上屏任何東西」,所以只看輸入框分不出來 —— 那正是這一支
腳本吃過的虧(它整輪點的是展開鍵,而正控照樣綠)。

分得出來的是畫面:
  · 翻頁鍵  只換候選列的內容,**鍵盤格線區一個像素都不動**
  · 展開鍵  把一片面板蓋在格線區上,那一塊會大面積改變

差異用「亮度差 > 24 的像素數」算,不是逐位元組比對:模擬器的截圖在同一
畫面上也會有幾個像素的抖動(抗鋸齒、游標閃爍),逐位元組比對會把那個
當成「變了」。24 遠小於一塊面板蓋上去的變化量。
"""
import sys
from PIL import Image

a, b = sys.argv[1], sys.argv[2]
x0, y0, x1, y1 = map(int, sys.argv[3:7])
out = sys.argv[7] if len(sys.argv) > 7 else ""

ia = Image.open(a).convert("L")
ib = Image.open(b).convert("L")
w = min(ia.width, ib.width)
h = min(ia.height, ib.height)
x0 = max(0, min(x0, w - 1))
x1 = max(x0 + 1, min(x1, w))
y0 = max(0, min(y0, h - 1))
y1 = max(y0 + 1, min(y1, h))
box = (x0, y0, x1, y1)
da = ia.crop(box).tobytes()
db = ib.crop(box).tobytes()
n = min(len(da), len(db))
diff = sum(1 for i in range(n) if abs(da[i] - db[i]) > 24)
if out:
    from PIL import ImageChops
    ImageChops.difference(ia.crop(box), ib.crop(box)).save(out)
print(diff, n, (diff * 1000) // n if n else 0)
