#!/usr/bin/env python3
"""候選列那一條帶子裡的**高亮塊**中心座標。找不到就什麼都不印。

候選列上唯一的飽和色塊就是高亮候選的底色(主題的 item.highlight_background)。
底色、文字、註解、翻頁箭頭全是灰階,所以「任一通道明顯高於其他兩個」這一條
只會命中高亮塊。

用它而不是寫死的 x:候選列左端有沒有那一格組字串會讓所有候選整條位移,
寫死的座標那時候會點在一塊按不動的文字上 —— 而「翻頁沒用」與「點錯地方」
在輸出上長得一模一樣(實測踩過)。
"""
import sys
from PIL import Image

src, top, bot = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
im = Image.open(src).convert("RGB")
top = max(0, min(top, im.height - 2))
bot = max(top + 2, min(bot, im.height))
crop = im.crop((0, top, im.width, bot))
px = crop.load()
xs, ys = [], []
for y in range(0, crop.height, 2):
    for x in range(0, crop.width, 2):
        r, g, b = px[x, y]
        mx, mn = max(r, g, b), min(r, g, b)
        if mx - mn > 60 and mx > 90:
            xs.append(x)
            ys.append(y)
if xs:
    print((min(xs) + max(xs)) // 2, top + (min(ys) + max(ys)) // 2)
