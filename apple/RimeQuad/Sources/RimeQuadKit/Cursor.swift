//
//  Cursor.swift — 帶路徑與診斷的節點游標（docs/theme-format.md §6.3）
//
//  所有取值器都遵守：欄位缺失 → 靜默採預設值；型別錯 → 預設值 + WARNING；
//  超出範圍 → 夾制 + WARNING。**沒有任何取值器會拋例外。**
//  致命錯誤只由 §6.2 的清單產生。
//

import Foundation

public final class ParseContext {
    public let diagnostics: Diagnostics
    public var palette: [String: RGBA] = [:]
    public var locale: String = "en"

    public init(diagnostics: Diagnostics) { self.diagnostics = diagnostics }
}

public final class Cursor {
    public let node: YamlNode?
    public let path: String
    public let ctx: ParseContext

    var diag: Diagnostics { ctx.diagnostics }

    init(node: YamlNode?, path: String, ctx: ParseContext) {
        self.node = node
        self.path = path
        self.ctx = ctx
    }

    public static func root(_ node: YamlNode?, ctx: ParseContext) -> Cursor {
        Cursor(node: node, path: "", ctx: ctx)
    }

    public var exists: Bool { node != nil }

    private func childPath(_ key: String) -> String { path.isEmpty ? key : "\(path).\(key)" }

    public func child(_ key: String) -> Cursor {
        guard let m = node?.mappingValue else { return Cursor(node: nil, path: childPath(key), ctx: ctx) }
        return Cursor(node: m[key], path: childPath(key), ctx: ctx)
    }

    /// 取子映射；節點存在但不是映射 → WARNING 並退化為「不存在」。
    public func mapping(_ key: String) -> Cursor { child(key).asMapping() }

    public func asMapping() -> Cursor {
        guard let n = node else { return self }
        if n.mappingValue != nil { return self }
        if n.isNullScalar { return Cursor(node: nil, path: path, ctx: ctx) }
        diag.add(.typeMismatch, ["mapping", Cursor.kindOf(n)], path: path, line: n.line)
        return Cursor(node: nil, path: path, ctx: ctx)
    }

    public func keys() -> [String] { node?.mappingValue?.keys ?? [] }

    /// 序列的元素。節點不是序列 → 空清單 + WARNING（null / 缺失則靜默）。
    public func items() -> [Cursor] {
        guard let n = node else { return [] }
        if n.isNullScalar { return [] }
        guard let seq = n.sequenceValue else {
            diag.add(.typeMismatch, ["sequence", Cursor.kindOf(n)], path: path, line: n.line)
            return []
        }
        return seq.enumerated().map { Cursor(node: $1, path: "\(path)[\($0)]", ctx: ctx) }
    }

    // ───────────────────────── 純量取值 ─────────────────────────

    private func scalarText() -> String? {
        guard let n = node else { return nil }
        if case .scalar(let v, _) = n { return v }
        diag.add(.typeMismatch, ["scalar", Cursor.kindOf(n)], path: path, line: n.line)
        return nil
    }

    public func string(_ def: String) -> String { scalarText() ?? def }

    public func stringOrNil() -> String? { scalarText() }

    public func bool(_ def: Bool) -> Bool {
        guard let s = scalarText() else { return def }
        switch s.trimmingCharacters(in: .whitespaces).lowercased() {
        case "true", "yes", "on", "1": return true
        case "false", "no", "off", "0": return false
        default:
            diag.add(.badBool, [s], path: path, line: node?.line)
            return def
        }
    }

    /// §4.2 / §6.3：超出範圍是**夾制 + WARNING**，不是靜默夾制。
    /// 使用者寫了 4.0 卻拿到 0.6，不告知說不過去；而且 §10 檢核第 9 條
    /// 要求四端對同一份壞檔案報一樣多則，靜默夾制會讓那條直接失守。
    private func reportClamp(_ v: String, _ minS: String, _ maxS: String, _ clamped: String) {
        diag.add(.outOfRange, [v, minS, maxS, clamped], path: path, line: node?.line)
    }

    public func int(_ def: Int, min lo: Int = Int.min, max hi: Int = Int.max) -> Int {
        guard let s = scalarText() else { return def }
        let t = s.trimmingCharacters(in: .whitespaces)
        guard let v = Int(t) ?? Double(t).map({ Int($0) }) else {
            diag.add(.badNumber, [s], path: path, line: node?.line)
            return def
        }
        if v < lo { reportClamp("\(v)", "\(lo)", "\(hi)", "\(lo)"); return lo }
        if v > hi { reportClamp("\(v)", "\(lo)", "\(hi)", "\(hi)"); return hi }
        return v
    }

    public func number(_ def: Double, min lo: Double = -.greatestFiniteMagnitude,
                       max hi: Double = .greatestFiniteMagnitude) -> Double {
        guard let s = scalarText() else { return def }
        let t = s.trimmingCharacters(in: .whitespaces)
        guard let v = Double(t), v.isFinite else {
            diag.add(.badNumber, [s], path: path, line: node?.line)
            return def
        }
        if v < lo { reportClamp(fmt(v), fmt(lo), fmt(hi), fmt(lo)); return lo }
        if v > hi { reportClamp(fmt(v), fmt(lo), fmt(hi), fmt(hi)); return hi }
        return v
    }

    private func fmt(_ v: Double) -> String {
        v == v.rounded() && abs(v) < 1e15 ? String(Int(v)) : String(v)
    }

    /// 長度（dp）。§4.3：一律無單位數字。
    public func length(_ def: Double, min lo: Double = 0, max hi: Double = 4096) -> Double {
        number(def, min: lo, max: hi)
    }

