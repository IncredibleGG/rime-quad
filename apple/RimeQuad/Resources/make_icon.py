#!/usr/bin/env python3
"""產生選單列圖示。

沒有圖示時，輸入法在「輸入來源」清單裡是一塊空白 —— 使用者選不到，
而且看起來像壞掉的安裝。

刻意寫成 20 行的產生器而不是把二進位圖檔入版控：
  · 版控裡的二進位沒人 review 得動，改了也看不出差別；
  · 顏色與尺寸是程式碼，之後要跟主題的 accent 對齊時改一個常數就好。

輸出是 TIFF（Info.plist 的 tsInputModeMenuIconFileKey 指向它）。
TIFF 的 header 手寫，避免相依 Pillow —— runner 上不保證有。
"""
import struct
import sys

SIZE = 32
# 深灰底 + 白色「口」字框：在淺色與深色選單列上都看得見。
BG = (0x33, 0x33, 0x33, 0xFF)
FG = (0xFF, 0xFF, 0xFF, 0xFF)
CLEAR = (0, 0, 0, 0)


def pixels():
    rows = []
    r = SIZE // 5           # 圓角半徑（用曼哈頓距離近似，夠用）
    for y in range(SIZE):
        row = bytearray()
        for x in range(SIZE):
            # 圓角矩形
            cx = min(x, SIZE - 1 - x)
            cy = min(y, SIZE - 1 - y)
            corner = cx < r and cy < r and (r - cx) ** 2 + (r - cy) ** 2 > r * r
            if corner:
                row += bytes(CLEAR)
                continue
            inner = 8 <= x < SIZE - 8 and 8 <= y < SIZE - 8
            border = (6 <= x < SIZE - 6 and 6 <= y < SIZE - 6) and not inner
            row += bytes(FG if border else BG)
        rows.append(bytes(row))
    return b"".join(rows)


def tiff(data):
    # 最小可用的 TIFF：8 個 IFD 條目，RGBA、未壓縮、單一 strip。
    header = struct.pack("<2sHI", b"II", 42, 8)
    # IFD：2 bytes 條目數 + 12 bytes/條目 + 4 bytes 下一個 IFD 的 offset。
    ifd_end = 8 + 2 + 12 * 9 + 4
    bits_offset = ifd_end                 # BitsPerSample 的 4×2 bytes 緊接在 IFD 之後
    offset_data = ifd_end + 8
    entries = [
        (256, 3, 1, SIZE),                 # ImageWidth
        (257, 3, 1, SIZE),                 # ImageLength
        (258, 3, 4, bits_offset),          # BitsPerSample -> 8,8,8,8
        (259, 3, 1, 1),                    # Compression = none
        (262, 3, 1, 2),                    # Photometric = RGB
        (273, 4, 1, offset_data),          # StripOffsets
        (277, 3, 1, 4),                    # SamplesPerPixel
        (279, 4, 1, len(data)),            # StripByteCounts
        (338, 3, 1, 2),                    # ExtraSamples = unassociated alpha
    ]
    out = bytearray(header)
    out += struct.pack("<H", len(entries))
    for tag, typ, count, value in entries:
        if typ == 3 and count == 1:
            out += struct.pack("<HHI HH", tag, typ, count, value, 0)
        else:
            out += struct.pack("<HHII", tag, typ, count, value)
    out += struct.pack("<I", 0)            # next IFD = 0
    out += struct.pack("<4H", 8, 8, 8, 8)  # BitsPerSample
    assert len(out) == offset_data, (len(out), offset_data)
    out += data
    return bytes(out)


def main():
    if len(sys.argv) != 2:
        print("用法: make_icon.py <輸出.tiff>", file=sys.stderr)
        return 2
    with open(sys.argv[1], "wb") as f:
        f.write(tiff(pixels()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
