package org.rimequad.ime.theme

/**
 * MiniYaml — RTS（Rime Theme Subset of YAML）的讀取器。
 *
 * 為什麼不用 snakeyaml-engine / kaml：見 docs/theme-format.md §3 與本次交付報告。
 * 摘要：
 *   · 本格式在規範層就把 YAML 限縮成一個小子集（無 anchor / alias / tag /
 *     多文件 / block scalar），因為四端要各自實作解析器，YAML 的邊角是行為分歧的源頭。
 *   · 既然子集已由規範釘死，執行期就不需要一個完整 YAML 實作。
 *   · 輸入法對 APK 體積與冷啟動時間敏感；這裡是 0 依賴、0 反射、單次線性掃描。
 *   · 關鍵正確性理由：本讀取器**不做隱式型別解析**，所有純量一律以原始字串交出，
 *     由綁定層依規範的欄位型別轉換。這消滅了 YAML 1.1 與 1.2 對
 *     `y` / `n` / `on` / `off` 的分歧 —— 若讀取層先把 `keysym: y` 變成布林，
 *     任何補救都太遲。
 *
 * 若日後子集被證明不夠用，替換點只有這一個檔案：把 [MiniYaml.parse] 換成
 * snakeyaml-engine 的包裝，只要它同樣回傳未經型別解析的 [YamlNode] 即可。
 */
sealed class YamlNode {
    abstract val line: Int

    /** 純量。[value] 為 null 代表 YAML 的 null（`null` / `~` / 空）。 */
    class Scalar(val value: String?, override val line: Int) : YamlNode()

    class Mapping(val entries: Map<String, YamlNode>, override val line: Int) : YamlNode()

    class Sequence(val items: List<YamlNode>, override val line: Int) : YamlNode()

    companion object {
        fun emptyMapping(): Mapping = Mapping(emptyMap(), 0)
    }
}

class YamlSyntaxException(
    val source: String,
    val line: Int,
    val detail: String
) : RuntimeException("$source:$line: $detail")

/**
 * 讀取層產生的診斷。**帶的是 code + args，不是句子** —— 這一層看不到 YAML
 * 路徑（它正在建那棵樹），所以 path 由 [DocumentLoader] 補成根層級。
 */
data class YamlWarning(val line: Int, val code: DiagnosticCode, val args: List<String>)

class YamlDocument(val root: YamlNode, val warnings: List<YamlWarning>)

internal object YamlEscapes {

    fun unescapeDouble(s: String, source: String, line: Int): String {
        if (s.indexOf('\\') < 0) return s
        val sb = StringBuilder(s.length)
        var i = 0
        while (i < s.length) {
            val c = s[i]
            if (c != '\\') {
                sb.append(c)
                i++
                continue
            }
            i++
            if (i >= s.length) throw YamlSyntaxException(source, line, "dangling backslash in quoted scalar")
            when (val e = s[i]) {
                'n' -> sb.append('\n')
                'r' -> sb.append('\r')
                't' -> sb.append('\t')
                '0' -> sb.append('\u0000')
                '\\' -> sb.append('\\')
                '"' -> sb.append('"')
                '\'' -> sb.append('\'')
                '/' -> sb.append('/')
                'u' -> {
                    if (i + 4 >= s.length) throw YamlSyntaxException(source, line, "truncated \\u escape")
                    val hex = s.substring(i + 1, i + 5)
                    val cp = hex.toIntOrNull(16)
                        ?: throw YamlSyntaxException(source, line, "invalid \\u escape '$hex'")
                    sb.append(cp.toChar())
                    i += 4
                }
                else -> throw YamlSyntaxException(source, line, "unsupported escape '\\$e'")
            }
            i++
        }
        return sb.toString()
    }

    fun unescapeSingle(s: String): String = s.replace("''", "'")
}

class MiniYaml private constructor(private val source: String, text: String) {

    private class Line(val no: Int, val indent: Int, val text: String)

    private val lines = ArrayList<Line>()
    private val warnings = ArrayList<YamlWarning>()
    private var pos = 0

    init {
        preprocess(text)
    }

    companion object {
        fun parse(source: String, text: String): YamlDocument = MiniYaml(source, text).run()
    }

    // ───────────────────────────── 前處理 ─────────────────────────────

