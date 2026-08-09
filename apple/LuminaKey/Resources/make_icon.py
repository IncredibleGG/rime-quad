#!/usr/bin/env python3
"""產生 app 圖示與選單列圖示的來源 PNG。

⚠ 這支腳本這一輪被整個改寫,原因是真機回報:「輸入來源」清單裡的圖示是
   一塊空白方框。原本的版本手寫 TIFF 的 IFD,少了 `RowsPerStrip`(tag 278)
   與 `PlanarConfiguration`(tag 284)—— 那份 TIFF 在多數解碼器上讀得出來,
   在 ImageIO 上讀不出來,而**讀不出來的症狀就是一塊空白,沒有錯誤訊息**。

   而且「輸入來源」清單顯示的其實是 **app 圖示**(`CFBundleIconFile`),
   當時的 bundle 裡根本沒有 `.icns`。手寫 TIFF 修好也不會讓那一格有東西。

改成:本腳本只產生 **PNG**(格式簡單、用 zlib 就寫得出來、而且每一個
解碼器都讀得懂),`.icns` 與多解析度 `.tiff` 交給 macOS 自己的
`iconutil` 與 `tiffutil` 組 —— 那兩支是系統自帶的,產出格式一定是對的。

刻意不把二進位圖檔入版控的理由不變:版控裡的二進位沒人 review 得動。
"""
import struct
import sys
import zlib

# 深藍底 + 白色注音「ㄅ」形的筆畫。刻意不用文字渲染 ——
# runner 上沒有保證存在的中文字型,而缺字的結果是一塊空白(又一次)。
BG = (0x1F, 0x3A, 0x6E, 0xFF)
FG = (0xFF, 0xFF, 0xFF, 0xFF)
CLEAR = (0, 0, 0, 0)


def draw(size):
    """回傳 size×size 的 RGBA 位元組。"""
    px = bytearray()
    r = size * 0.22                      # 圓角半徑
    # 「ㄅ」的兩筆:一橫(上方)+ 一豎折(右上往下再往左)。
    stroke = max(1, round(size * 0.085))
    x0, x1 = size * 0.26, size * 0.74
    y_top = size * 0.30
    y_bot = size * 0.72
    x_hook = size * 0.40

    for y in range(size):
        for x in range(size):
            fx, fy = x + 0.5, y + 0.5
            # 圓角矩形:離最近的角超過半徑就切掉。
            cx = min(fx, size - fx)
            cy = min(fy, size - fy)
            if cx < r and cy < r and (r - cx) ** 2 + (r - cy) ** 2 > r * r:
                px += bytes(CLEAR)
                continue
            on = False
            # 上面那一橫
            if x0 <= fx <= x1 and abs(fy - y_top) <= stroke / 2:
                on = True
            # 右邊那一豎
            if abs(fx - x1) <= stroke / 2 and y_top <= fy <= y_bot:
                on = True
            # 底下往左的鉤
            if abs(fy - y_bot) <= stroke / 2 and x_hook <= fx <= x1:
                on = True
            # 左邊短豎(讓它不只是一個「フ」)
            if abs(fx - x0) <= stroke / 2 and y_top <= fy <= y_top + size * 0.26:
                on = True
            px += bytes(FG if on else BG)
    return bytes(px)


def png(size, rgba):
    """最小可用的 PNG。用 zlib,所以不必手寫壓縮。"""
    raw = bytearray()
    stride = size * 4
    for y in range(size):
        raw.append(0)                    # filter type 0(None)
        raw += rgba[y * stride:(y + 1) * stride]

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)   # 8-bit RGBA
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", ihdr)
            + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
            + chunk(b"IEND", b""))


def main():
    if len(sys.argv) != 3:
        print("用法: make_icon.py <輸出目錄> <尺寸,逗號分隔>", file=sys.stderr)
        return 2
    out_dir, sizes = sys.argv[1], sys.argv[2]
    for s in [int(v) for v in sizes.split(",")]:
        with open(f"{out_dir}/icon_{s}.png", "wb") as f:
            f.write(png(s, draw(s)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
