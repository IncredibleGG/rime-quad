package org.luminakey.ime.store

/**
 * MiniJson —— 索引檔用的零依賴 JSON 讀取器。
 *
 * 為什麼不用 org.json：
 *   · `org.json` 在 Android 上是系統提供的，但在 JVM 單元測試裡 android.jar 是
 *     stub，任何呼叫都會丟 "not mocked"。索引解析是本模組最需要被測試的部分，
 *     不能為了它額外拉一個 test-only 的 JSON 實作進來，那等於測的不是出貨的程式。
 *   · 專案 production 端刻意零依賴（見 gradle/libs.versions.toml 的註解），
 *     主題那條線已經自製了 MiniYaml，索引這條線照同一個原則辦。
 *
 * 相對 RFC 8259 的兩點差異，兩點都是**放寬**而非收緊：
 *   1. 接受行註解與區塊註解 —— docs/schema-store.md §1 的範例本身就是
 *      jsonc，若讀取器不接受註解，照著規範寫出來的索引反而讀不進來。
 *   2. 接受頂層任意值（不限 object / array）。
 *
 * 不放寬的部分：尾隨逗號、單引號字串、未加引號的鍵一律視為語法錯誤 ——
 * 索引是機器產生的，寬鬆只會讓伺服器側的 bug 晚一點被發現。
 */
sealed class Json {

    object Null : Json()

    data class Bool(val value: Boolean) : Json()

    /** [raw] 保留原始文字，讓 64 位元整數不會被 Double 磨掉精度。 */
    data class Num(val raw: String) : Json() {
        val asLong: Long? get() = raw.toLongOrNull() ?: raw.toDoubleOrNull()?.toLong()
        val asDouble: Double? get() = raw.toDoubleOrNull()
    }

    data class Str(val value: String) : Json()

    data class Arr(val items: List<Json>) : Json()

    data class Obj(val entries: Map<String, Json>) : Json()

    /* ── 取值輔助。全部「查不到就回 null / 預設值」，不丟例外 ── */

    operator fun get(key: String): Json? = (this as? Obj)?.entries?.get(key)

    val asArray: List<Json> get() = (this as? Arr)?.items ?: emptyList()

    val string: String? get() = (this as? Str)?.value

    val long: Long? get() = (this as? Num)?.asLong

    val boolean: Boolean? get() = (this as? Bool)?.value

    fun str(key: String): String? = this[key]?.string

    fun long(key: String): Long? = this[key]?.long

    fun bool(key: String): Boolean? = this[key]?.boolean

    fun arr(key: String): List<Json> = this[key]?.asArray ?: emptyList()

    fun strings(key: String): List<String> = arr(key).mapNotNull { it.string }
}

class JsonSyntaxException(val offset: Int, val detail: String) :
    RuntimeException("JSON 位移 $offset: $detail")

object MiniJson {

    fun parse(text: String): Json {
        val p = Parser(text)
        p.skipTrivia()
        val v = p.parseValue()
        p.skipTrivia()
        if (!p.atEnd()) p.fail("尾端有多餘內容")
        return v
    }

    /** 解析失敗時回 null，供「壞掉的快取當作沒有」這類場合使用。 */
    fun parseOrNull(text: String): Json? = runCatching { parse(text) }.getOrNull()

    private class Parser(private val s: String) {
        private var i = 0

        fun atEnd(): Boolean = i >= s.length

        fun fail(msg: String): Nothing = throw JsonSyntaxException(i, msg)

        fun skipTrivia() {
            while (i < s.length) {
                val c = s[i]
                when {
                    c == ' ' || c == '\t' || c == '\n' || c == '\r' -> i++
                    c == '/' && i + 1 < s.length && s[i + 1] == '/' -> {
                        while (i < s.length && s[i] != '\n') i++
                    }
                    c == '/' && i + 1 < s.length && s[i + 1] == '*' -> {
                        val end = s.indexOf("*/", i + 2)
                        if (end < 0) fail("區塊註解沒有結尾")
                        i = end + 2
                    }
                    else -> return
                }
            }
        }