    /// 文字尺寸（sp）。§4.4。
    public func size(_ def: Double, min lo: Double = 1, max hi: Double = 200) -> Double {
        number(def, min: lo, max: hi)
    }

    /// 比例 0…1。§4.5。
    public func ratio(_ def: Double) -> Double { number(def, min: 0, max: 1) }

    public func enumOf(_ allowed: [String], _ def: String) -> String {
        guard let s = scalarText() else { return def }
        let t = s.trimmingCharacters(in: .whitespaces)
        for a in allowed where a.lowercased() == t.lowercased() { return a }
        diag.add(.badEnum, [t, allowed.joined(separator: "/"), def], path: path, line: node?.line)
        return def
    }

    /// 字串清單。單一字串等價於單元素清單（§4.10）。
    public func stringList(_ def: [String]) -> [String] {
        guard let n = node else { return def }
        if case .scalar(let v, _) = n { return v.map { [$0] } ?? def }
        guard let seq = n.sequenceValue else {
            diag.add(.typeMismatch, ["sequence", Cursor.kindOf(n)], path: path, line: n.line)
            return def
        }
        var out: [String] = []
        for (i, item) in seq.enumerated() {
            guard let s = item.scalarValue else {
                diag.add(.entryDropped, [], path: "\(path)[\(i)]", line: item.line)
                continue
            }
            out.append(s)
        }
        return out
    }

    /// 顏色。§4.7。解析失敗一律採欄位預設值 —— **不使用洋紅等除錯色**。
    public func color(_ def: RGBA) -> RGBA {
        guard let s = scalarText() else { return def }
        guard let v = ColorSpec.resolve(s, palette: ctx.palette) else {
            diag.add(.badColor, [s], path: path, line: node?.line)
            return def
        }
        return v
    }

    public func localized() -> LocalizedString {
        guard let n = node else { return .empty }
        if case .scalar(let v, _) = n {
            guard let v else { return .empty }
            return LocalizedString(["und": v], order: ["und"])
        }
        guard let m = n.mappingValue else {
            diag.add(.typeMismatch, ["string or locale map", Cursor.kindOf(n)], path: path, line: n.line)
            return .empty
        }
        var values: [String: String] = [:]
        var order: [String] = []
        for (k, v) in m.entries {
            guard let s = v.scalarValue else {
                diag.add(.entryDropped, [], path: "\(path).\(k)", line: v.line)
                continue
            }
            values[k] = s
            order.append(k)
        }
        return LocalizedString(values, order: order)
    }

    // ───────────────────────── 未知欄位 ─────────────────────────

    /// §6.3：未知欄位 → 忽略 + WARNING，附上最接近的已知欄位名。
    public func warnUnknownKeys(_ known: Set<String>) {
        guard let m = node?.mappingValue else { return }
        for (k, v) in m.entries {
            if known.contains(k) { continue }
            if let hint = closestKey(k, known) {
                diag.add(.unknownField, [k, hint], path: childPath(k), line: v.line)
            } else {
                diag.add(.unknownField, [k], path: childPath(k), line: v.line)
            }
        }
    }

    static func kindOf(_ n: YamlNode) -> String {
        switch n {
        case .scalar(let v, _): return v == nil ? "null" : "a scalar"
        case .mapping: return "a mapping"
        case .sequence: return "a sequence"
        }
    }
}

func clamp<T: Comparable>(_ v: T, _ lo: T, _ hi: T) -> T { v < lo ? lo : (v > hi ? hi : v) }

/// 編輯距離 <= 2 的最近候選，用於拼字錯誤的診斷訊息。
func closestKey(_ key: String, _ known: Set<String>) -> String? {
    var best: String?
    var bestD = 3
    for k in known.sorted() {
        let d = editDistance(key, k)
        if d < bestD { bestD = d; best = k }
    }
    return best
}

func editDistance(_ a: String, _ b: String) -> Int {
    if a == b { return 0 }
    let x = Array(a), y = Array(b)
    if x.isEmpty { return y.count }
    if y.isEmpty { return x.count }
    var prev = Array(0...y.count)
    var cur = [Int](repeating: 0, count: y.count + 1)
    for i in 1...x.count {
        cur[0] = i
        for j in 1...y.count {
            let cost = x[i - 1] == y[j - 1] ? 0 : 1
            cur[j] = Swift.min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost)
        }
        swap(&prev, &cur)
    }
    return prev[y.count]
}

/// 依 BCP-47 標籤查詢的在地化字串（§4.9）。
public struct LocalizedString: Sendable {
    public let values: [String: String]
    public let order: [String]

    public static let empty = LocalizedString([:], order: [])

    public init(_ values: [String: String], order: [String]) {
        self.values = values
        self.order = order
    }

    public var isEmpty: Bool { values.isEmpty }

    public func get(_ locale: String) -> String {
        if values.isEmpty { return "" }
        let lc = locale.lowercased()
        for k in order where k.lowercased() == lc { return values[k]! }
        var cur = lc
        while let idx = cur.lastIndex(of: "-") {
            cur = String(cur[cur.startIndex..<idx])
            for k in order where k.lowercased() == cur { return values[k]! }
        }
        let primary = lc.split(separator: "-").first.map(String.init) ?? lc
        for k in order where k.lowercased().split(separator: "-").first.map(String.init) == primary {
            return values[k]!
        }
        if let v = values["en"] { return v }
        if let v = values["und"] { return v }
        return order.first.flatMap { values[$0] } ?? ""
    }
}
