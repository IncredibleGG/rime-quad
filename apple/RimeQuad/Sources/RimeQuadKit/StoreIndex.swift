//
//  StoreIndex.swift — 方案市集的索引
//
//  格式的規範在 docs/schema-store.md §1。這一層照抄 Android 的 SchemaIndex /
//  IndexParser / DependencyResolver 的**行為**,不是重新設計 ——
//  同一份索引在兩端必須解析出同一件事,否則「這個方案在手機上裝得起來、
//  在電腦上不見了」會變成常態。
//
//  ⚠ **整份拒絕與逐筆降級的分界是刻意的。** 索引是伺服器產生的,使用者
//  改不了;因為其中一筆壞掉就整份不給用,會讓完全正常的三十幾個方案
//  一起消失。所以只有四種情況整份拒絕(見 IndexParser.parse),其餘一律
//  丟掉那一筆 + 一則警告。
//

import Foundation

// MARK: - 模型

public struct StoreSchemaRef: Equatable, Sendable {
    public let id: String
    public let name: String
    /// BCP 47。**掛在套件底下的 schema 參照上,不是掛在全域的 id 表上。**
    ///
    /// ⚠ 方案 id **不是全域唯一的**:`double_pinyin` 同時存在於
    /// `double-pinyin`(繁體詞庫)與 `ice`(簡體詞庫)兩個套件裡,字集相反。
    /// 任何拿 schema id 當唯一鍵的地方都會在這裡出錯。
    public let languageTag: String?
}

public struct StorePackage: Equatable, Sendable {
    public let id: String
    public let name: String
    public let category: String
    public let description: String
    public let file: String
    public let size: Int64
    public let sha256: String
    public let schemas: [StoreSchemaRef]
    public let requires: [String]
    public let license: String?
    public let upstream: String?
    public let recommended: Bool
    public let layoutNote: String?

    /// 沒有可啟用的方案 = 基礎元件(詞庫、opencc 資料),只當相依存在。
    public var isComponentOnly: Bool { schemas.isEmpty }
    public var schemaIds: [String] { schemas.map(\.id) }
}

public struct StoreCategory: Equatable, Sendable {
    public let id: String
    public let name: String
    public let order: Int
    public let hidden: Bool
}

public struct SchemaIndex: Equatable, Sendable {
    public static let supportedFormatVersion = 1

    public let formatVersion: Int
    public let generatedAt: String?
    public let baseURL: String?
    public let categories: [StoreCategory]
    public let packages: [StorePackage]

    public func package(id: String) -> StorePackage? { packages.first { $0.id == id } }

    public func visibleCategories() -> [StoreCategory] {
        categories.filter { !$0.hidden }.sorted { $0.order < $1.order }
    }

    public func packages(in categoryId: String) -> [StorePackage] {
        packages.filter { $0.category == categoryId }
            .sorted {
                // 推薦的排前面,其餘按名字。使用者第一眼該看到的是我們驗過的那些。
                if $0.recommended != $1.recommended { return $0.recommended }
                return $0.name < $1.name
            }
    }

    /// schemaId → BCP 47。**後面的覆蓋前面的**,與 Android 一致。
    public func languageTags() -> [String: String] {
        var out: [String: String] = [:]
        for p in packages {
            for s in p.schemas {
                guard let tag = s.languageTag, !tag.isEmpty, tag != "und" else { continue }
                out[s.id] = tag
            }
        }
        return out
    }

    public func packageProviding(schemaId: String) -> StorePackage? {
        packages.first { $0.schemaIds.contains(schemaId) }
    }
}

// MARK: - 解析

public enum IndexParseResult {
    case ok(SchemaIndex, warnings: [String])
    case err(String)
}

public enum IndexParser {

    static let sha256Pattern = "^[0-9a-f]{64}$"

