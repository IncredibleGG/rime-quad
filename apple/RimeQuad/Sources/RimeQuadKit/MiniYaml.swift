//
//  MiniYaml.swift — RTS（Rime Theme Subset of YAML）的讀取器
//
//  為什麼不用 Yams：規範 §3 已經把 YAML 限縮成一個小子集，而且 §3.3 明文規定
//  **讀取層不得做隱式型別解析** —— 所有純量一律以原始字串交出，由綁定層依欄位
//  型別轉換。Yams 走的是 YAML 1.1，`keysym: y` 會在讀取層就變成布林 `true`，
//  之後任何補救都太遲（規範自己點名這是四端分歧最惡名昭彰的一格）。
//
//  Yams 也可以叫它「不要解析」（`.string` resolver），但那要靠設定，而設定會漂移；
//  這裡是 0 依賴、單次線性掃描，行為與 Android 的 MiniYaml.kt 逐條對齊 ——
//  因為兩端要對同一份壞檔案報出同一串診斷。
//
//  ⚠ 修改本檔之前先看 MiniYamlTests：診斷數量是規範 §10 檢核第 9 條的一部分。
//

import Foundation

public indirect enum YamlNode {
    /// 純量。`nil` 代表 YAML 的 null（`null` / `~` / 空）。
    case scalar(String?, line: Int)
    case mapping(OrderedMap, line: Int)
    case sequence([YamlNode], line: Int)

    public var line: Int {
        switch self {
        case .scalar(_, let l), .mapping(_, let l), .sequence(_, let l): return l
        }
    }

    public var scalarValue: String? {
        if case .scalar(let v, _) = self { return v }
        return nil
    }

    public var isNullScalar: Bool {
        if case .scalar(let v, _) = self { return v == nil }
        return false
    }

    public var mappingValue: OrderedMap? {
        if case .mapping(let m, _) = self { return m }
        return nil
    }

    public var sequenceValue: [YamlNode]? {
        if case .sequence(let s, _) = self { return s }
        return nil
    }

    public static func emptyMapping() -> YamlNode { .mapping(OrderedMap(), line: 0) }
}

/// 保序映射。文件順序是規範的一部分（§4.9 的「文件順序中的第一筆」、
/// 未知欄位診斷的順序都靠它），Swift 的 Dictionary 沒有順序。
public struct OrderedMap {
    private(set) public var keys: [String] = []
    private var storage: [String: YamlNode] = [:]

    public init() {}

    public subscript(key: String) -> YamlNode? {
        get { storage[key] }
        set {
            if let newValue {
                if storage[key] == nil { keys.append(key) }
                storage[key] = newValue
            } else {
                storage.removeValue(forKey: key)
                keys.removeAll { $0 == key }
            }
        }
    }

    public var isEmpty: Bool { keys.isEmpty }
    public var count: Int { keys.count }
    public func contains(_ key: String) -> Bool { storage[key] != nil }

    /// 依文件順序走訪。
    public var entries: [(key: String, value: YamlNode)] {
        keys.compactMap { k in storage[k].map { (key: k, value: $0) } }
    }
}

public struct YamlSyntaxError: Error {
    public let source: String
    public let line: Int
    public let detail: String
}

public struct YamlWarning {
    public let line: Int
    public let key: String
}

public struct YamlDocument {
    public let root: YamlNode
    public let warnings: [YamlWarning]
}

enum YamlEscapes {

