//
//  SchemaPreflight.swift — 部署前先問「檔案到底齊不齊」
//
//  ── 為什麼需要它 ────────────────────────────────────────────────────────
//  `rs_last_error()` 在部署失敗時是**空字串**(coordination.md §4):
//  librime 只回一個 bool,原因寫進 glog,永遠到不了使用者面前。
//  所以「少了一本詞庫」與「方案語法錯」在畫面上長得一模一樣,
//  都是「部署失敗」四個字。
//
//  這一層把最常見的那一種失敗**提前**算出來,而且是在動 schema_list **之前** ——
//  所以它不但講得出是哪一本詞庫不見了,而且根本不需要回滾。
//  回滾仍然留著,但只服務「檔案都在、librime 還是建不起來」那一種說不準的情況。
//
//  ⚠ **這不是一個完整的 librime 設定解析器,也不該是。**
//  它只認得幾個高頻的鍵,而且**只有在檔案確實不存在時才出聲**。
//  誤判會擋下完全正常的套件,那比漏判更糟 —— 漏判只是回到現狀。
//

import Foundation

public struct SchemaPreflight {

    public enum Kind: String, Sendable {
        case dictionary, schema, config, grammar
    }

    public struct Missing: Equatable, Sendable {
        public let kind: Kind
        public let fileName: String
        public let referencedBy: String

        public var message: T {
            switch kind {
            case .dictionary:
                return T("方案「\(referencedBy)」需要詞典 \(fileName),但使用者目錄與隨附目錄裡都沒有。請一併安裝提供這本詞典的套件。",
                         "方案「\(referencedBy)」需要词典 \(fileName),但用户目录与随附目录里都没有。请一并安装提供这本词典的套件。",
                         "Schema \(referencedBy) needs the dictionary \(fileName), which is in neither the user nor the shared directory. Install the package that provides it.")
            case .schema:
                return T("找不到方案檔 \(fileName) —— 這個方案並沒有安裝。",
                         "找不到方案文件 \(fileName) —— 这个方案并没有安装。",
                         "Schema file \(fileName) is missing — this schema is not installed.")
            case .config:
                return T("方案「\(referencedBy)」引用了設定檔 \(fileName),但找不到它。",
                         "方案「\(referencedBy)」引用了配置文件 \(fileName),但找不到它。",
                         "Schema \(referencedBy) refers to the config file \(fileName), which is missing.")
            case .grammar:
                return T("方案「\(referencedBy)」需要語言模型 \(fileName),但找不到它。",
                         "方案「\(referencedBy)」需要语言模型 \(fileName),但找不到它。",
                         "Schema \(referencedBy) needs the language model \(fileName), which is missing.")
            }
        }
    }

    public struct Report: Equatable, Sendable {
        public let schemaId: String
        public let missing: [Missing]
        public var ok: Bool { missing.isEmpty }
    }

    /// - Parameter searchDirs: **必須**照 librime 自己的查找順序:
    ///   `[使用者目錄, 隨附目錄]`。反過來的話,使用者覆寫過的檔案會被
    ///   隨附的那一份遮住,而檢查結果與實際載入的東西不一致。
    public static func check(schemaFile: URL, searchDirs: [URL],
                             fm: FileManager = .default) -> Report {
        let base = schemaFile.lastPathComponent
        let fallbackId = base.hasSuffix(SchemaCatalog.suffix)
            ? String(base.dropLast(SchemaCatalog.suffix.count)) : base
        guard let text = try? String(contentsOf: schemaFile, encoding: .utf8) else {
            return Report(schemaId: fallbackId,
                          missing: [Missing(kind: .schema, fileName: base, referencedBy: base)])
        }
        let header = SchemaCatalog.readHeader(text)
        let declaredId = header.schemaId ?? fallbackId

        var refs: [(String, Kind)] = []
        collect(text: text, into: &refs)

        var missing: [Missing] = []
        var seen = Set<String>()
        for (name, kind) in refs {
            guard !seen.contains(name) else { continue }
            seen.insert(name)
            let exists = searchDirs.contains {
                fm.fileExists(atPath: $0.appendingPathComponent(name).path)
            }
            if !exists {
                missing.append(Missing(kind: kind, fileName: name, referencedBy: declaredId))
            }
        }
        return Report(schemaId: declaredId, missing: missing)
    }

