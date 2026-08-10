#!/usr/bin/env python3
"""一塊矩形區域裡有多少「墨跡」像素,並把那一塊另存成圖供 artifact 使用。

用法:count_ink.py <png> <x0> <y0> <x1> <y1> <out.png>
輸出一行:`<墨跡像素數> <總像素數>`

「墨跡」= 與該區域**眾數亮度**(也就是底色)相差超過 48 的像素。
用眾數而不是寫死的顏色:淺色主題底色接近白、深色主題接近黑,兩邊都要能用。

為什麼是數像素而不是 OCR:這一關要問的是「這裡**有沒有東西**」,
而 OCR 的答案會隨螢幕尺寸、字級、語言包而變 —— CI 的模擬器是 1080x2400、
開發機是 1440x3120,同一段 OCR 前處理在兩邊讀出來的東西完全不同。
有沒有墨跡是像素等級的事實,兩邊一樣。
"""
import sys
from collections import Counter
from PIL import Image

src, x0, y0, x1, y1, out = sys.argv[1], *map(int, sys.argv[2:6]), sys.argv[6]
im = Image.open(src).convert("L")
x0 = max(0, min(x0, im.width - 1))
x1 = max(x0 + 1, min(x1, im.width))
y0 = max(0, min(y0, im.height - 1))
y1 = max(y0 + 1, min(y1, im.height))
crop = im.crop((x0, y0, x1, y1))
crop.save(out)
data = list(crop.tobytes())
if not data:
    print(0, 0)
    sys.exit(0)
bg = Counter(data).most_common(1)[0][0]
ink = sum(1 for v in data if abs(v - bg) > 48)
print(ink, len(data))