    static func unescapeDouble(_ s: String, source: String, line: Int) throws -> String {
        guard s.contains("\\") else { return s }
        var out = ""
        let chars = Array(s)
        var i = 0
        while i < chars.count {
            let c = chars[i]
            if c != "\\" { out.append(c); i += 1; continue }
            i += 1
            guard i < chars.count else {
                throw YamlSyntaxError(source: source, line: line, detail: "dangling backslash in quoted scalar")
            }
            switch chars[i] {
            case "n": out.append("\n")
            case "r": out.append("\r")
            case "t": out.append("\t")
            case "0": out.append("\0")
            case "\\": out.append("\\")
            case "\"": out.append("\"")
            case "'": out.append("'")
            case "/": out.append("/")
            case "u":
                guard i + 4 < chars.count else {
                    throw YamlSyntaxError(source: source, line: line, detail: "truncated \\u escape")
                }
                let hex = String(chars[(i + 1)...(i + 4)])
                guard let cp = UInt32(hex, radix: 16), let scalar = Unicode.Scalar(cp) else {
                    throw YamlSyntaxError(source: source, line: line, detail: "invalid \\u escape '\(hex)'")
                }
                out.unicodeScalars.append(scalar)
                i += 4
            case let e:
                throw YamlSyntaxError(source: source, line: line, detail: "unsupported escape '\\\(e)'")
            }
            i += 1
        }
        return out
    }

    static func unescapeSingle(_ s: String) -> String {
        s.replacingOccurrences(of: "''", with: "'")
    }
}

public final class MiniYaml {

    private struct SourceLine {
        let no: Int
        let indent: Int
        let text: [Character]
        var string: String { String(text) }
    }

    private let source: String
    private var lines: [SourceLine] = []
    private var warnings: [YamlWarning] = []
    private var pos = 0

    private init(source: String, text: String) throws {
        self.source = source
        try preprocess(text)
    }

    public static func parse(source: String, text: String) throws -> YamlDocument {
        try MiniYaml(source: source, text: text).run()
    }

    // ───────────────────────────── 前處理 ─────────────────────────────

    private func preprocess(_ raw: String) throws {
        var normalized = raw.replacingOccurrences(of: "\r\n", with: "\n")
        normalized = normalized.replacingOccurrences(of: "\r", with: "\n")
        if normalized.hasPrefix("\u{FEFF}") { normalized.removeFirst() }

        var lineNo = 0
        for rawLine in normalized.components(separatedBy: "\n") {
            lineNo += 1
            let stripped = MiniYaml.stripComment(Array(rawLine))
            if stripped.allSatisfy({ $0 == " " || $0 == "\t" }) { continue }
            var indent = 0
            while indent < stripped.count && stripped[indent] == " " { indent += 1 }
            if indent < stripped.count && stripped[indent] == "\t" {
                throw YamlSyntaxError(source: source, line: lineNo,
                                      detail: "tabs are not allowed for indentation")
            }
            var content = Array(stripped[indent...])
            while let last = content.last, last == " " || last == "\t" { content.removeLast() }
            let s = String(content)
            if s == "---" { continue }
            if s == "..." { break }
            if s.hasPrefix("&") || s.hasPrefix("*") || s.hasPrefix("<<") {
                throw YamlSyntaxError(
                    source: source, line: lineNo,
                    detail: "anchors, aliases and merge keys are not part of the Rime theme YAML subset")
            }
            lines.append(SourceLine(no: lineNo, indent: indent, text: content))
        }
    }

    /// 去掉行內註解，但不動引號內的 `#`。
    static func stripComment(_ line: [Character]) -> [Character] {
        var inSingle = false, inDouble = false
        var i = 0
        while i < line.count {
            let c = line[i]
            if inDouble {
                if c == "\\" { i += 2; continue }
                if c == "\"" { inDouble = false }
            } else if inSingle {
                if c == "'" {
                    if i + 1 < line.count && line[i + 1] == "'" { i += 2; continue }
                    inSingle = false
                }
            } else {
                switch c {
                case "\"": inDouble = true
                case "'": inSingle = true
                case "#":
                    if i == 0 || line[i - 1] == " " || line[i - 1] == "\t" { return Array(line[0..<i]) }
                default: break
                }
            }
            i += 1
        }
        return line
    }

    // ───────────────────────────── 主流程 ─────────────────────────────

    private func run() throws -> YamlDocument {
        if lines.isEmpty { return YamlDocument(root: .emptyMapping(), warnings: warnings) }
        let root = try parseBlockNode(indent: lines[0].indent)
        if pos < lines.count {
            throw YamlSyntaxError(source: source, line: lines[pos].no, detail: "unexpected indentation")
        }
        return YamlDocument(root: root, warnings: warnings)
    }

