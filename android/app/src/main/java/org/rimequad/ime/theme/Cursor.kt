package org.rimequad.ime.theme

import org.rimequad.ime.theme.DiagnosticCode.BAD_BOOL
import org.rimequad.ime.theme.DiagnosticCode.BAD_COLOR
import org.rimequad.ime.theme.DiagnosticCode.BAD_ENUM
import org.rimequad.ime.theme.DiagnosticCode.BAD_NUMBER
import org.rimequad.ime.theme.DiagnosticCode.ENTRY_DROPPED
import org.rimequad.ime.theme.DiagnosticCode.OUT_OF_RANGE
import org.rimequad.ime.theme.DiagnosticCode.TYPE_MISMATCH
import org.rimequad.ime.theme.DiagnosticCode.UNKNOWN_FIELD

/**
 * 解析情境。palette 在管線第 7 步填入，之後所有 color 欄位才解析（§7.4）。
 */
class ParseContext(val diagnostics: Diagnostics) {
    var palette: Map<String, Int> = emptyMap()
    var locale: String = "en"
}

/**
 * 帶路徑與診斷的節點游標。
 *
 * 所有取值器都遵守 docs/theme-format.md §6.3：
 * 欄位缺失 → 靜默採預設值；型別錯 → 預設值 + WARNING；超出範圍 → 夾制 + WARNING。
 * **沒有任何取值器會拋例外。** 致命錯誤只由 §6.2 的清單產生。
 */
