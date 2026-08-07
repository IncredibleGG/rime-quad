package org.rimequad.ime.theme

/**
 * 顏色字面值的解析（docs/theme-format.md §4.7）。
 *
 * 檔案中的順序是 **RGB / RGBA**；本層一律轉成 Android 慣用的 **ARGB Int**。
 * 這個轉換點只有這裡一處，其餘程式碼看到的都是 ARGB。
 */
object ColorSpec {

    const val TRANSPARENT: Int = 0x00000000

    fun rgba(r: Int, g: Int, b: Int, a: Int): Int =
        ((a and 0xFF) shl 24) or ((r and 0xFF) shl 16) or ((g and 0xFF) shl 8) or (b and 0xFF)

    fun alphaOf(argb: Int): Int = (argb ushr 24) and 0xFF

    /** 解析一個顏色字面值；無法解析回傳 null（由呼叫端決定退回哪個預設值）。 */
    fun resolve(spec: String, palette: Map<String, Int>): Int? {
        val t = spec.trim()
        if (t.isEmpty()) return null
        if (t.equals("transparent", ignoreCase = true)) return TRANSPARENT
        if (t[0] == '#') return parseHex(t)
        if (t[0] == '$') return resolveRef(t, palette)
        return null
    }

    private fun resolveRef(t: String, palette: Map<String, Int>): Int? {
        val at = t.indexOf('@')
        val name = (if (at < 0) t.substring(1) else t.substring(1, at)).trim()
        if (name.isEmpty()) return null
        val base = palette[name] ?: return null
        if (at < 0) return base
        val factor = parseAlphaFactor(t.substring(at + 1).trim()) ?: return null
        val scaled = alphaOf(base).toFloat() * factor
        var a = (scaled + 0.5f).toInt()
        if (a < 0) a = 0
        if (a > 255) a = 255
        return (a shl 24) or (base and 0x00FFFFFF)
    }

    fun parseHex(t: String): Int? {
        val h = t.substring(1)
        for (c in h) if (!isHexDigit(c)) return null
        val sb = StringBuilder(8)
        when (h.length) {
            3 -> {
                sb.append(h[0]).append(h[0]).append(h[1]).append(h[1]).append(h[2]).append(h[2])
                sb.append("ff")
            }
            4 -> {
                sb.append(h[0]).append(h[0]).append(h[1]).append(h[1])
                sb.append(h[2]).append(h[2]).append(h[3]).append(h[3])
            }
            6 -> sb.append(h).append("ff")
            8 -> sb.append(h)
            else -> return null
        }
        val e = sb.toString()
        val r = e.substring(0, 2).toInt(16)
        val g = e.substring(2, 4).toInt(16)
        val b = e.substring(4, 6).toInt(16)
        val a = e.substring(6, 8).toInt(16)
        return rgba(r, g, b, a)
    }

    /** `@30%` 或 `@0.3`。回傳 0..1 的乘數。 */
    fun parseAlphaFactor(s: String): Float? {
        if (s.isEmpty()) return null
        if (s.endsWith("%")) {
            val v = s.substring(0, s.length - 1).trim().toFloatOrNull() ?: return null
            if (v < 0f || v > 100f) return null
            return v / 100f
        }
        val v = s.toFloatOrNull() ?: return null
        if (v < 0f || v > 1f) return null
        return v
    }

    private fun isHexDigit(c: Char): Boolean =
        (c in '0'..'9') || (c in 'a'..'f') || (c in 'A'..'F')

    /** 除錯用的 `#RRGGBBAA` 表示。 */
    fun format(argb: Int): String {
        val r = (argb ushr 16) and 0xFF
        val g = (argb ushr 8) and 0xFF
        val b = argb and 0xFF
        val a = (argb ushr 24) and 0xFF
        return "#" + hex2(r) + hex2(g) + hex2(b) + hex2(a)
    }

    private fun hex2(v: Int): String {
        val s = Integer.toHexString(v)
        return if (s.length == 1) "0$s" else s
    }
}

/**
 * palette 的解析，含遞迴 `$ref`（深度上限 8）。
 * 無法解析的條目視為**不存在** —— 指向它的欄位會退回各自的預設值 + WARNING。
 */
internal object PaletteResolver {

    private const val MAX_DEPTH = 8

    fun resolve(cursor: Cursor): Map<String, Int> {
        val raw = LinkedHashMap<String, String>()
        val node = cursor.node as? YamlNode.Mapping ?: return emptyMap()
        for ((k, v) in node.entries) {
            val s = (v as? YamlNode.Scalar)?.value
            if (s == null) {
                cursor.diag.warn("palette.$k", "expected a color literal; entry dropped", v.line)
                continue
            }
            raw[k] = s
        }
        val out = LinkedHashMap<String, Int>()
        for (k in raw.keys) {
            val v = resolveOne(k, raw, out, 0, cursor)
            if (v != null) out[k] = v
        }
        return out
    }

    private fun resolveOne(
        name: String,
        raw: Map<String, String>,
        done: MutableMap<String, Int>,
        depth: Int,
        cursor: Cursor
    ): Int? {
        done[name]?.let { return it }
        if (depth >= MAX_DEPTH) {
            cursor.diag.warn("palette.$name", "reference chain too deep or cyclic; entry dropped")
            return null
        }
        val spec = raw[name] ?: return null
        val t = spec.trim()
        if (t.startsWith("$")) {
            val at = t.indexOf('@')
            val target = (if (at < 0) t.substring(1) else t.substring(1, at)).trim()
            if (target == name) {
                cursor.diag.warn("palette.$name", "self-reference; entry dropped")
                return null
            }
            val base = resolveOne(target, raw, done, depth + 1, cursor) ?: run {
                cursor.diag.warn("palette.$name", "unresolved reference '\$$target'; entry dropped")
                return null
            }
            val single = mapOf(target to base)
            val v = ColorSpec.resolve(t, single)
            if (v == null) cursor.diag.warn("palette.$name", "invalid color '$t'; entry dropped")
            return v
        }
        val v = ColorSpec.resolve(t, emptyMap())
        if (v == null) cursor.diag.warn("palette.$name", "invalid color '$t'; entry dropped")
        return v
    }
}