    private func peek() -> SourceLine? { pos < lines.count ? lines[pos] : nil }

    @discardableResult
    private func advance() -> SourceLine { defer { pos += 1 }; return lines[pos] }

    private func isSeqEntry(_ t: [Character]) -> Bool {
        if t.count == 1 && t[0] == "-" { return true }
        return t.count >= 2 && t[0] == "-" && t[1] == " "
    }

    private func parseBlockNode(indent: Int) throws -> YamlNode {
        guard let l = peek() else { return .scalar(nil, line: 0) }
        if isSeqEntry(l.text) { return try parseSequence(indent: indent) }
        if MiniYaml.isFlowStart(l.text) {
            advance()
            return try parseScalarOrFlow(l.text, lineNo: l.no)
        }
        return try parseMapping(indent: indent)
    }

    private func parseMapping(indent: Int) throws -> YamlNode {
        let startLine = peek()?.no ?? 0
        var entries = OrderedMap()
        while let l = peek() {
            if l.indent != indent { break }
            if isSeqEntry(l.text) { break }
            advance()
            try putEntry(&entries, line: l, text: l.text, indent: indent)
        }
        return .mapping(entries, line: startLine)
    }

    /// 序列項目 `- key: value` 的緊湊映射。
    private func parseInlineMapping(first: SourceLine, firstText: [Character],
                                    contentIndent: Int) throws -> YamlNode {
        var entries = OrderedMap()
        try putEntry(&entries, line: first, text: firstText, indent: contentIndent)
        while let l = peek() {
            if l.indent != contentIndent { break }
            if isSeqEntry(l.text) { break }
            advance()
            try putEntry(&entries, line: l, text: l.text, indent: contentIndent)
        }
        return .mapping(entries, line: first.no)
    }

    private func parseSequence(indent: Int) throws -> YamlNode {
        let startLine = peek()?.no ?? 0
        var items: [YamlNode] = []
        while let l = peek() {
            if l.indent != indent || !isSeqEntry(l.text) { break }
            advance()
            let afterDash = Array(l.text.dropFirst())
            var lead = 0
            while lead < afterDash.count && (afterDash[lead] == " " || afterDash[lead] == "\t") { lead += 1 }
            let trimmed = Array(afterDash[lead...])
            let contentIndent = l.indent + 1 + lead
            if trimmed.isEmpty {
                if let n = peek(), n.indent > indent {
                    items.append(try parseBlockNode(indent: n.indent))
                } else {
                    items.append(.scalar(nil, line: l.no))
                }
            } else if MiniYaml.isFlowStart(trimmed) || MiniYaml.topLevelColonIndex(trimmed) < 0 {
                items.append(try parseScalarOrFlow(trimmed, lineNo: l.no))
            } else {
                items.append(try parseInlineMapping(first: l, firstText: trimmed,
                                                    contentIndent: contentIndent))
            }
        }
        return .sequence(items, line: startLine)
    }

    private func putEntry(_ entries: inout OrderedMap, line l: SourceLine,
                          text: [Character], indent: Int) throws {
        let ci = MiniYaml.topLevelColonIndex(text)
        guard ci >= 0 else {
            throw YamlSyntaxError(source: source, line: l.no,
                                  detail: "expected 'key: value', found '\(String(text))'")
        }
        let rawKey = String(text[0..<ci])
        guard let key = try decodeScalar(rawKey, lineNo: l.no) else {
            throw YamlSyntaxError(source: source, line: l.no, detail: "mapping key must not be empty")
        }
        var rest = Array(text[(ci + 1)...])
        while let f = rest.first, f == " " || f == "\t" { rest.removeFirst() }
        while let b = rest.last, b == " " || b == "\t" { rest.removeLast() }

        let value: YamlNode
        if rest.isEmpty {
            if let n = peek(), n.indent > indent {
                value = try parseBlockNode(indent: n.indent)
            } else if let n = peek(), n.indent == indent, isSeqEntry(n.text) {
                value = try parseSequence(indent: indent)
            } else {
                value = .scalar(nil, line: l.no)
            }
        } else {
            value = try parseScalarOrFlow(rest, lineNo: l.no)
        }
        if entries.contains(key) {
            warnings.append(YamlWarning(line: l.no, key: key))
        }
        entries[key] = value
    }