class Cursor internal constructor(
    val node: YamlNode?,
    val path: String,
    val ctx: ParseContext
) {
    val diag: Diagnostics get() = ctx.diagnostics

    val exists: Boolean get() = node != null

    val isNull: Boolean get() = node == null || (node is YamlNode.Scalar && node.value == null)

    companion object {
        fun root(node: YamlNode?, ctx: ParseContext): Cursor = Cursor(node, "", ctx)
    }

    private fun childPath(key: String): String = if (path.isEmpty()) key else "$path.$key"

    fun child(key: String): Cursor {
        val m = node as? YamlNode.Mapping ?: return Cursor(null, childPath(key), ctx)
        return Cursor(m.entries[key], childPath(key), ctx)
    }

    /** 取子映射；若節點存在但不是映射，產生 WARNING 並退化為「不存在」。 */
    fun mapping(key: String): Cursor = child(key).asMapping()

    fun asMapping(): Cursor {
        val n = node ?: return this
        if (n is YamlNode.Mapping) return this
        if (n is YamlNode.Scalar && n.value == null) return Cursor(null, path, ctx)
        diag.add(TYPE_MISMATCH, path, n.line, listOf(DiagnosticTerm.MAPPING.id, kindOf(n)))
        return Cursor(null, path, ctx)
    }

    /** 映射的 key 清單（保持文件順序）。 */
    fun keys(): List<String> = (node as? YamlNode.Mapping)?.entries?.keys?.toList() ?: emptyList()

    /** 序列的元素。節點不是序列時回傳空清單並產生 WARNING（null / 缺失則靜默）。 */
    fun items(): List<Cursor> {
        val n = node ?: return emptyList()
        if (n is YamlNode.Scalar && n.value == null) return emptyList()
        if (n !is YamlNode.Sequence) {
            diag.add(TYPE_MISMATCH, path, n.line, listOf(DiagnosticTerm.SEQUENCE.id, kindOf(n)))
            return emptyList()
        }
        return n.items.mapIndexed { i, item -> Cursor(item, "$path[$i]", ctx) }
    }

    // ───────────────────────── 純量取值 ─────────────────────────

    private fun scalarText(): String? {
        val n = node ?: return null
        if (n is YamlNode.Scalar) return n.value
        diag.add(TYPE_MISMATCH, path, n.line, listOf(DiagnosticTerm.SCALAR.id, kindOf(n)))
        return null
    }

    fun string(default: String): String = scalarText() ?: default

    fun stringOrNull(): String? = scalarText()

    fun bool(default: Boolean): Boolean {
        val s = scalarText() ?: return default
        return when (s.trim().lowercase()) {
            "true", "yes", "on", "1" -> true
            "false", "no", "off", "0" -> false
            else -> {
                diag.add(BAD_BOOL, path, node?.line, listOf(s))
                default
            }
        }
    }

    /**
     * §4.2 / §6.3：超出範圍是**夾制 + WARNING**，不是靜默夾制。
     *
     * 使用者寫了 4.0 卻拿到 0.6，不告知是說不過去的；而且 §10 檢核第 9 條要求
     * 四端對同一份壞檔案報出同樣多則診斷，靜默夾制會讓那一條直接失守。
     * 純粹的夾制輔助函式 [clampInt] / [clampFloat] 仍然安靜，它們用於
     * 執行期計算（如 HeightSpec.resolve），那裡沒有「使用者寫錯」的語義。
     */
    private fun reportClamp(value: String, bound: String, min: String, max: String) {
        diag.add(OUT_OF_RANGE, path, node?.line, listOf(value, min, max, bound))
    }

    fun int(default: Int, min: Int = Int.MIN_VALUE, max: Int = Int.MAX_VALUE): Int {
        val s = scalarText() ?: return default
        val v = s.trim().toIntOrNull() ?: s.trim().toDoubleOrNull()?.toInt()
        if (v == null) {
            diag.add(BAD_NUMBER, path, node?.line, listOf(s))
            return default
        }
        if (v < min) {
            reportClamp(v.toString(), min.toString(), min.toString(), max.toString())
            return min
        }
        if (v > max) {
            reportClamp(v.toString(), max.toString(), min.toString(), max.toString())
            return max
        }
        return v
    }

    fun number(default: Float, min: Float = -Float.MAX_VALUE, max: Float = Float.MAX_VALUE): Float {
        val s = scalarText() ?: return default
        val v = s.trim().toFloatOrNull()
        if (v == null || v.isNaN() || v.isInfinite()) {
            diag.add(BAD_NUMBER, path, node?.line, listOf(s))
            return default
        }
        if (v < min) {
            reportClamp(v.toString(), min.toString(), min.toString(), max.toString())
            return min
        }
        if (v > max) {
            reportClamp(v.toString(), max.toString(), min.toString(), max.toString())
            return max
        }
        return v
    }

    /** 長度（dp）。見 §4.3：一律無單位數字。 */
    fun length(default: Float, min: Float = 0f, max: Float = 4096f): Float = number(default, min, max)

    /** 文字尺寸（sp）。見 §4.4。 */
    fun size(default: Float, min: Float = 1f, max: Float = 200f): Float = number(default, min, max)

    /** 比例 0..1。見 §4.5。 */
    fun ratio(default: Float, min: Float = 0f, max: Float = 1f): Float = number(default, min, max)

    /** 毫秒。見 §4.6。 */
    fun duration(default: Int): Int = int(default, 0, 5000)

    fun enumOf(allowed: List<String>, default: String): String {
        val s = scalarText() ?: return default
        val t = s.trim()
        for (a in allowed) if (a.equals(t, ignoreCase = true)) return a
        diag.add(BAD_ENUM, path, node?.line, listOf(t, allowed.joinToString("/"), default))
        return default
    }

    /** 字串清單。單一字串等價於單元素清單（§4.10）。 */
    fun stringList(default: List<String>): List<String> {
        val n = node ?: return default
        if (n is YamlNode.Scalar) {
            val v = n.value ?: return default
            return listOf(v)
        }
        if (n !is YamlNode.Sequence) {
            diag.add(TYPE_MISMATCH, path, n.line, listOf(DiagnosticTerm.STRING_LIST.id, kindOf(n)))
            return default
        }
        val out = ArrayList<String>(n.items.size)
        for ((i, item) in n.items.withIndex()) {
            val s = (item as? YamlNode.Scalar)?.value
            if (s == null) {
                diag.add(ENTRY_DROPPED, "$path[$i]", item.line)
                continue
            }
            out.add(s)
        }
        return out
    }

    /** 顏色。見 §4.7。解析失敗一律採欄位預設值 —— 不使用洋紅等除錯色。 */
    fun color(default: Int): Int {
        val s = scalarText() ?: return default
        val v = ColorSpec.resolve(s, ctx.palette)
        if (v == null) {
            diag.add(BAD_COLOR, path, node?.line, listOf(s))
            return default
        }
        return v
    }

    fun localized(): LocalizedString {
        val n = node ?: return LocalizedString.EMPTY
        if (n is YamlNode.Scalar) {
            val v = n.value ?: return LocalizedString.EMPTY
            return LocalizedString(linkedMapOf("und" to v))
        }
        if (n !is YamlNode.Mapping) {
            diag.add(
                TYPE_MISMATCH, path, n.line,
                listOf(DiagnosticTerm.LOCALIZED_STRING.id, kindOf(n))
            )
            return LocalizedString.EMPTY
        }
        val map = LinkedHashMap<String, String>()
        for ((k, v) in n.entries) {
            val s = (v as? YamlNode.Scalar)?.value
            if (s == null) {
                diag.add(ENTRY_DROPPED, "$path.$k", v.line)
                continue
            }
            map[k] = s
        }
        return LocalizedString(map)
    }

    // ───────────────────────── 未知欄位 ─────────────────────────

    /**
     * §6.3：未知欄位 → 忽略 + WARNING，附上最接近的已知欄位名。
     *
     * 規範 §6.5.1 的 `unknown_field` 允許 `[field]` 與 `[field, suggestion]`
     * 兩種參數，各對應一份樣板 —— 猜不出來時不要硬塞一個空字串進去，
     * 那會在畫面上變成「你要寫的是「」嗎？」。
     */
    fun warnUnknownKeys(known: Set<String>) {
        val m = node as? YamlNode.Mapping ?: return
        for ((k, v) in m.entries) {
            if (known.contains(k)) continue
            val hint = closestKey(k, known)
            val args = if (hint != null) listOf(k, hint) else listOf(k)
            diag.add(UNKNOWN_FIELD, childPath(k), v.line, args)
        }
    }

    /**
     * `type_mismatch` 的 `found` 參數。**回傳的是 [DiagnosticTerm] 的穩定代號，
     * 不是給人看的字** —— 客戶端各自把它翻成當地語言（§6.5：args 是填進樣板的
     * 參數，不是訊息本身）。
     */
    private fun kindOf(n: YamlNode): String = when (n) {
        is YamlNode.Scalar ->
            if (n.value == null) DiagnosticTerm.NOTHING.id else DiagnosticTerm.SCALAR.id
        is YamlNode.Mapping -> DiagnosticTerm.MAPPING.id
        is YamlNode.Sequence -> DiagnosticTerm.SEQUENCE.id
    }
}

