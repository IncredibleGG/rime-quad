//
//  DocumentLoader.swift — 規範 §7.4 的第 1–5 步
//  讀取、標頭檢查、繼承鏈、深度合併、平台覆寫。
//

import Foundation

/// 依 id 取得 YAML 原文。搜尋路徑順序（使用者 → 隨附 → 內建）由實作負責（§2.3）。
public protocol DocumentSource {
    func read(_ id: String) -> String?
}

public struct MapDocumentSource: DocumentSource {
    private let docs: [String: String]
    public init(_ docs: [String: String]) { self.docs = docs }
    public func read(_ id: String) -> String? { docs[id] }
}

/// 檔案系統來源。`directories` 依序搜尋，先找到者勝出。
public struct DirectoryDocumentSource: DocumentSource {
    private let directories: [URL]
    public init(_ directories: [URL]) { self.directories = directories }

    public func read(_ id: String) -> String? {
        for dir in directories {
            // 單檔散佈：<dir>/<id>.yaml
            let flat = dir.appendingPathComponent("\(id).yaml")
            if let s = try? String(contentsOf: flat, encoding: .utf8) { return s }
            // 主題套件（§2.4）：<dir>/<id>/<id>.yaml
            let packaged = dir.appendingPathComponent(id).appendingPathComponent("\(id).yaml")
            if let s = try? String(contentsOf: packaged, encoding: .utf8) { return s }
        }
        return nil
    }
}

public struct ChainedDocumentSource: DocumentSource {
    private let sources: [DocumentSource]
    public init(_ sources: [DocumentSource]) { self.sources = sources }
    public func read(_ id: String) -> String? {
        for s in sources { if let t = s.read(id) { return t } }
        return nil
    }
}

/// §7.2 的合併規則。序列永不逐元素合併；顯式 null 代表刪除。
public enum NodeMerge {
    public static func mergeMapping(_ base: OrderedMap, _ over: OrderedMap) -> OrderedMap {
        var out = base
        for (k, v) in over.entries {
            if v.isNullScalar { out[k] = nil; continue }
            if let b = out[k]?.mappingValue, let o = v.mappingValue {
                out[k] = .mapping(mergeMapping(b, o), line: v.line)
            } else {
                out[k] = v
            }
        }
        return out
    }
}

public struct MergedDocument {
    public let root: YamlNode
    public let ancestry: [String]
}

public enum DocumentLoader {

    public static let clientVersion = "0.2.0"
    static let maxChain = 8

    /// §7.3：永遠取自最終文件本身，不從父代繼承。
    static let ownKeys = ["format", "id", "revision", "inherits", "min_client"]

    public static func load(kind: String, supportedMajor: Int, id: String,
                            source: DocumentSource, platform: Platform,
                            diag: Diagnostics, clientVersion: String) -> MergedDocument? {
        var docs: [OrderedMap] = []
        var docLines: [Int] = []
        var ids: [String] = []
        var seen: [String] = []
        var currentId = id

        while true {
            if seen.contains(currentId) {
                diag.add(.fatalInheritsCycle, [(seen + [currentId]).joined(separator: " -> ")],
                         path: "inherits")
                return nil
            }
            seen.append(currentId)
            if docs.count >= maxChain {
                diag.add(.fatalInheritsTooDeep, ["\(maxChain)"], path: "inherits")
                return nil
            }
            guard let text = source.read(currentId) else {
                if docs.isEmpty {
                    diag.add(.fatalDocumentNotFound, [currentId], path: "")
                } else {
                    diag.add(.fatalParentNotFound, [currentId], path: "inherits")
                }
                return nil
            }

            let parsed: YamlDocument
            do {
                parsed = try MiniYaml.parse(source: "\(currentId).yaml", text: text)
            } catch let e as YamlSyntaxError {
                diag.add(.fatalYamlSyntax, [e.detail], path: "", line: e.line)
                return nil
            } catch {
                diag.add(.fatalYamlSyntax, ["\(error)"], path: "")
                return nil
            }
            for w in parsed.warnings {
                diag.add(.duplicateKey, [w.key], path: "", line: w.line)
            }
            guard let root = parsed.root.mappingValue else {
                diag.add(.fatalRootNotMapping, [], path: "")
                return nil
            }
            guard checkHeader(root, line: parsed.root.line, kind: kind,
                              supportedMajor: supportedMajor, id: currentId, diag: diag) else {
                return nil
            }
            if docs.isEmpty,
               !checkMinClient(root, line: parsed.root.line,
                               clientVersion: clientVersion, diag: diag) {
                return nil
            }

            docs.append(root)
            docLines.append(parsed.root.line)
            ids.append(currentId)

            guard let parent = root["inherits"]?.scalarValue else { break }
            currentId = parent
        }

        // 自最遠祖先向下合併。
        var merged = docs[docs.count - 1]
        var i = docs.count - 2
        while i >= 0 {
            merged = NodeMerge.mergeMapping(merged, docs[i])
            i -= 1
        }
        merged = restoreOwnKeys(merged, leaf: docs[0])
        merged = applyPlatformOverrides(merged, platform: platform, diag: diag)
        return MergedDocument(root: .mapping(merged, line: docLines[0]), ancestry: ids.reversed())
    }

