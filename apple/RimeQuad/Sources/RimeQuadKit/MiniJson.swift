//
//  MiniJson.swift — 方案市集索引用的 JSON 讀取器
//
//  ── 為什麼不用 JSONSerialization ────────────────────────────────────────
//  兩個具體理由,不是潔癖:
//
//  1. **索引允許註解。** `docs/schema-store.md` 的範例本身就是 JSONC ——
//     索引是人在維護的,每一筆套件旁邊寫一句「這個要配大千鍵盤」是常態。
//     `JSONSerialization` 看到 `//` 直接整份拒絕,而錯誤訊息會是
//     「Invalid value around character 812」,使用者只會看到「清單壞了」。
//
//  2. **`size` 是 64 位元整數。** `JSONSerialization` 把數字讀成 `Double`,
//     超過 2^53 就開始失真。這裡保留原始字串,要什麼型別再轉。
//
//  與 RFC 8259 的差異只有**放寬**,沒有收緊 —— 合法的 JSON 一定讀得進來。
//  刻意**不**放寬的:結尾逗號、單引號字串、沒引號的鍵、字串裡的裸控制字元、
//  頂層值之後還有東西。那些都是「檔案壞了」的訊號,吞掉它們等於把
//  「索引被截斷」變成「索引裡的套件少了幾個」,而後者沒有人查得出來。
//

import Foundation

public indirect enum Json: Equatable, Sendable {
    case null
    case bool(Bool)
    /// 保留原始字串。見檔頭第 2 點。
    case number(String)
    case string(String)
    case array([Json])
    case object([String: Json])

    // ── 取值:一律不丟例外。缺鍵或型別不符回 nil / 空陣列。 ──
    public subscript(key: String) -> Json? {
        if case .object(let o) = self { return o[key] }
        return nil
    }
    public var stringValue: String? {
        if case .string(let s) = self { return s }
        return nil
    }
    public var intValue: Int? {
        if case .number(let raw) = self {
            return Int(raw) ?? Double(raw).map { Int($0) }
        }
        return nil
    }
    public var int64Value: Int64? {
        if case .number(let raw) = self {
            return Int64(raw) ?? Double(raw).map { Int64($0) }
        }
        return nil
    }
    public var boolValue: Bool? {
        if case .bool(let b) = self { return b }
        return nil
    }
    public var arrayValue: [Json] {
        if case .array(let a) = self { return a }
        return []
    }
    public var objectValue: [String: Json]? {
        if case .object(let o) = self { return o }
        return nil
    }
    public var stringsValue: [String] { arrayValue.compactMap(\.stringValue) }
}

public struct JsonSyntaxError: Error, Equatable {
    public let offset: Int
    public let detail: String
    public var message: String { "JSON 位移 \(offset):\(detail)" }
}

public enum MiniJson {

    public static func parse(_ text: String) throws -> Json {
        var p = Parser(Array(text.unicodeScalars))
        p.skipTrivia()
        let v = try p.value()
        p.skipTrivia()
        guard p.atEnd else { throw p.error("頂層值之後還有內容") }
        return v
    }

    /// 壞掉就當作沒有。用在本機的帳本(一份壞掉的快取等於不存在)。
    public static func parseOrNil(_ text: String) -> Json? { try? parse(text) }

    // MARK: - 解析器

    struct Parser {
        let s: [Unicode.Scalar]
        var i = 0
        init(_ s: [Unicode.Scalar]) { self.s = s }

        var atEnd: Bool { i >= s.count }
        func error(_ d: String) -> JsonSyntaxError { JsonSyntaxError(offset: i, detail: d) }

        mutating func skipTrivia() {
            while i < s.count {
                let c = s[i]
                if c == " " || c == "\t" || c == "\n" || c == "\r" { i += 1; continue }
                // JSONC:兩種註解都接受,出現在任何空白合法的地方。
                if c == "/", i + 1 < s.count {
                    if s[i + 1] == "/" {
                        while i < s.count, s[i] != "\n" { i += 1 }
                        continue
                    }
                    if s[i + 1] == "*" {
                        i += 2
                        while i + 1 < s.count, !(s[i] == "*" && s[i + 1] == "/") { i += 1 }
                        i = min(i + 2, s.count)
                        continue
                    }
                }
                return
            }
        }

        mutating func value() throws -> Json {
            guard i < s.count else { throw error("內容提早結束") }
            switch s[i] {
            case "{": return try object()
            case "[": return try array()
            case "\"": return .string(try string())
            case "t": try literal("true"); return .bool(true)
            case "f": try literal("false"); return .bool(false)
            case "n": try literal("null"); return .null
            default: return .number(try number())
            }
        }

        mutating func literal(_ word: String) throws {
            for ch in word.unicodeScalars {
                guard i < s.count, s[i] == ch else { throw error("預期 \(word)") }
                i += 1
            }
        }