    // ─────────────────────── 純量與 flow ───────────────────────

    static func isFlowStart(_ s: [Character]) -> Bool {
        guard let f = s.first else { return false }
        return f == "{" || f == "["
    }

    private func parseScalarOrFlow(_ text: [Character], lineNo: Int) throws -> YamlNode {
        if MiniYaml.isFlowStart(text) {
            let complete = try completeFlow(text, lineNo: lineNo)
            return try FlowParser(source: source, s: complete, lineNo: lineNo).parse()
        }
        return .scalar(try decodeScalar(String(text), lineNo: lineNo), line: lineNo)
    }

    /// flow 集合可跨行；持續併入後續行直到括號平衡。
    private func completeFlow(_ first: [Character], lineNo: Int) throws -> [Character] {
        var acc = first
        while !MiniYaml.flowBalanced(acc) {
            guard let n = peek() else {
                throw YamlSyntaxError(source: source, line: lineNo, detail: "unterminated flow collection")
            }
            advance()
            acc.append(" ")
            acc.append(contentsOf: n.text)
        }
        return acc
    }

    static func flowBalanced(_ s: [Character]) -> Bool {
        var depth = 0, inSingle = false, inDouble = false
        var i = 0
        while i < s.count {
            let c = s[i]
            if inDouble {
                if c == "\\" { i += 2; continue }
                if c == "\"" { inDouble = false }
            } else if inSingle {
                if c == "'" {
                    if i + 1 < s.count && s[i + 1] == "'" { i += 2; continue }
                    inSingle = false
                }
            } else {
                switch c {
                case "\"": inDouble = true
                case "'": inSingle = true
                case "{", "[": depth += 1
                case "}", "]": depth -= 1
                default: break
                }
            }
            i += 1
        }
        return depth <= 0 && !inSingle && !inDouble
    }

    static func topLevelColonIndex(_ text: [Character]) -> Int {
        var depth = 0, inSingle = false, inDouble = false
        var i = 0
        while i < text.count {
            let c = text[i]
            if inDouble {
                if c == "\\" { i += 2; continue }
                if c == "\"" { inDouble = false }
            } else if inSingle {
                if c == "'" {
                    if i + 1 < text.count && text[i + 1] == "'" { i += 2; continue }
                    inSingle = false
                }
            } else {
                switch c {
                case "\"": inDouble = true
                case "'": inSingle = true
                case "{", "[": depth += 1
                case "}", "]": depth -= 1
                case ":":
                    if depth == 0 && (i + 1 >= text.count || text[i + 1] == " ") { return i }
                default: break
                }
            }
            i += 1
        }
        return -1
    }

    private func decodeScalar(_ raw: String, lineNo: Int) throws -> String? {
        let t = raw.trimmingCharacters(in: .whitespaces)
        if t.isEmpty { return nil }
        let chars = Array(t)
        if chars.count >= 2 && chars[0] == "\"" && chars[chars.count - 1] == "\"" {
            return try YamlEscapes.unescapeDouble(String(chars[1..<(chars.count - 1)]),
                                                  source: source, line: lineNo)
        }
        if chars.count >= 2 && chars[0] == "'" && chars[chars.count - 1] == "'" {
            return YamlEscapes.unescapeSingle(String(chars[1..<(chars.count - 1)]))
        }
        if t == "null" || t == "Null" || t == "NULL" || t == "~" { return nil }
        return t
    }
}

/// 流式集合 `{a: b}` / `[a, b]` 的遞迴下降解析。
final class FlowParser {
    private let source: String
    private let s: [Character]
    private let lineNo: Int
    private var i = 0