    public static func parse(_ text: String) -> IndexParseResult {
        let root: Json
        do {
            root = try MiniJson.parse(text)
        } catch let e as JsonSyntaxError {
            return .err("索引不是合法的 JSON:\(e.message)")
        } catch {
            return .err("索引不是合法的 JSON")
        }
        guard root.objectValue != nil else { return .err("索引的頂層必須是物件") }
        guard let version = root["format_version"]?.intValue else {
            return .err("索引缺少 format_version")
        }
        guard version == SchemaIndex.supportedFormatVersion else {
            return .err("索引 format_version=\(version),本版只支援 "
                        + "\(SchemaIndex.supportedFormatVersion)。請更新 app。")
        }

        var warnings: [String] = []

        var categories: [StoreCategory] = []
        for (i, c) in (root["categories"]?.arrayValue ?? []).enumerated() {
            guard let id = c["id"]?.stringValue, !id.isEmpty else {
                warnings.append("categories[\(i)] 缺少 id,已略過"); continue
            }
            categories.append(StoreCategory(
                id: id,
                name: c["name"]?.stringValue ?? id,
                order: c["order"]?.intValue ?? (100 + i),
                hidden: c["hidden"]?.boolValue ?? false))
        }
        let categoryIds = Set(categories.map(\.id))

        var packages: [StorePackage] = []
        var seen = Set<String>()
        for (i, p) in (root["packages"]?.arrayValue ?? []).enumerated() {
            let where_ = "packages[\(i)]"
            guard let id = p["id"]?.stringValue, !id.isEmpty else {
                warnings.append("\(where_) 缺少 id,已略過"); continue
            }
            guard !seen.contains(id) else {
                warnings.append("\(where_) 的 id「\(id)」重複,已略過後者"); continue
            }
            guard let file = p["file"]?.stringValue, !file.isEmpty else {
                warnings.append("套件 \(id) 缺少 file,已略過"); continue
            }
            guard let shaRaw = p["sha256"]?.stringValue, !shaRaw.isEmpty else {
                warnings.append("套件 \(id) 缺少 sha256,無法驗證完整性,已略過"); continue
            }
            let sha = shaRaw.lowercased()
            guard sha.range(of: sha256Pattern, options: .regularExpression) != nil else {
                warnings.append("套件 \(id) 的 sha256 不是 64 位十六進位字串,已略過"); continue
            }
            var category = p["category"]?.stringValue ?? "other"
            if !categoryIds.isEmpty, !categoryIds.contains(category) {
                warnings.append("套件 \(id) 的 category「\(category)」不在 categories 中,歸入 other")
                category = "other"
            }
            let schemas = (p["schemas"]?.arrayValue ?? []).compactMap { s -> StoreSchemaRef? in
                guard let sid = s["id"]?.stringValue, !sid.isEmpty else { return nil }
                return StoreSchemaRef(id: sid,
                                      name: s["name"]?.stringValue ?? sid,
                                      languageTag: s["language"]?.stringValue)
            }
            seen.insert(id)
            packages.append(StorePackage(
                id: id,
                name: p["name"]?.stringValue ?? id,
                category: category,
                description: p["description"]?.stringValue ?? "",
                file: file,
                size: p["size"]?.int64Value ?? 0,
                sha256: sha,
                schemas: schemas,
                requires: p["requires"]?.stringsValue ?? [],
                license: p["license"]?.stringValue,
                upstream: p["upstream"]?.stringValue,
                recommended: p["recommended"]?.boolValue ?? false,
                layoutNote: p["layout_note"]?.stringValue))
        }

        // requires 指到不存在的套件:**警告但保留**。索引可能引用一個
        // 下一版才會出現的東西,而那不是這一筆套件壞掉。
        let ids = Set(packages.map(\.id))
        for p in packages {
            for r in p.requires where !ids.contains(r) {
                warnings.append("套件 \(p.id) 的 requires 指向不存在的套件「\(r)」")
            }
        }

        guard !packages.isEmpty else { return .err("索引裡沒有任何可用的套件") }

        return .ok(SchemaIndex(formatVersion: version,
                               generatedAt: root["generated_at"]?.stringValue,
                               baseURL: root["base_url"]?.stringValue,
                               categories: categories,
                               packages: packages),
                   warnings: warnings)
    }
}

// MARK: - 相依展開

public enum DependencyResolver {

    /// 「解壓後大概佔多少」的粗估倍率。
    ///
    /// 有實測依據但**只有一組**:luna-pinyin 的 0.4 MB zip → 解壓 0.9 MB →
    /// 部署後 13 MB(約 32 倍)。所以 UI **必須**把它標成估計值,
    /// 不可以寫成「將佔用 13 MB」——說得太肯定,錯的時候就是在騙人。
    public static let installedSizeMultiplier: Int64 = 30

    public struct Plan: Equatable, Sendable {
        public let toDownload: [StorePackage]
        public let alreadyInstalled: [String]
        /// 相依成環。**合法,不是錯誤** —— 真實索引裡 `luna-pinyin` 與
        /// `stroke` 互相 requires(互為反查詞典)。安裝流程會先把所有套件
        /// 解壓完再部署一次,所以順序不必是嚴格的拓樸序。
        public let cycles: [[String]]

        public var count: Int { toDownload.count }
        public var totalBytes: Int64 { toDownload.reduce(0) { $0 + $1.size } }
        public var estimatedInstalledBytes: Int64 {
            totalBytes * DependencyResolver.installedSizeMultiplier
        }
    }

    public enum Outcome: Equatable {
        case ok(Plan)
        case missingDependency(missing: String, requiredBy: String)
        case unknownPackage(String)
    }

    public static func resolve(index: SchemaIndex, selected: [String],
                               installed: Set<String>) -> Outcome {
        var done = Set<String>()
        var onStack: [String] = []
        var order: [StorePackage] = []
        var skipped: [String] = []
        var cycles: [[String]] = []
        var failure: Outcome?

        func visit(_ id: String, requiredBy: String?) {
            if failure != nil { return }
            if installed.contains(id) {
                if !skipped.contains(id) { skipped.append(id) }
                return
            }
            if done.contains(id) { return }
            if let at = onStack.firstIndex(of: id) {
                cycles.append(Array(onStack[at...]) + [id])
                return
            }
            guard let pkg = index.package(id: id) else {
                failure = requiredBy.map { .missingDependency(missing: id, requiredBy: $0) }
                    ?? .unknownPackage(id)
                return
            }
            onStack.append(id)
            for r in pkg.requires { visit(r, requiredBy: id) }
            onStack.removeLast()
            done.insert(id)
            order.append(pkg)
        }

        for id in selected { visit(id, requiredBy: nil) }
        if let f = failure { return f }
        return .ok(Plan(toDownload: order, alreadyInstalled: skipped, cycles: cycles))
    }

    /// 移除之前要先問:還有誰需要它。
    public static func dependents(index: SchemaIndex, of id: String,
                                  installed: Set<String>) -> [String] {
        index.packages.filter { installed.contains($0.id) && $0.requires.contains(id) }
            .map(\.id)
    }
}

/// 位元組 → 人看得懂的字串。除數是二進位,後綴寫成十進位的樣子 ——
/// 與作業系統顯示的數字對得起來比嚴謹更重要。
public func formatBytes(_ bytes: Int64) -> String {
    let kb: Double = 1024, mb = kb * 1024, gb = mb * 1024
    let b = Double(bytes)
    if b >= gb { return String(format: "%.2f GB", b / gb) }
    if b >= mb { return String(format: "%.1f MB", b / mb) }
    if b >= kb { return String(format: "%.0f KB", b / kb) }
    return "\(bytes) B"
}