    private fun preprocess(raw: String) {
        var normalized = raw.replace("\r\n", "\n").replace('\r', '\n')
        if (normalized.startsWith("\uFEFF")) normalized = normalized.substring(1)
        var lineNo = 0
        for (rawLine in normalized.split("\n")) {
            lineNo++
            val stripped = stripComment(rawLine)
            if (stripped.isBlank()) continue
            var indent = 0
            while (indent < stripped.length && stripped[indent] == ' ') indent++
            if (indent < stripped.length && stripped[indent] == '\t') {
                throw YamlSyntaxException(source, lineNo, "tabs are not allowed for indentation")
            }
            val content = stripped.substring(indent).trimEnd()
            if (content == "---") continue
            if (content == "...") break
            if (content.startsWith("&") || content.startsWith("*") || content.startsWith("<<")) {
                throw YamlSyntaxException(
                    source, lineNo,
                    "anchors, aliases and merge keys are not part of the Rime theme YAML subset"
                )
            }
            lines.add(Line(lineNo, indent, content))
        }
    }

    /** 去掉行內註解，但不動引號內的 '#'。 */
    private fun stripComment(line: String): String {
        var inSingle = false
        var inDouble = false
        var i = 0
        while (i < line.length) {
            val c = line[i]
            if (inDouble) {
                if (c == '\\') { i += 2; continue }
                if (c == '"') inDouble = false
            } else if (inSingle) {
                if (c == '\'') {
                    if (i + 1 < line.length && line[i + 1] == '\'') { i += 2; continue }
                    inSingle = false
                }
            } else {
                when (c) {
                    '"' -> inDouble = true
                    '\'' -> inSingle = true
                    '#' -> if (i == 0 || line[i - 1] == ' ' || line[i - 1] == '\t') return line.substring(0, i)
                    else -> {}
                }
            }
            i++
        }
        return line
    }

    // ───────────────────────────── 主流程 ─────────────────────────────

    private fun run(): YamlDocument {
        if (lines.isEmpty()) return YamlDocument(YamlNode.emptyMapping(), warnings)
        val root = parseBlockNode(lines[0].indent)
        if (pos < lines.size) {
            throw YamlSyntaxException(source, lines[pos].no, "unexpected indentation")
        }
        return YamlDocument(root, warnings)
    }

    private fun peek(): Line? = if (pos < lines.size) lines[pos] else null

    private fun advance(): Line = lines[pos++]

    private fun isSeqEntry(text: String): Boolean = text == "-" || text.startsWith("- ")

    private fun parseBlockNode(indent: Int): YamlNode {
        val l = peek() ?: return YamlNode.Scalar(null, 0)
        if (isSeqEntry(l.text)) return parseSequence(indent)
        // 整份文件（或整個子樹）也可以直接是一個流式集合，例如 `{}`。
        if (isFlowStart(l.text)) {
            advance()
            return parseScalarOrFlow(l.text, l.no)
        }
        return parseMapping(indent)
    }

    private fun parseMapping(indent: Int): YamlNode {
        val startLine = peek()?.no ?: 0
        val entries = LinkedHashMap<String, YamlNode>()
        while (true) {
            val l = peek() ?: break
            if (l.indent != indent) break
            if (isSeqEntry(l.text)) break
            advance()
            putEntry(entries, l, l.text, indent)
        }
        return YamlNode.Mapping(entries, startLine)
    }

    /** 序列項目 `- key: value` 的緊湊映射：第一筆已在 dash 之後，其餘在 contentIndent。 */
    private fun parseInlineMapping(first: Line, firstText: String, contentIndent: Int): YamlNode {
        val entries = LinkedHashMap<String, YamlNode>()
        putEntry(entries, first, firstText, contentIndent)
        while (true) {
            val l = peek() ?: break
            if (l.indent != contentIndent) break
            if (isSeqEntry(l.text)) break
            advance()
            putEntry(entries, l, l.text, contentIndent)
        }
        return YamlNode.Mapping(entries, first.no)
    }

