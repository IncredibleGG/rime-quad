//
//  SchemaCatalog.swift — 這台電腦上到底有哪些方案
//
//  ── 為什麼不直接問 librime ──────────────────────────────────────────────
//  `rs_schema_list()` 回的是**部署過的**方案。設定介面要顯示的是
//  「已經裝在硬碟上、可以打勾啟用的」—— 一個剛下載完還沒部署的方案
//  在 `rs_schema_list()` 裡看不到,而使用者剛剛才按下下載,他預期它在清單裡。
//
//  而且設定介面是**另一個行程**,它沒有 librime session(見 apple/README.md §6)。
//  所以這一層直接看檔案系統,純函式,有單元測試。
//

import Foundation

public struct InstalledSchema: Equatable, Sendable {
    public let id: String
    /// 方案自己宣告的名字。讀不到時等於 `id` —— 畫面上絕不顯示空字串。
    public let name: String
    /// `schema:` 區塊的 `version`。給診斷用。
    public let version: String?
    /// 來自方案市集索引或已安裝紀錄。決定它算繁體還是簡體。
    public let languageTag: String?
    /// 檔案在哪。使用者目錄的優先於隨附目錄的。
    public let url: URL
    /// true = 隨附在 .app 裡,不能移除。
    public let isBuiltin: Bool

    public init(id: String, name: String, version: String? = nil, languageTag: String? = nil,
                url: URL, isBuiltin: Bool) {
        self.id = id
        self.name = name
        self.version = version
        self.languageTag = languageTag
        self.url = url
        self.isBuiltin = isBuiltin
    }

    public var entry: SchemaEntry { SchemaEntry(id: id, name: name, languageTag: languageTag) }
}

public enum SchemaCatalog {

    public static let suffix = ".schema.yaml"

    /// 掃描目錄,回傳所有方案。
    ///
    /// - Parameters:
    ///   - userDir: 使用者目錄(下載與匯入的方案落在這裡)。
    ///   - sharedDir: `.app` 隨附的目錄。
    ///   - languageTags: schemaId → BCP 47,由市集索引/已安裝紀錄提供。
    ///
    /// **同 id 時使用者目錄勝出**,與 librime 自己的查找順序一致 ——
    /// 不一致的話設定畫面顯示的名字會跟實際載入的那一份不同,
    /// 而那種落差沒有任何線索可循。
    public static func scan(userDir: URL?, sharedDir: URL?,
                            languageTags: [String: String] = [:],
                            fm: FileManager = .default) -> [InstalledSchema] {
        var byId: [String: InstalledSchema] = [:]
        // 先掃隨附的,再掃使用者的 —— 後掃的覆蓋先掃的。
        for (dir, builtin) in [(sharedDir, true), (userDir, false)] {
            guard let dir else { continue }
            let names = (try? fm.contentsOfDirectory(atPath: dir.path)) ?? []
            for name in names.sorted() where name.hasSuffix(suffix) {
                let url = dir.appendingPathComponent(name)
                let fallbackId = String(name.dropLast(suffix.count))
                guard !fallbackId.isEmpty else { continue }
                let text = (try? String(contentsOf: url, encoding: .utf8)) ?? ""
                let header = readHeader(text)
                let id = header.schemaId ?? fallbackId
                byId[id] = InstalledSchema(
                    id: id,
                    name: header.name?.isEmpty == false ? header.name! : id,
                    version: header.version,
                    languageTag: languageTags[id],
                    url: url,
                    isBuiltin: builtin)
            }
        }
        return byId.values.sorted { $0.id < $1.id }
    }

    // MARK: - 方案檔的表頭

    public struct Header: Equatable, Sendable {
        public var schemaId: String?
        public var name: String?
        public var version: String?
    }

    /// 讀 `schema:` 區塊底下的 `schema_id` / `name` / `version`。
    ///
    /// ⚠ **刻意不用 MiniYaml。** MiniYaml 實作的是 RTS(規範 §3 的 YAML 子集),
    /// 而第三方方案檔用的是完整 YAML:錨點、標籤、多行字串、`__include`……
    /// 拿 RTS 去解析它們會在合法的方案上報錯,然後那個方案就從清單裡消失了,
    /// 而使用者只會看到「我裝的東西不見了」。
    ///
    /// 所以這裡是一個**只認得三個鍵的行掃描器**:看不懂的一律跳過,
    /// 三個鍵都讀不到就回落到檔名。寧可少讀一個名字,不可少一個方案。
    public static func readHeader(_ text: String) -> Header {
        var h = Header()
        var inSchema = false
        for raw in text.components(separatedBy: "\n") {
            if raw.isEmpty { continue }
            let indent = raw.prefix { $0 == " " }.count
            let line = raw.trimmingCharacters(in: .whitespaces)
            if line.isEmpty || line.hasPrefix("#") { continue }
            if indent == 0 {
                // `---` / `...` 是 YAML 的文件分隔,不影響區塊判斷。
                if line == "---" || line == "..." { continue }
                inSchema = (line == "schema:")
                continue
            }
            guard inSchema, indent > 0 else { continue }
            guard let colon = line.firstIndex(of: ":") else { continue }
            let key = String(line[line.startIndex..<colon]).trimmingCharacters(in: .whitespaces)
            var value = String(line[line.index(after: colon)...])
                .trimmingCharacters(in: .whitespaces)
            if value.hasPrefix("#") { value = "" }
            value = stripQuotes(value)
            guard !value.isEmpty else { continue }
            switch key {
            case "schema_id": if h.schemaId == nil { h.schemaId = value }
            case "name": if h.name == nil { h.name = value }
            case "version": if h.version == nil { h.version = value }
            default: break
            }
        }
        return h
    }

    static func stripQuotes(_ s: String) -> String {
        guard s.count >= 2 else { return s }
        let first = s.first!, last = s.last!
        if (first == "\"" && last == "\"") || (first == "'" && last == "'") {
            return String(s.dropFirst().dropLast())
        }
        return s
    }

    // MARK: - 清單的合併

    /// 已啟用清單 + 已安裝清單 → 設定畫面那一份帶勾與順序的清單。
    ///
    /// 兩個規則:
    ///   · **啟用的排在前面,而且照使用者排的順序** —— 那個順序就是切換順序。
    ///   · **已啟用但檔案不在了的 id 要留著**,標成「找不到檔案」。
    ///     偷偷把它拿掉的話,使用者的 schema_list 會在他不知情的時候變短,
    ///     而那通常代表同步或安裝出了問題,正是他需要知道的事。
    public struct Row: Equatable, Sendable {
        public let id: String
        public let name: String
        public let enabled: Bool
        public let installed: Bool
        public let isBuiltin: Bool
        public let languageTag: String?
        public var script: ScriptVariant { SchemaScript.of(id: id, languageTag: languageTag) }
    }

    public static func rows(installed: [InstalledSchema], enabled: [String]) -> [Row] {
        let byId = Dictionary(uniqueKeysWithValues: installed.map { ($0.id, $0) })
        var out: [Row] = []
        var seen = Set<String>()
        for id in enabled where !seen.contains(id) {
            seen.insert(id)
            let s = byId[id]
            out.append(Row(id: id, name: s?.name ?? id, enabled: true,
                           installed: s != nil, isBuiltin: s?.isBuiltin ?? false,
                           languageTag: s?.languageTag))
        }
        for s in installed where !seen.contains(s.id) {
            out.append(Row(id: s.id, name: s.name, enabled: false, installed: true,
                           isBuiltin: s.isBuiltin, languageTag: s.languageTag))
        }
        return out
    }
}