        mutating func object() throws -> Json {
            i += 1 // {
            var out: [String: Json] = [:]
            skipTrivia()
            if i < s.count, s[i] == "}" { i += 1; return .object(out) }
            while true {
                skipTrivia()
                guard i < s.count, s[i] == "\"" else { throw error("物件的鍵必須是雙引號字串") }
                let k = try string()
                skipTrivia()
                guard i < s.count, s[i] == ":" else { throw error("鍵之後預期 :") }
                i += 1
                skipTrivia()
                out[k] = try value()
                skipTrivia()
                guard i < s.count else { throw error("物件沒有結束") }
                if s[i] == "," {
                    i += 1
                    skipTrivia()
                    // 結尾逗號是壞檔案的訊號,不是風格。
                    guard i < s.count, s[i] != "}" else { throw error("物件有結尾逗號") }
                    continue
                }
                if s[i] == "}" { i += 1; return .object(out) }
                throw error("物件裡預期 , 或 }")
            }
        }

        mutating func array() throws -> Json {
            i += 1 // [
            var out: [Json] = []
            skipTrivia()
            if i < s.count, s[i] == "]" { i += 1; return .array(out) }
            while true {
                skipTrivia()
                out.append(try value())
                skipTrivia()
                guard i < s.count else { throw error("陣列沒有結束") }
                if s[i] == "," {
                    i += 1
                    skipTrivia()
                    guard i < s.count, s[i] != "]" else { throw error("陣列有結尾逗號") }
                    continue
                }
                if s[i] == "]" { i += 1; return .array(out) }
                throw error("陣列裡預期 , 或 ]")
            }
        }

        mutating func string() throws -> String {
            i += 1 // "
            var units: [UInt16] = []
            while i < s.count {
                let c = s[i]
                if c == "\"" { i += 1; return String(decoding: units, as: UTF16.self) }
                if c.value < 0x20 { throw error("字串裡有未跳脫的控制字元") }
                if c == "\\" {
                    i += 1
                    guard i < s.count else { throw error("跳脫序列被截斷") }
                    switch s[i] {
                    case "\"": units.append(0x22)
                    case "\\": units.append(0x5C)
                    case "/": units.append(0x2F)
                    case "b": units.append(0x08)
                    case "f": units.append(0x0C)
                    case "n": units.append(0x0A)
                    case "r": units.append(0x0D)
                    case "t": units.append(0x09)
                    case "u":
                        guard i + 4 < s.count else { throw error("\\u 被截斷") }
                        var v: UInt16 = 0
                        for k in 1...4 {
                            guard let d = hex(s[i + k]) else { throw error("\\u 後面不是十六進位") }
                            v = v << 4 | UInt16(d)
                        }
                        i += 4
                        // 直接推 UTF-16 單元:代理對是兩個連續的 \u,
                        // 各推一個單元,String(decoding:as:UTF16.self) 會自己合起來。
                        units.append(v)
                    default: throw error("不支援的跳脫序列")
                    }
                    i += 1
                    continue
                }
                units.append(contentsOf: Array(String(c).utf16))
                i += 1
            }
            throw error("字串沒有結束")
        }

        func hex(_ c: Unicode.Scalar) -> Int? {
            switch c {
            case "0"..."9": return Int(c.value - 48)
            case "a"..."f": return Int(c.value - 87)
            case "A"..."F": return Int(c.value - 55)
            default: return nil
            }
        }

        mutating func number() throws -> String {
            let start = i
            if i < s.count, s[i] == "-" { i += 1 }
            let intStart = i
            var digits = 0
            while i < s.count, s[i] >= "0", s[i] <= "9" { i += 1; digits += 1 }
            guard digits > 0 else { throw error("不是合法的值") }
            // 前導零是壞檔案的訊號(RFC 8259 不允許)。放寬它等於接受
            // 一份被別的工具寫壞的索引,而壞在哪裡沒有人看得出來。
            if digits > 1, s[intStart] == "0" { throw error("數字有前導零") }
            if i < s.count, s[i] == "." {
                i += 1
                var frac = 0
                while i < s.count, s[i] >= "0", s[i] <= "9" { i += 1; frac += 1 }
                guard frac > 0 else { throw error("小數點後面沒有數字") }
            }
            if i < s.count, s[i] == "e" || s[i] == "E" {
                i += 1
                if i < s.count, s[i] == "+" || s[i] == "-" { i += 1 }
                var exp = 0
                while i < s.count, s[i] >= "0", s[i] <= "9" { i += 1; exp += 1 }
                guard exp > 0 else { throw error("指數沒有數字") }
            }
            return String(String.UnicodeScalarView(s[start..<i]))
        }
    }
}