    init(source: String, s: [Character], lineNo: Int) {
        self.source = source
        self.s = s
        self.lineNo = lineNo
    }

    func parse() throws -> YamlNode {
        skipWs()
        let n = try parseValue()
        skipWs()
        if i < s.count { throw fail("unexpected trailing content in flow collection") }
        return n
    }

    private func fail(_ msg: String) -> YamlSyntaxError {
        YamlSyntaxError(source: source, line: lineNo, detail: msg)
    }

    private func skipWs() {
        while i < s.count && (s[i] == " " || s[i] == "\t") { i += 1 }
    }

    private func parseValue() throws -> YamlNode {
        guard i < s.count else { return .scalar(nil, line: lineNo) }
        switch s[i] {
        case "{": return try parseMap()
        case "[": return try parseSeq()
        default: return .scalar(try readScalar(stops: ",}]"), line: lineNo)
        }
    }

    private func parseMap() throws -> YamlNode {
        i += 1
        var entries = OrderedMap()
        skipWs()
        if i < s.count && s[i] == "}" { i += 1; return .mapping(entries, line: lineNo) }
        while true {
            skipWs()
            guard let key = try readScalar(stops: ":") else { throw fail("flow mapping key must not be empty") }
            skipWs()
            guard i < s.count && s[i] == ":" else { throw fail("expected ':' in flow mapping") }
            i += 1
            skipWs()
            entries[key] = try parseValue()
            skipWs()
            if i < s.count && s[i] == "," {
                i += 1
                skipWs()
                if i < s.count && s[i] == "}" { i += 1; break }
                continue
            }
            if i < s.count && s[i] == "}" { i += 1; break }
            throw fail("expected ',' or '}' in flow mapping")
        }
        return .mapping(entries, line: lineNo)
    }

    private func parseSeq() throws -> YamlNode {
        i += 1
        var items: [YamlNode] = []
        skipWs()
        if i < s.count && s[i] == "]" { i += 1; return .sequence(items, line: lineNo) }
        while true {
            skipWs()
            items.append(try parseValue())
            skipWs()
            if i < s.count && s[i] == "," {
                i += 1
                skipWs()
                if i < s.count && s[i] == "]" { i += 1; break }
                continue
            }
            if i < s.count && s[i] == "]" { i += 1; break }
            throw fail("expected ',' or ']' in flow sequence")
        }
        return .sequence(items, line: lineNo)
    }

    private func readScalar(stops: String) throws -> String? {
        skipWs()
        guard i < s.count else { return nil }
        if s[i] == "\"" { return try readQuotedDouble() }
        if s[i] == "'" { return readQuotedSingle() }
        let start = i
        let stopSet = Set(stops)
        while i < s.count && !stopSet.contains(s[i]) { i += 1 }
        let raw = String(s[start..<i]).trimmingCharacters(in: .whitespaces)
        if raw.isEmpty || raw == "null" || raw == "Null" || raw == "NULL" || raw == "~" { return nil }
        return raw
    }

    private func readQuotedDouble() throws -> String {
        let start = i
        i += 1
        while i < s.count {
            if s[i] == "\\" { i += 2; continue }
            if s[i] == "\"" {
                i += 1
                return try YamlEscapes.unescapeDouble(String(s[(start + 1)..<(i - 1)]),
                                                      source: source, line: lineNo)
            }
            i += 1
        }
        throw fail("unterminated double-quoted scalar")
    }

    private func readQuotedSingle() -> String {
        let start = i
        i += 1
        while i < s.count {
            if s[i] == "'" {
                if i + 1 < s.count && s[i + 1] == "'" { i += 2; continue }
                i += 1
                return YamlEscapes.unescapeSingle(String(s[(start + 1)..<(i - 1)]))
            }
            i += 1
        }
        // 未閉合：回傳到行尾，交由上層的 balance 檢查處理。
        return YamlEscapes.unescapeSingle(String(s[(start + 1)...]))
    }
}