        fun parseValue(): Json {
            if (atEnd()) fail("預期一個值，但已到結尾")
            return when (val c = s[i]) {
                '{' -> parseObject()
                '[' -> parseArray()
                '"' -> Json.Str(parseString())
                't' -> literal("true", Json.Bool(true))
                'f' -> literal("false", Json.Bool(false))
                'n' -> literal("null", Json.Null)
                else ->
                    if (c == '-' || c in '0'..'9') parseNumber()
                    else fail("無法解析的字元 '$c'")
            }
        }

        private fun literal(word: String, value: Json): Json {
            if (!s.startsWith(word, i)) fail("預期 '$word'")
            i += word.length
            return value
        }

        private fun parseObject(): Json {
            i++ // {
            val out = LinkedHashMap<String, Json>()
            skipTrivia()
            if (i < s.length && s[i] == '}') { i++; return Json.Obj(out) }
            while (true) {
                skipTrivia()
                if (i >= s.length || s[i] != '"') fail("物件的鍵必須是帶引號的字串")
                val key = parseString()
                skipTrivia()
                if (i >= s.length || s[i] != ':') fail("鍵之後預期 ':'")
                i++
                skipTrivia()
                out[key] = parseValue()
                skipTrivia()
                if (i >= s.length) fail("物件沒有結尾 '}'")
                when (s[i]) {
                    ',' -> { i++; skipTrivia(); if (i < s.length && s[i] == '}') fail("不接受尾隨逗號") }
                    '}' -> { i++; return Json.Obj(out) }
                    else -> fail("物件內預期 ',' 或 '}'")
                }
            }
        }

        private fun parseArray(): Json {
            i++ // [
            val out = ArrayList<Json>()
            skipTrivia()
            if (i < s.length && s[i] == ']') { i++; return Json.Arr(out) }
            while (true) {
                skipTrivia()
                out.add(parseValue())
                skipTrivia()
                if (i >= s.length) fail("陣列沒有結尾 ']'")
                when (s[i]) {
                    ',' -> { i++; skipTrivia(); if (i < s.length && s[i] == ']') fail("不接受尾隨逗號") }
                    ']' -> { i++; return Json.Arr(out) }
                    else -> fail("陣列內預期 ',' 或 ']'")
                }
            }
        }

        private fun parseString(): String {
            i++ // opening quote
            val sb = StringBuilder()
            while (true) {
                if (i >= s.length) fail("字串沒有結尾引號")
                when (val c = s[i]) {
                    '"' -> { i++; return sb.toString() }
                    '\\' -> {
                        i++
                        if (i >= s.length) fail("跳脫序列被截斷")
                        when (val e = s[i]) {
                            '"' -> sb.append('"')
                            '\\' -> sb.append('\\')
                            '/' -> sb.append('/')
                            'b' -> sb.append('\b')
                            'f' -> sb.append('\u000C')
                            'n' -> sb.append('\n')
                            'r' -> sb.append('\r')
                            't' -> sb.append('\t')
                            'u' -> {
                                if (i + 4 >= s.length) fail("\\u 跳脫被截斷")
                                val hex = s.substring(i + 1, i + 5)
                                val cp = hex.toIntOrNull(16) ?: fail("無效的 \\u 跳脫 '$hex'")
                                sb.append(cp.toChar())
                                i += 4
                            }
                            else -> fail("不支援的跳脫 '\\$e'")
                        }
                        i++
                    }
                    else -> {
                        if (c.code < 0x20) fail("字串內出現未跳脫的控制字元")
                        sb.append(c)
                        i++
                    }
                }
            }
        }

        private fun parseNumber(): Json {
            val start = i
            if (i < s.length && s[i] == '-') i++
            while (i < s.length && s[i] in '0'..'9') i++
            if (i < s.length && s[i] == '.') {
                i++
                while (i < s.length && s[i] in '0'..'9') i++
            }
            if (i < s.length && (s[i] == 'e' || s[i] == 'E')) {
                i++
                if (i < s.length && (s[i] == '+' || s[i] == '-')) i++
                while (i < s.length && s[i] in '0'..'9') i++
            }
            val raw = s.substring(start, i)
            if (raw.isEmpty() || raw == "-") fail("無效的數字")
            if (raw.toDoubleOrNull() == null) fail("無效的數字 '$raw'")
            return Json.Num(raw)
        }
    }
}