    private fun parseSequence(indent: Int): YamlNode {
        val startLine = peek()?.no ?: 0
        val items = ArrayList<YamlNode>()
        while (true) {
            val l = peek() ?: break
            if (l.indent != indent || !isSeqEntry(l.text)) break
            advance()
            val afterDash = l.text.substring(1)
            val trimmed = afterDash.trimStart()
            val contentIndent = l.indent + 1 + (afterDash.length - trimmed.length)
            if (trimmed.isEmpty()) {
                val n = peek()
                if (n != null && n.indent > indent) items.add(parseBlockNode(n.indent))
                else items.add(YamlNode.Scalar(null, l.no))
            } else if (isFlowStart(trimmed) || topLevelColonIndex(trimmed) < 0) {
                items.add(parseScalarOrFlow(trimmed, l.no))
            } else {
                items.add(parseInlineMapping(l, trimmed, contentIndent))
            }
        }
        return YamlNode.Sequence(items, startLine)
    }

    private fun putEntry(
        entries: LinkedHashMap<String, YamlNode>,
        l: Line,
        text: String,
        indent: Int
    ) {
        val ci = topLevelColonIndex(text)
        if (ci < 0) throw YamlSyntaxException(source, l.no, "expected 'key: value', found '$text'")
        val rawKey = text.substring(0, ci).trim()
        val key = decodeScalar(rawKey, l.no)
            ?: throw YamlSyntaxException(source, l.no, "mapping key must not be empty")
        val rest = text.substring(ci + 1).trim()
        val value: YamlNode = if (rest.isEmpty()) {
            val n = peek()
            when {
                n != null && n.indent > indent -> parseBlockNode(n.indent)
                n != null && n.indent == indent && isSeqEntry(n.text) -> parseSequence(indent)
                else -> YamlNode.Scalar(null, l.no)
            }
        } else {
            parseScalarOrFlow(rest, l.no)
        }
        if (entries.containsKey(key)) {
            warnings.add(YamlWarning(l.no, DiagnosticCode.DUPLICATE_KEY, listOf(key)))
        }
        entries[key] = value
    }

    // ───────────────────────────── 純量與 flow ─────────────────────────────

    private fun isFlowStart(s: String): Boolean = s.startsWith("{") || s.startsWith("[")

    private fun parseScalarOrFlow(text: String, lineNo: Int): YamlNode {
        if (isFlowStart(text)) {
            val complete = completeFlow(text, lineNo)
            return FlowParser(source, complete, lineNo).parse()
        }
        return YamlNode.Scalar(decodeScalar(text, lineNo), lineNo)
    }

    /** flow 集合可跨行；持續併入後續行直到括號平衡。 */
    private fun completeFlow(first: String, lineNo: Int): String {
        var acc = first
        while (!flowBalanced(acc)) {
            val n = peek() ?: throw YamlSyntaxException(source, lineNo, "unterminated flow collection")
            advance()
            acc = "$acc ${n.text}"
        }
        return acc
    }

    private fun flowBalanced(s: String): Boolean {
        var depth = 0
        var inSingle = false
        var inDouble = false
        var i = 0
        while (i < s.length) {
            val c = s[i]
            if (inDouble) {
                if (c == '\\') { i += 2; continue }
                if (c == '"') inDouble = false
            } else if (inSingle) {
                if (c == '\'') {
                    if (i + 1 < s.length && s[i + 1] == '\'') { i += 2; continue }
                    inSingle = false
                }
            } else {
                when (c) {
                    '"' -> inDouble = true
                    '\'' -> inSingle = true
                    '{', '[' -> depth++
                    '}', ']' -> depth--
                    else -> {}
                }
            }
            i++
        }
        return depth <= 0 && !inSingle && !inDouble
    }

    private fun topLevelColonIndex(text: String): Int {
        var depth = 0
        var inSingle = false
        var inDouble = false
        var i = 0
        while (i < text.length) {
            val c = text[i]
            if (inDouble) {
                if (c == '\\') { i += 2; continue }
                if (c == '"') inDouble = false
            } else if (inSingle) {
                if (c == '\'') {
                    if (i + 1 < text.length && text[i + 1] == '\'') { i += 2; continue }
                    inSingle = false
                }
            } else {
                when (c) {
                    '"' -> inDouble = true
                    '\'' -> inSingle = true
                    '{', '[' -> depth++
                    '}', ']' -> depth--
                    ':' -> if (depth == 0 && (i + 1 >= text.length || text[i + 1] == ' ')) return i
                    else -> {}
                }
            }
            i++
        }
        return -1
    }