    static func isValidId(_ s: String) -> Bool {
        guard (1...64).contains(s.count) else { return false }
        let chars = Array(s)
        func headOK(_ c: Character) -> Bool { c.isNumber || (c.isLowercase && c.isASCII) }
        guard headOK(chars[0]) else { return false }
        for c in chars.dropFirst() {
            let ok = c.isNumber || (c.isLowercase && c.isASCII) || c == "." || c == "_" || c == "-"
            if !ok { return false }
        }
        return true
    }

    static func checkHeader(_ root: OrderedMap, line: Int, kind: String,
                            supportedMajor: Int, id: String, diag: Diagnostics) -> Bool {
        guard let fmt = root["format"]?.scalarValue else {
            diag.add(.fatalFormatMissing, [id], path: "format", line: line)
            return false
        }
        guard let slash = fmt.lastIndex(of: "/"),
              slash != fmt.startIndex, fmt.index(after: slash) != fmt.endIndex else {
            diag.add(.fatalFormatMalformed, [fmt, id], path: "format", line: line)
            return false
        }
        let docKind = String(fmt[fmt.startIndex..<slash])
        let majorText = String(fmt[fmt.index(after: slash)...])
        if docKind != kind {
            diag.add(.fatalFormatKindMismatch, [docKind, kind, id], path: "format", line: line)
            return false
        }
        guard let major = Int(majorText), major >= 1 else {
            diag.add(.fatalFormatMalformed, [fmt, id], path: "format", line: line)
            return false
        }
        if major > supportedMajor {
            diag.add(.fatalFormatMajorUnsupported, [id, kind, "\(major)", "\(supportedMajor)"],
                     path: "format", line: line)
            return false
        }
        guard let docId = root["id"]?.scalarValue else {
            diag.add(.fatalIdMissing, [id], path: "id", line: line)
            return false
        }
        guard isValidId(docId) else {
            diag.add(.fatalIdInvalid, [docId], path: "id", line: line)
            return false
        }
        guard docId == id else {
            diag.add(.fatalIdMismatch, [docId, id], path: "id", line: line)
            return false
        }
        return true
    }

    static func checkMinClient(_ root: OrderedMap, line: Int,
                               clientVersion: String, diag: Diagnostics) -> Bool {
        guard let min = root["min_client"]?.scalarValue else { return true }
        if compareVersions(clientVersion, min) < 0 {
            diag.add(.fatalMinClient, [min, clientVersion], path: "min_client", line: line)
            return false
        }
        return true
    }

    static func compareVersions(_ a: String, _ b: String) -> Int {
        let pa = a.split(separator: ".").map { Int($0.trimmingCharacters(in: .whitespaces)) ?? 0 }
        let pb = b.split(separator: ".").map { Int($0.trimmingCharacters(in: .whitespaces)) ?? 0 }
        for i in 0..<Swift.max(pa.count, pb.count) {
            let va = i < pa.count ? pa[i] : 0
            let vb = i < pb.count ? pb[i] : 0
            if va != vb { return va < vb ? -1 : 1 }
        }
        return 0
    }

    static func restoreOwnKeys(_ merged: OrderedMap, leaf: OrderedMap) -> OrderedMap {
        var out = merged
        for k in ownKeys { out[k] = leaf[k] }
        return out
    }

    static func applyPlatformOverrides(_ merged: OrderedMap, platform: Platform,
                                       diag: Diagnostics) -> OrderedMap {
        guard let po = merged["platform_overrides"]?.mappingValue else { return merged }
        // §7.4：未知平台鍵必須被忽略且**不產生 WARNING**（新增平台時的前向相容機制）。
        guard let branchNode = po[platform.rawValue], let branch = branchNode.mappingValue else {
            return merged
        }
        var cleaned = OrderedMap()
        for (k, v) in branch.entries {
            if k == "platform_overrides" {
                diag.add(.nestedPlatformOverrides, [],
                         path: "platform_overrides.\(platform.rawValue).platform_overrides",
                         line: v.line)
                continue
            }
            cleaned[k] = v
        }
        if cleaned.isEmpty { return merged }
        return NodeMerge.mergeMapping(merged, cleaned)
    }
}

/// 主題載入的公開進入點。
public enum ThemeLoader {
    public static let supportedMajor = 1
    public static let kind = "rime-theme"

    public static func load(id: String, source: DocumentSource,
                            platform: Platform = .macos, locale: String = "en",
                            clientVersion: String = DocumentLoader.clientVersion) -> LoadResult<Theme> {
        let diag = Diagnostics()
        guard let merged = DocumentLoader.load(kind: kind, supportedMajor: supportedMajor, id: id,
                                               source: source, platform: platform, diag: diag,
                                               clientVersion: clientVersion) else {
            return LoadResult(value: nil, diagnostics: diag.items)
        }
        let ctx = ParseContext(diagnostics: diag)
        ctx.locale = locale
        let theme = ThemeParser.bind(merged.root, id: id, ancestry: merged.ancestry, ctx: ctx)
        return LoadResult(value: theme, diagnostics: diag.items)
    }
}
