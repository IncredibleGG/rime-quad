package org.rimequad.ime.theme

/**
 * X11 keysym 名稱 → 數值。
 *
 * **這張表是快路徑，不是權威。** 權威是 librime 的 `RimeGetKeycodeByName()`
 * （見 third_party/librime/src/rime/key_table.cc）。本表只涵蓋內建佈局用得到的名稱，
 * 目的是讓解析與單元測試不需要載入 native library。
 *
 * 呼叫端的規範行為（docs/theme-format.md §9.4）：
 *   1. 先查本表；
 *   2. 查不到時**必須**回落到 `RimeGetKeycodeByName()`；
 *   3. 仍為 [VOID_SYMBOL] 時該鍵變成 noop + WARNING。
 *
 * ASCII 可見字元的 keysym 值等於其 ASCII 碼（0x20–0x7E），所以下表可由順序產生。
 */
object Keysym {

    const val VOID_SYMBOL: Int = 0xFFFFFF

    /** 0x20 起連續排列的 95 個 ASCII keysym 名稱（X11 標準名）。 */
    private val ASCII_NAMES = arrayOf(
        "space", "exclam", "quotedbl", "numbersign", "dollar", "percent", "ampersand",
        "apostrophe", "parenleft", "parenright", "asterisk", "plus", "comma", "minus",
        "period", "slash",
        "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
        "colon", "semicolon", "less", "equal", "greater", "question", "at",
        "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
        "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z",
        "bracketleft", "backslash", "bracketright", "asciicircum", "underscore", "grave",
        "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m",
        "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z",
        "braceleft", "bar", "braceright", "asciitilde"
    )

    private val TABLE: Map<String, Int> = buildTable()

    private fun buildTable(): Map<String, Int> {
        val m = HashMap<String, Int>(256)
        for (i in ASCII_NAMES.indices) m[ASCII_NAMES[i]] = 0x20 + i
        // X11 對同一碼位的別名
        m["quoteright"] = 0x27
        m["quoteleft"] = 0x60

        m["BackSpace"] = 0xFF08
        m["Tab"] = 0xFF09
        m["Linefeed"] = 0xFF0A
        m["Clear"] = 0xFF0B
        m["Return"] = 0xFF0D
        m["Pause"] = 0xFF13
        m["Scroll_Lock"] = 0xFF14
        m["Sys_Req"] = 0xFF15
        m["Escape"] = 0xFF1B
        m["Delete"] = 0xFFFF

        m["Home"] = 0xFF50
        m["Left"] = 0xFF51
        m["Up"] = 0xFF52
        m["Right"] = 0xFF53
        m["Down"] = 0xFF54
        m["Prior"] = 0xFF55
        m["Page_Up"] = 0xFF55
        m["Next"] = 0xFF56
        m["Page_Down"] = 0xFF56
        m["End"] = 0xFF57
        m["Begin"] = 0xFF58
        m["Insert"] = 0xFF63
        m["Menu"] = 0xFF67

        m["KP_Enter"] = 0xFF8D
        m["KP_Multiply"] = 0xFFAA
        m["KP_Add"] = 0xFFAB
        m["KP_Subtract"] = 0xFFAD
        m["KP_Decimal"] = 0xFFAE
        m["KP_Divide"] = 0xFFAF
        for (d in 0..9) m["KP_$d"] = 0xFFB0 + d

        for (f in 1..12) m["F$f"] = 0xFFBD + f

        m["Shift_L"] = 0xFFE1
        m["Shift_R"] = 0xFFE2
        m["Control_L"] = 0xFFE3
        m["Control_R"] = 0xFFE4
        m["Caps_Lock"] = 0xFFE5
        m["Alt_L"] = 0xFFE9
        m["Alt_R"] = 0xFFEA
        m["Super_L"] = 0xFFEB
        m["Super_R"] = 0xFFEC
        return m
    }

    /** §9.4 的解析順序 1–3。查不到回傳 [VOID_SYMBOL]。 */
    fun resolve(spec: String): Int {
        val t = spec.trim()
        if (t.isEmpty()) return VOID_SYMBOL

        // 1) U+XXXX
        if ((t.length in 6..8) && (t[0] == 'U' || t[0] == 'u') && t[1] == '+') {
            val cp = t.substring(2).toIntOrNull(16) ?: return VOID_SYMBOL
            return unicodeToKeysym(cp)
        }
        // 2) 數值。**只接受 0x 形式**：十進位會與 X11 的數字鍵名撞車
        //    —— "1" 是名稱（keysym 0x31），不是數值 1。見 §9.4。
        if (t.length > 2 && (t[0] == '0') && (t[1] == 'x' || t[1] == 'X')) {
            return t.substring(2).toIntOrNull(16) ?: VOID_SYMBOL
        }
        // 3) 名稱表
        return TABLE[t] ?: VOID_SYMBOL
    }

    fun unicodeToKeysym(codepoint: Int): Int =
        if (codepoint in 0x20..0xFF) codepoint else (0x01000000 or codepoint)

    fun isKnownName(name: String): Boolean = TABLE.containsKey(name)
}

/**
 * 修飾鍵位元，對應 core/include/rime_shell.h 的 `rs_modifier`。
 *
 * ⚠ 這些位元與 librime 內部的 `RimeModifier` **不同**（librime 的 Control 是 1<<2）。
 * 轉換是 rime_shell 那一層的責任，佈局檔只使用下列名稱。
 */
object Modifiers {
    const val NONE = 0
    const val SHIFT = 1 shl 0
    const val CONTROL = 1 shl 1
    const val ALT = 1 shl 2
    const val SUPER = 1 shl 3
    const val CAPS = 1 shl 4

    private val NAMES = mapOf(
        "shift" to SHIFT,
        "control" to CONTROL,
        "ctrl" to CONTROL,
        "alt" to ALT,
        "super" to SUPER,
        "caps" to CAPS
    )

    /** 未知名稱回傳 null，由呼叫端產生 WARNING 並忽略該筆。 */
    fun byName(name: String): Int? = NAMES[name.trim().lowercase()]

    fun format(mask: Int): String {
        if (mask == 0) return ""
        val parts = ArrayList<String>()
        if (mask and SHIFT != 0) parts.add("Shift")
        if (mask and CONTROL != 0) parts.add("Control")
        if (mask and ALT != 0) parts.add("Alt")
        if (mask and SUPER != 0) parts.add("Super")
        if (mask and CAPS != 0) parts.add("Caps")
        return parts.joinToString("+")
    }
}