    private fun decodeScalar(raw: String, lineNo: Int): String? {
        val t = raw.trim()
        if (t.isEmpty()) return null
        if (t.length >= 2 && t[0] == '"' && t[t.length - 1] == '"') {
            return YamlEscapes.unescapeDouble(t.substring(1, t.length - 1), source, lineNo)
        }
        if (t.length >= 2 && t[0] == '\'' && t[t.length - 1] == '\'') {
            return YamlEscapes.unescapeSingle(t.substring(1, t.length - 1))
        }
        if (t == "null" || t == "Null" || t == "NULL" || t == "~") return null
        return t
    }
}

/** 流式集合 `{a: b}` / `[a, b]` 的遞迴下降解析。 */
internal class FlowParser(
    private val source: String,
    private val s: String,
    private val lineNo: Int
) {
    private var i = 0

    fun parse(): YamlNode {
        skipWs()
        val n = parseValue()
        skipWs()
        if (i < s.length) fail("unexpected trailing content in flow collection")
        return n
    }

    private fun fail(msg: String): Nothing = throw YamlSyntaxException(source, lineNo, msg)

    private fun skipWs() {
        while (i < s.length && (s[i] == ' ' || s[i] == '\t')) i++
    }

    private fun parseValue(): YamlNode {
        if (i >= s.length) return YamlNode.Scalar(null, lineNo)
        return when (s[i]) {
            '{' -> parseMap()
            '[' -> parseSeq()
            else -> YamlNode.Scalar(readScalar(",}]"), lineNo)
        }
    }

    private fun parseMap(): YamlNode {
        i++
        val entries = LinkedHashMap<String, YamlNode>()
        skipWs()
        if (i < s.length && s[i] == '}') { i++; return YamlNode.Mapping(entries, lineNo) }
        while (true) {
            skipWs()
            val key = readScalar(":") ?: fail("flow mapping key must not be empty")
            skipWs()
            if (i >= s.length || s[i] != ':') fail("expected ':' in flow mapping")
            i++
            skipWs()
            entries[key] = parseValue()
            skipWs()
            if (i < s.length && s[i] == ',') {
                i++
                skipWs()
                if (i < s.length && s[i] == '}') { i++; break }
                continue
            }
            if (i < s.length && s[i] == '}') { i++; break }
            fail("expected ',' or '}' in flow mapping")
        }
        return YamlNode.Mapping(entries, lineNo)
    }

    private fun parseSeq(): YamlNode {
        i++
        val items = ArrayList<YamlNode>()
        skipWs()
        if (i < s.length && s[i] == ']') { i++; return YamlNode.Sequence(items, lineNo) }
        while (true) {
            skipWs()
            items.add(parseValue())
            skipWs()
            if (i < s.length && s[i] == ',') {
                i++
                skipWs()
                if (i < s.length && s[i] == ']') { i++; break }
                continue
            }
            if (i < s.length && s[i] == ']') { i++; break }
            fail("expected ',' or ']' in flow sequence")
        }
        return YamlNode.Sequence(items, lineNo)
    }

    private fun readScalar(stops: String): String? {
        skipWs()
        if (i >= s.length) return null
        when (s[i]) {
            '"' -> return readQuotedDouble()
            '\'' -> return readQuotedSingle()
            else -> {}
        }
        val start = i
        while (i < s.length && stops.indexOf(s[i]) < 0) i++
        val raw = s.substring(start, i).trim()
        if (raw.isEmpty() || raw == "null" || raw == "Null" || raw == "NULL" || raw == "~") return null
        return raw
    }

    private fun readQuotedDouble(): String {
        val start = i
        i++
        while (i < s.length) {
            if (s[i] == '\\') { i += 2; continue }
            if (s[i] == '"') {
                i++
                return YamlEscapes.unescapeDouble(s.substring(start + 1, i - 1), source, lineNo)
            }
            i++
        }
        fail("unterminated double-quoted scalar")
    }

    private fun readQuotedSingle(): String {
        val start = i
        i++
        while (i < s.length) {
            if (s[i] == '\'') {
                if (i + 1 < s.length && s[i + 1] == '\'') { i += 2; continue }
                i++
                return YamlEscapes.unescapeSingle(s.substring(start + 1, i - 1))
            }
            i++
        }
        fail("unterminated single-quoted scalar")
    }
}