    /// 逐行掃描,依**上一個看到的鍵**解讀值。
    ///
    /// 序列會沿用父鍵,這正是 `dependencies:\n  - stroke` 解得出來的原因。
    ///
    /// ⚠ 這裡不用 MiniYaml,理由與 `SchemaCatalog.readHeader` 相同:
    /// 方案檔是完整 YAML,RTS 解析器會在合法的方案上報錯。
    static func collect(text: String, into out: inout [(String, Kind)]) {
        var currentKey = ""
        for raw in text.components(separatedBy: "\n") {
            var line = raw.replacingOccurrences(of: "\r", with: "")
            // 砍掉行尾註解。引號裡的 `#` 很罕見,而誤砍的代價只是少檢查一項。
            if let hash = line.firstIndex(of: "#"),
               line[line.startIndex..<hash].trimmingCharacters(in: .whitespaces).isEmpty
                || !line[line.startIndex..<hash].contains("\"") {
                line = String(line[line.startIndex..<hash])
            }
            let trimmed = line.trimmingCharacters(in: .whitespaces)
            if trimmed.isEmpty { continue }

            if trimmed.hasPrefix("- ") || trimmed == "-" {
                let value = SchemaCatalog.stripQuotes(
                    String(trimmed.dropFirst()).trimmingCharacters(in: .whitespaces))
                add(key: currentKey, value: value, into: &out)
                continue
            }
            guard let colon = trimmed.firstIndex(of: ":") else { continue }
            let key = String(trimmed[trimmed.startIndex..<colon]).trimmingCharacters(in: .whitespaces)
            let value = SchemaCatalog.stripQuotes(
                String(trimmed[trimmed.index(after: colon)...]).trimmingCharacters(in: .whitespaces))
            currentKey = key
            if !value.isEmpty, !value.hasPrefix("[") { add(key: key, value: value, into: &out) }
            // 行內序列 `dependencies: [stroke, terra_pinyin]`
            if value.hasPrefix("["), value.hasSuffix("]") {
                for item in value.dropFirst().dropLast().components(separatedBy: ",") {
                    add(key: key, value: SchemaCatalog.stripQuotes(
                        item.trimmingCharacters(in: .whitespaces)), into: &out)
                }
            }
        }
    }

    static func add(key: String, value: String, into out: inout [(String, Kind)]) {
        guard !value.isEmpty else { return }
        switch key {
        case "dictionary":
            out.append(("\(value).dict.yaml", .dictionary))
        case "dependencies":
            out.append(("\(value)\(SchemaCatalog.suffix)", .schema))
        case "import_preset":
            out.append(("\(value).yaml", .config))
        case "__include", "__patch":
            if let target = includeTarget(value) { out.append((target, .config)) }
        case "language":
            // librime 的語言模型檔名慣例是 `zh-hans-t-essay-bgw.gram` 這種帶連字號的。
            // 不含連字號的 `language:` 多半是別的意思,不要猜。
            if value.contains("-") { out.append(("\(value).gram", .grammar)) }
        default:
            break
        }
    }

    /// `pinyin:/xxx` → `pinyin.yaml`;`symbols.yaml:/xxx` → `symbols.yaml`;
    /// `/local/node` → nil(同文件內的節點參照,不需要檔案)。
    static func includeTarget(_ raw: String) -> String? {
        let v = raw.trimmingCharacters(in: .whitespaces)
        guard !v.hasPrefix("/") else { return nil }
        var head = v.components(separatedBy: ":/")[0]
        head = head.components(separatedBy: ":")[0].trimmingCharacters(in: .whitespaces)
        guard !head.isEmpty else { return nil }
        return head.hasSuffix(".yaml") ? head : head + ".yaml"
    }

    /// 一組方案的預檢。任何一個有問題就整組不通過 ——
    /// 部署是全有全無的,放行一半沒有意義。
    public static func checkAll(schemaIds: [String], searchDirs: [URL],
                                fm: FileManager = .default) -> [Missing] {
        var out: [Missing] = []
        for id in schemaIds {
            let name = id + SchemaCatalog.suffix
            guard let dir = searchDirs.first(where: {
                fm.fileExists(atPath: $0.appendingPathComponent(name).path)
            }) else {
                out.append(Missing(kind: .schema, fileName: name, referencedBy: id))
                continue
            }
            out.append(contentsOf: check(schemaFile: dir.appendingPathComponent(name),
                                         searchDirs: searchDirs, fm: fm).missing)
        }
        return out
    }
}
