package org.rimequad.ime.core

/**
 * X11 keysym 常數（librime 沿用 X11 的定義）。
 *
 * rime_shell.h 檔頭提到：桌面端得自己做「原生鍵碼 → keysym」映射，
 * 而行動端因為鍵盤是自繪的，可以由佈局直接指定 keysym，繞開整個問題。
 * 這裡就是那份「佈局直接給 keysym」的常數表。
 *
 * 之後接上 core/layouts 目錄的 yaml 時，YAML 應該直接寫 keysym 名稱，
 * 由解析器查這張表，而不是再發明一套鍵碼。
 */
object RimeKeysym {
    const val BACKSPACE = 0xFF08
    const val TAB = 0xFF09
    const val RETURN = 0xFF0D
    const val ESCAPE = 0xFF1B
    const val SPACE = 0x0020

    const val HOME = 0xFF50
    const val LEFT = 0xFF51
    const val UP = 0xFF52
    const val RIGHT = 0xFF53
    const val DOWN = 0xFF54
    const val PAGE_UP = 0xFF55
    const val PAGE_DOWN = 0xFF56
    const val END = 0xFF57
    const val DELETE = 0xFFFF

    const val SHIFT_L = 0xFFE1
    const val CONTROL_L = 0xFFE3

    /** ASCII 可見字元的 keysym 與其碼位相同（0x20..0x7E）。 */
    fun ofAsciiChar(c: Char): Int = c.code
}

/** 對應 `rs_modifier`。 */
object RimeModifier {
    const val NONE = 0
    const val SHIFT = 1 shl 0
    const val CONTROL = 1 shl 1
    const val ALT = 1 shl 2
    const val SUPER = 1 shl 3
    const val CAPS = 1 shl 4
    const val RELEASE = 1 shl 5
}
