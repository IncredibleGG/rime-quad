//
//  ColorSpec.swift — 顏色字面值的解析（docs/theme-format.md §4.7）
//
//  檔案中的順序是 **RGB / RGBA**。本層一律轉成一個 32 位元的 RGBA 值
//  （r << 24 | g << 16 | b << 8 | a），轉換點只有這裡一處。
//
//  ⚠ 刻意**不**用 NSColor：這一層必須能在沒有 AppKit 的情況下被單元測試掃過。
//    轉成 NSColor 是渲染端（CandidateRenderer）的事。
//

import Foundation

public struct RGBA: Equatable, Sendable {
    public let r: UInt8, g: UInt8, b: UInt8, a: UInt8

    public init(r: UInt8, g: UInt8, b: UInt8, a: UInt8) {
        self.r = r; self.g = g; self.b = b; self.a = a
    }

    public static let transparent = RGBA(r: 0, g: 0, b: 0, a: 0)

    public static func hex(_ v: UInt32) -> RGBA {
        RGBA(r: UInt8((v >> 24) & 0xFF), g: UInt8((v >> 16) & 0xFF),
             b: UInt8((v >> 8) & 0xFF), a: UInt8(v & 0xFF))
    }

    /// 對 alpha 做**相乘**調變（§4.7：不是覆寫）。
    public func scalingAlpha(_ f: Double) -> RGBA {
        let scaled = Double(a) * f + 0.5
        let clamped = scaled < 0 ? 0 : (scaled > 255 ? 255 : scaled)
        return RGBA(r: r, g: g, b: b, a: UInt8(clamped))
    }

    public var description: String {
        String(format: "#%02x%02x%02x%02x", r, g, b, a)
    }
}

public enum ColorSpec {

    /// 解析一個顏色字面值；無法解析回傳 nil（由呼叫端決定退回哪個預設值）。
    public static func resolve(_ spec: String, palette: [String: RGBA]) -> RGBA? {
        let t = spec.trimmingCharacters(in: .whitespaces)
        if t.isEmpty { return nil }
        if t.lowercased() == "transparent" { return .transparent }
        if t.hasPrefix("#") { return parseHex(t) }
        if t.hasPrefix("$") { return resolveRef(t, palette: palette) }
        return nil
    }

    static func resolveRef(_ t: String, palette: [String: RGBA]) -> RGBA? {
        let chars = Array(t)
        let at = chars.firstIndex(of: "@")
        let nameEnd = at ?? chars.count
        let name = String(chars[1..<nameEnd]).trimmingCharacters(in: .whitespaces)
        if name.isEmpty { return nil }
        guard let base = palette[name] else { return nil }
        guard let at else { return base }
        let factorText = String(chars[(at + 1)...]).trimmingCharacters(in: .whitespaces)
        guard let factor = parseAlphaFactor(factorText) else { return nil }
        return base.scalingAlpha(factor)
    }

    public static func parseHex(_ t: String) -> RGBA? {
        let h = Array(t.dropFirst())
        if h.contains(where: { !$0.isHexDigit }) { return nil }
        var e: [Character]
        switch h.count {
        case 3: e = [h[0], h[0], h[1], h[1], h[2], h[2], "f", "f"]
        case 4: e = [h[0], h[0], h[1], h[1], h[2], h[2], h[3], h[3]]
        case 6: e = h + ["f", "f"]
        case 8: e = h
        default: return nil
        }
        func byte(_ i: Int) -> UInt8 { UInt8(String(e[i...(i + 1)]), radix: 16) ?? 0 }
        return RGBA(r: byte(0), g: byte(2), b: byte(4), a: byte(6))
    }

    /// `@30%` 或 `@0.3`。回傳 0…1 的乘數。
    public static func parseAlphaFactor(_ s: String) -> Double? {
        if s.isEmpty { return nil }
        if s.hasSuffix("%") {
            let body = String(s.dropLast()).trimmingCharacters(in: .whitespaces)
            guard let v = Double(body), v >= 0, v <= 100 else { return nil }
            return v / 100.0
        }
        guard let v = Double(s), v >= 0, v <= 1 else { return nil }
        return v
    }
}

/// palette 的解析，含遞迴 `$ref`（深度上限 8）。
/// 無法解析的條目視為**不存在** —— 指向它的欄位會退回各自的預設值 + WARNING。
enum PaletteResolver {

    private static let maxDepth = 8

    static func resolve(_ cursor: Cursor) -> [String: RGBA] {
        guard let node = cursor.node?.mappingValue else { return [:] }
        var raw: [String: String] = [:]
        var order: [String] = []
        for (k, v) in node.entries {
            guard let s = v.scalarValue else {
                cursor.diag.add(.paletteNotScalar, [k], path: "\(cursor.path).\(k)", line: v.line)
                continue
            }
            raw[k] = s
            order.append(k)
        }
        var out: [String: RGBA] = [:]
        for k in order {
            if let v = resolveOne(name: k, raw: raw, done: &out, depth: 0, cursor: cursor) {
                out[k] = v
            }
        }
        return out
    }

    private static func resolveOne(name: String, raw: [String: String],
                                   done: inout [String: RGBA], depth: Int,
                                   cursor: Cursor) -> RGBA? {
        if let v = done[name] { return v }
        let path = "\(cursor.path).\(name)"
        if depth >= maxDepth {
            cursor.diag.add(.paletteCycleOrTooDeep, [name], path: path)
            return nil
        }
        guard let spec = raw[name] else { return nil }
        let t = spec.trimmingCharacters(in: .whitespaces)
        if t.hasPrefix("$") {
            let chars = Array(t)
            let at = chars.firstIndex(of: "@")
            let target = String(chars[1..<(at ?? chars.count)]).trimmingCharacters(in: .whitespaces)
            if target == name {
                cursor.diag.add(.paletteSelfReference, [name], path: path)
                return nil
            }
            guard let base = resolveOne(name: target, raw: raw, done: &done,
                                        depth: depth + 1, cursor: cursor) else {
                cursor.diag.add(.paletteUnresolvedRef, [name, target], path: path)
                return nil
            }
            guard let v = ColorSpec.resolve(t, palette: [target: base]) else {
                cursor.diag.add(.paletteBadColor, [name, t], path: path)
                return nil
            }
            return v
        }
        guard let v = ColorSpec.resolve(t, palette: [:]) else {
            cursor.diag.add(.paletteBadColor, [name, t], path: path)
            return nil
        }
        return v
    }
}
