//
//  EffectiveSchemaList.swift — 「現在到底啟用了哪些方案」
//
//  ── 這一檔修的是什麼 ────────────────────────────────────────────────────
//  設定介面問「啟用了哪些方案」時，一直只讀 `default.custom.yaml` 的
//  `patch/schema_list`（`StoreEngine.enabledSchemas`）。那個檔案是**使用者
//  改過之後才會存在**的東西：沒有它時 librime 照樣有一整份方案清單
//  （來自 `default.yaml` 的 `schema_list`），而設定畫面回答的是「零個」。
//
//  症狀正是 Windows 端使用者拍到的那一張：**引擎有方案，清單沒東西。**
//  在這一端還多一層後果 —— `AppContext.applySchemaForInputMode()` 也讀同一個
//  函式，拿到空陣列時它挑不出方案，於是上一輪修好的「繁／簡輸入來源 →
//  對應方案」整條靜靜地不做事。
//
//  ⚠ 兩端的**根因不同**，不要照抄對方的修法：
//    · Windows 是服務行程自己有 librime，問題在怎麼問；
//    · 這一端的設定介面是**另一個行程、沒有 librime**（見 apple/README.md §6），
//      它只能看檔案。所以這裡的修法是「把 librime 的查找順序照做一次」：
//      **`patch` 有就用 `patch`，沒有就用底層的 `default.yaml`。**
//
//  ⚠ 這一檔**只讀不寫**。寫仍然一律走 `RimeConfigPatch`（寫進
//    `default.custom.yaml`），因為 `default.yaml` 是上游檔案，改它會在下一次
//    更新時衝突 —— 那是 `scripts/collect_data.sh` 當初選 patch 的理由。
//

import Foundation

public enum SchemaListSource: String, Sendable {
    /// 來自 `default.custom.yaml` 的 `patch/schema_list`（使用者改過）。
    case custom
    /// 來自 `default.yaml` 的頂層 `schema_list`（隨附的預設）。
    case base
    /// 兩邊都沒有。這是真的「一個方案都沒有」，畫面上該出現說明卡。
    case none
}

public struct EffectiveSchemaList: Equatable, Sendable {
    public let ids: [String]
    public let source: SchemaListSource

    public init(ids: [String], source: SchemaListSource) {
        self.ids = ids
        self.source = source
    }

    public static let empty = EffectiveSchemaList(ids: [], source: .none)
}

public enum SchemaListReader {

    public static let baseFileName = "default.yaml"

    /// `patch` 優先，其次 `default.yaml`。
    ///
    /// 為什麼是「覆蓋」而不是「合併」：librime 的 `patch:` 對一個序列鍵是
    /// **整段取代**，不是附加。合併會讓使用者取消勾選的方案又冒出來。
    public static func resolve(customText: String, baseText: String) -> EffectiveSchemaList {
        let patched = RimeConfigPatch.readSchemaList(text: customText)
        if !patched.isEmpty { return EffectiveSchemaList(ids: patched, source: .custom) }
        let base = readBaseSchemaList(text: baseText)
        if !base.isEmpty { return EffectiveSchemaList(ids: base, source: .base) }
        return .empty
    }

    /// 讀 `default.yaml` 頂層的 `schema_list`。
    ///
    /// ⚠ **刻意不用 MiniYaml。** 上游的 `default.yaml` 用了 `__include`、
    ///   `__patch` 與巢狀映射，那些不在 RTS（規範 §3 的 YAML 子集）裡；
    ///   拿 MiniYaml 去解析它會在一份完全合法的檔案上失敗，而失敗的樣子
    ///   是「清單又變成空的」—— 跟這一檔要修的缺陷一模一樣。
    ///   所以這裡與 `SchemaCatalog.readHeader` 同一個策略：
    ///   只認得一個鍵的行掃描器，看不懂的一律跳過。
    public static func readBaseSchemaList(text: String) -> [String] {
        var out: [String] = []
        var inList = false
        for raw in text.components(separatedBy: "\n") {
            let line = raw.trimmingCharacters(in: .whitespaces)
            if line.isEmpty || line.hasPrefix("#") { continue }
            let indent = raw.prefix { $0 == " " }.count
            if indent == 0 {
                if line == "---" || line == "..." { continue }
                // 頂層換了一個鍵 → 清單結束。
                inList = (line == "schema_list:")
                continue
            }
            guard inList else { continue }
            guard line.hasPrefix("-") else {
                // 縮排的非清單行（例如註解掉的區塊之後的東西）。保守起見停下來，
                // 不要把別的區塊的內容當成方案。
                continue
            }
            var rest = String(line.dropFirst()).trimmingCharacters(in: .whitespaces)
            if rest.hasPrefix("schema:") {
                rest = String(rest.dropFirst("schema:".count)).trimmingCharacters(in: .whitespaces)
            }
            let id = rest.components(separatedBy: "#")[0].trimmingCharacters(in: .whitespaces)
            guard !id.isEmpty, RimeConfigPatch.isPlainId(id), !out.contains(id) else { continue }
            out.append(id)
        }
        return out
    }

    // MARK: - 檔案

    /// `userDir` 的 `default.custom.yaml` + `default.yaml`。
    ///
    /// `default.yaml` 依 librime 的查找順序先看使用者目錄、再看隨附目錄 ——
    /// 順序不一致的話，設定畫面顯示的清單會跟引擎實際載入的那一份不同，
    /// 而那種落差沒有任何線索可循（同 `SchemaCatalog.scan` 的註解）。
    public static func resolve(userDir: URL?, sharedDir: URL?) -> EffectiveSchemaList {
        func read(_ dir: URL?, _ name: String) -> String? {
            guard let dir else { return nil }
            return try? String(contentsOf: dir.appendingPathComponent(name), encoding: .utf8)
        }
        let custom = read(userDir, RimeConfigPatch.fileName) ?? ""
        let base = read(userDir, baseFileName) ?? read(sharedDir, baseFileName) ?? ""
        return resolve(customText: custom, baseText: base)
    }
}