internal fun clampInt(v: Int, min: Int, max: Int): Int = if (v < min) min else if (v > max) max else v

internal fun clampFloat(v: Float, min: Float, max: Float): Float =
    if (v < min) min else if (v > max) max else v

/** 編輯距離 <= 2 的最近候選，用於拼字錯誤的診斷訊息。 */
internal fun closestKey(key: String, known: Set<String>): String? {
    var best: String? = null
    var bestD = 3
    for (k in known) {
        val d = editDistance(key, k)
        if (d < bestD) {
            bestD = d
            best = k
        }
    }
    return best
}

internal fun editDistance(a: String, b: String): Int {
    if (a == b) return 0
    if (a.isEmpty()) return b.length
    if (b.isEmpty()) return a.length
    var prev = IntArray(b.length + 1) { it }
    var cur = IntArray(b.length + 1)
    for (i in 1..a.length) {
        cur[0] = i
        for (j in 1..b.length) {
            val cost = if (a[i - 1] == b[j - 1]) 0 else 1
            var m = prev[j] + 1
            if (cur[j - 1] + 1 < m) m = cur[j - 1] + 1
            if (prev[j - 1] + cost < m) m = prev[j - 1] + cost
            cur[j] = m
        }
        val tmp = prev
        prev = cur
        cur = tmp
    }
    return prev[b.length]
}

/** 依 BCP-47 標籤查詢的在地化字串（§4.9）。 */
class LocalizedString(val values: Map<String, String>) {

    companion object {
        val EMPTY = LocalizedString(emptyMap())
    }

    fun get(locale: String): String {
        if (values.isEmpty()) return ""
        val lc = locale.lowercase()
        for ((k, v) in values) if (k.lowercase() == lc) return v
        var cur = lc
        while (true) {
            val idx = cur.lastIndexOf('-')
            if (idx < 0) break
            cur = cur.substring(0, idx)
            for ((k, v) in values) if (k.lowercase() == cur) return v
        }
        val primary = lc.substringBefore('-')
        for ((k, v) in values) if (k.lowercase().substringBefore('-') == primary) return v
        values["en"]?.let { return it }
        values["und"]?.let { return it }
        return values.values.first()
    }

    val isEmpty: Boolean get() = values.isEmpty()

    override fun toString(): String = get("en")
}
