//
//  InstalledRegistry.swift — 「裝了哪些套件」的帳本
//
//  ── 已安裝 ≠ 已啟用,這兩件事不可以合成一個開關 ──────────────────────────
//  · 已安裝 = 檔案在使用者目錄裡(本帳本)
//  · 已啟用 = 出現在 default.custom.yaml 的 schema_list(RimeConfigPatch)
//
//  理由是成本:librime **每次部署都會把 schema_list 裡的每一個方案重編一次**。
//  Android 端實測三個方案 7.2 秒、S24U 首次 12.5 秒,而且是相加的。
//  裝了十個、只用兩個的人,每次部署都要為八個用不到的詞庫付錢,永遠。
//
//  所以:**停用** = 從 schema_list 移除 + 重新部署,檔案留著(要用時不必再下載);
//        **移除** = 真的刪檔案,而且要先確認沒有別的套件依賴它。
//
//  帳本放在**使用者目錄**而不是 UserDefaults:它描述的是那個目錄的內容,
//  必須跟著那個目錄一起死。使用者手動刪掉資料夾之後,不該留下一份
//  「帳本說裝了、檔案卻不在」的紀錄。
//

import Foundation

public struct InstalledPackage: Equatable, Sendable {
    public let id: String
    public let name: String
    public let sha256: String
    public let installedAt: Date
    /// `store` 或 `local`。
    public let source: String
    public let requires: [String]
    /// **相對於使用者目錄**的路徑。移除時刪的就是這一份清單。
    public let files: [String]
    public let schemas: [StoreSchemaRef]

    public var schemaIds: [String] { schemas.map(\.id) }

    public init(id: String, name: String, sha256: String, installedAt: Date = Date(),
                source: String = "store", requires: [String] = [],
                files: [String] = [], schemas: [StoreSchemaRef] = []) {
        self.id = id
        self.name = name
        self.sha256 = sha256
        self.installedAt = installedAt
        self.source = source
        self.requires = requires
        self.files = files
        self.schemas = schemas
    }
}

public final class InstalledRegistry {

    public static let fileName = "luminakey-store.json"
    public static let formatVersion = 1

    private let url: URL
    private(set) public var all: [InstalledPackage] = []

    public init(userDir: URL) {
        self.url = userDir.appendingPathComponent(InstalledRegistry.fileName)
        load()
    }

    public var ids: Set<String> { Set(all.map(\.id)) }
    public func get(_ id: String) -> InstalledPackage? { all.first { $0.id == id } }
    public func isInstalled(_ id: String) -> Bool { get(id) != nil }

    /// 誰依賴它。移除之前一定要問。
    public func dependents(of id: String) -> [InstalledPackage] {
        all.filter { $0.requires.contains(id) }
    }

    /// schemaId → BCP 47。三個來源裡**最準的一個**,因為它知道是哪一個套件
    /// 裝了這個 schema id(而 id 不是全域唯一的,見 StoreSchemaRef)。
    public func languageTags() -> [String: String] {
        var out: [String: String] = [:]
        for p in all {
            for s in p.schemas {
                guard let t = s.languageTag, !t.isEmpty, t != "und" else { continue }
                out[s.id] = t
            }
        }
        return out
    }

    public func put(_ p: InstalledPackage) {
        all.removeAll { $0.id == p.id }
        all.append(p)
        save()
    }

    @discardableResult
    public func remove(_ id: String) -> InstalledPackage? {
        guard let p = get(id) else { return nil }
        all.removeAll { $0.id == id }
        save()
        return p
    }

    // MARK: - 讀寫

    /// **讀取一律不失敗。** 壞掉的帳本等於沒有帳本 —— 使用者的方案檔還在,
    /// 頂多是市集畫面顯示「未安裝」,他再裝一次就好。丟例外只會讓
    /// 整個設定視窗開不起來。
    private func load() {
        guard let text = try? String(contentsOf: url, encoding: .utf8),
              let root = MiniJson.parseOrNil(text) else { return }
        all = root["packages"]?.arrayValue.compactMap { j -> InstalledPackage? in
            guard let id = j["id"]?.stringValue, !id.isEmpty else { return nil }
            let ms = j["installed_at"]?.int64Value ?? 0
            return InstalledPackage(
                id: id,
                name: j["name"]?.stringValue ?? id,
                sha256: j["sha256"]?.stringValue ?? "",
                installedAt: Date(timeIntervalSince1970: Double(ms) / 1000),
                source: j["source"]?.stringValue ?? "store",
                requires: j["requires"]?.stringsValue ?? [],
                files: j["files"]?.stringsValue ?? [],
                schemas: (j["schemas"]?.arrayValue ?? []).compactMap { s in
                    guard let sid = s["id"]?.stringValue else { return nil }
                    return StoreSchemaRef(id: sid,
                                          name: s["name"]?.stringValue ?? sid,
                                          languageTag: s["language"]?.stringValue)
                })
        } ?? []
    }

    private func save() {
        let packages: [[String: Any]] = all.map { p in
            var d: [String: Any] = [
                "id": p.id, "name": p.name, "sha256": p.sha256,
                "installed_at": Int(p.installedAt.timeIntervalSince1970 * 1000),
                "source": p.source,
                "requires": p.requires,
                "files": p.files,
                "schemas": p.schemas.map { s -> [String: Any] in
                    var e: [String: Any] = ["id": s.id, "name": s.name]
                    if let t = s.languageTag { e["language"] = t }
                    return e
                },
            ]
            d["format"] = nil
            d.removeValue(forKey: "format")
            return d
        }
        let root: [String: Any] = [
            "format_version": InstalledRegistry.formatVersion,
            "packages": packages,
        ]
        guard let data = try? JSONSerialization.data(withJSONObject: root,
                                                     options: [.prettyPrinted, .sortedKeys])
        else { return }
        try? FileManager.default.createDirectory(at: url.deletingLastPathComponent(),
                                                 withIntermediateDirectories: true)
        try? data.write(to: url, options: .atomic)
    }
}
