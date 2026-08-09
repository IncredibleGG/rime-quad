//
//  RimeConfigPatch.swift — 編輯 default.custom.yaml
//
//  ── 為什麼是文字外科手術,不是「解析再重新輸出」 ──────────────────────────
//  `MiniYaml` 是**讀取器**,沒有 round-trip 能力:它丟掉註解、丟掉引號風格、
//  丟掉區塊順序。解析完再重新輸出的話,使用者自己寫在 default.custom.yaml
//  裡的其他設定與註解會被整段洗掉 —— 而他做的只是「裝一個方案」。
//
//  所以:**用 YAML 解析器讀,只用文字編輯寫**,而且只碰我們要動的那幾行。
//  這一條與 Android 的 SchemaListPatch 是同一個決定,四端行為必須一致,
//  因為同一份 default.custom.yaml 會被兩端輪流編輯(iCloud / 同步目錄)。
//
//  ⚠ 這個檔案不碰 librime,也不部署。改完檔案要生效必須另外呼叫 rs_deploy()。
//

import Foundation

public enum RimeConfigPatch {

    public static let fileName = "default.custom.yaml"

    /// 新增項目的縮排。**四個空格**,與 Android 端一致 ——
    /// 兩端輪流寫同一個檔案時,縮排不同會讓 diff 每次都整片變動。
    static let itemIndent = "    "

    // MARK: - 讀

    /// 已啟用的方案清單(`patch` → `schema_list`)。
    ///
    /// **順序有意義**:第一個是預設方案,也是使用者按 switcher 時的排列順序。
    /// 讀不到(檔案不存在、解析失敗、沒有那個鍵)一律回空陣列而不是拋錯 ——
    /// 「還沒設定過」與「壞掉了」在這一層的處置相同:當作沒有。
    public static func readSchemaList(text: String) -> [String] {
        guard let doc = try? MiniYaml.parse(source: fileName, text: text),
              let root = doc.root.mappingValue,
              let patch = root["patch"]?.mappingValue,
              let seq = patch["schema_list"]?.sequenceValue else { return [] }
        var out: [String] = []
        for node in seq {
            if let m = node.mappingValue, let id = m["schema"]?.scalarValue, !id.isEmpty {
                out.append(id)
            } else if let bare = node.scalarValue, !bare.isEmpty {
                // 上游偶爾會寫成裸字串。接受它是防禦性的,不是鼓勵。
                out.append(bare)
            }
        }
        return out
    }

    public static func readSchemaList(userDir: URL) -> [String] {
        readSchemaList(text: (try? String(contentsOf: userDir.appendingPathComponent(fileName),
                                          encoding: .utf8)) ?? "")
    }

    /// 每頁候選數(`patch` → `menu/page_size`)。
    ///
    /// ⚠ 這是 **librime 的設定,不是主題的**。規範 §8.6.7.1 明訂主題**不得**
    /// 改變一頁有幾個候選 —— 改了會讓序號標籤與使用者按的數字鍵對不上。
    /// 所以「一次顯示幾個候選字」這一項寫在這裡,不寫在 settings.json。
    public static func readPageSize(text: String) -> Int? {
        guard let doc = try? MiniYaml.parse(source: fileName, text: text),
              let root = doc.root.mappingValue,
              let patch = root["patch"]?.mappingValue else { return nil }
        // 兩種寫法都認:`"menu/page_size": 7` 與巢狀的 `menu: { page_size: 7 }`。
        if let s = patch["menu/page_size"]?.scalarValue, let n = Int(s) { return n }
        if let menu = patch["menu"]?.mappingValue,
           let s = menu["page_size"]?.scalarValue, let n = Int(s) { return n }
        return nil
    }

    public static func readPageSize(userDir: URL) -> Int? {
        readPageSize(text: (try? String(contentsOf: userDir.appendingPathComponent(fileName),
                                        encoding: .utf8)) ?? "")
    }

    // MARK: - 寫

    /// 把 `schema_list` 換成 `ids`,其餘一字不動。
    ///
    /// 三種情形:
    ///   1. 已經有 `schema_list:` → 換掉它底下的區塊。
    ///   2. 有 `patch:` 但沒有 `schema_list:` → 插在 `patch:` 之後。
    ///   3. 兩個都沒有(含空檔案)→ 整塊附加在最後。
    public static func writeSchemaList(into original: String, ids: [String]) -> String {
        let existing = existingItemLines(original)
        let body = ids.map { existing[$0] ?? "\(itemIndent)- schema: \($0)" }
        return spliceUnderPatch(original, key: "schema_list", body: body)
    }

    /// 設定每頁候選數。`nil` = 移除這一項(回到方案自己的預設)。
    public static func writePageSize(into original: String, size: Int?) -> String {
        // 巢狀寫法若存在就先拆掉,避免兩種寫法同時存在時 librime 取到哪一個要靠猜。
        var text = removeNestedMenuPageSize(original)
        let body = size.map { ["\(itemIndent)menu/page_size: \($0)"] } ?? []
        text = spliceScalarUnderPatch(text, key: "menu/page_size", body: body)
        return text
    }

    // MARK: - 快照與還原

    /// 整份檔案的內容。`nil` = 當時這個檔案不存在。
    ///
    /// 還原用的是**整份覆寫**而不是「套用反向的編輯」:反向編輯的前提是
    /// 外科手術本身沒有 bug,而那正是出事當下最不該假設的事。
    public struct Snapshot: Equatable, Sendable {
        public let text: String?
        public init(text: String?) { self.text = text }
    }

    public static func snapshot(userDir: URL) -> Snapshot {
        let url = userDir.appendingPathComponent(fileName)
        return Snapshot(text: try? String(contentsOf: url, encoding: .utf8))
    }

    @discardableResult
    public static func restore(_ snap: Snapshot, userDir: URL) -> Bool {
        let url = userDir.appendingPathComponent(fileName)
        if let text = snap.text {
            return (try? text.write(to: url, atomically: true, encoding: .utf8)) != nil
        }
        // 當時不存在 → 還原就是刪掉它。留著一個我們憑空造出來的檔案,
        // 下一次讀會以為使用者設定過。
        try? FileManager.default.removeItem(at: url)
        return true
    }

    public static func write(_ text: String, userDir: URL) throws {
        try FileManager.default.createDirectory(at: userDir, withIntermediateDirectories: true)
        try text.write(to: userDir.appendingPathComponent(fileName),
                       atomically: true, encoding: .utf8)
    }

    // MARK: - 內部:文字外科手術

    /// 掃出檔案裡每一個 `- schema: xxx` / `- xxx` 的**原始那一行**。
    ///
    /// 保留原行是為了讓行尾註解活下來:
    ///   `    - schema: luna_pinyin_tw    # 拼音(臺灣字形)`
    /// 重排順序之後那句註解還在原本那個 id 旁邊。
    static func existingItemLines(_ text: String) -> [String: String] {
        var out: [String: String] = [:]
        for raw in text.components(separatedBy: "\n") {
            let line = raw.trimmingCharacters(in: .whitespaces)
            guard line.hasPrefix("-") else { continue }
            var rest = String(line.dropFirst()).trimmingCharacters(in: .whitespaces)
            if rest.hasPrefix("schema:") {
                rest = String(rest.dropFirst("schema:".count)).trimmingCharacters(in: .whitespaces)
            }
            // 砍掉行尾註解**只是為了取出 id**;存進去的仍然是原始整行。
            let idPart = rest.components(separatedBy: "#")[0].trimmingCharacters(in: .whitespaces)
            guard !idPart.isEmpty, isPlainId(idPart) else { continue }
            if out[idPart] == nil {
                out[idPart] = String(raw.reversed().drop { $0 == " " || $0 == "\t" }.reversed())
            }
        }
        return out
    }

    static func isPlainId(_ s: String) -> Bool {
        !s.isEmpty && s.allSatisfy { $0.isLetter || $0.isNumber || $0 == "_" || $0 == "." || $0 == "-" }
    }

    static func indentOf(_ line: String) -> Int {
        line.prefix { $0 == " " }.count
    }

    /// 把 `body` 放到 `patch:` 底下的 `key:` 區塊(區塊型:key 底下是清單)。
    static func spliceUnderPatch(_ text: String, key: String, body: [String]) -> String {
        splice(text, key: key, header: "  \(key):", body: body, blockStyle: true)
    }

    /// 純量型:`key: value` 就是一行,沒有底下的區塊。
    static func spliceScalarUnderPatch(_ text: String, key: String, body: [String]) -> String {
        // 純量的 body 已經含了 `key: value`,所以 header 為空。
        splice(text, key: key, header: nil, body: body.map { line -> String in
            "  " + line.trimmingCharacters(in: .whitespaces)
        }, blockStyle: false)
    }

    private static func splice(_ text: String, key: String, header: String?,
                               body: [String], blockStyle: Bool) -> String {
        var lines = text.components(separatedBy: "\n")

        var inPatch = false
        var keyLine: Int? = nil
        var keyIndent = 0
        for (i, line) in lines.enumerated() {
            let trimmed = line.trimmingCharacters(in: .whitespaces)
            if trimmed.isEmpty || trimmed.hasPrefix("#") { continue }
            if indentOf(line) == 0 {
                inPatch = trimmed == "patch:" || trimmed.hasPrefix("patch:")
                continue
            }
            guard inPatch else { continue }
            let name = trimmed.hasSuffix(":")
                ? String(trimmed.dropLast())
                : trimmed.components(separatedBy: ":")[0]
            if name.trimmingCharacters(in: .whitespaces) == key {
                keyLine = i
                keyIndent = indentOf(line)
                break
            }
        }

        if let k = keyLine {
            var end = k + 1
            if blockStyle {
                // 吃掉所有縮排比 key 更深的行(空行也一起吃,它們屬於這個區塊)。
                var pendingBlank = 0
                while end < lines.count {
                    let l = lines[end]
                    if l.trimmingCharacters(in: .whitespaces).isEmpty {
                        pendingBlank += 1; end += 1; continue
                    }
                    if indentOf(l) > keyIndent { pendingBlank = 0; end += 1; continue }
                    break
                }
                end -= pendingBlank
            }
            var out = Array(lines[0..<k])
            if body.isEmpty {
                // 值被清掉 → 連 key 那一行一起移除。留一個空的 key 會讓
                // librime 讀到一個空清單,那與「沒有這個 patch」不是同一件事。
            } else {
                if let h = header { out.append(h) }
                out.append(contentsOf: body)
            }
            out.append(contentsOf: lines[end...])
            lines = out
            return normalise(lines)
        }

        guard !body.isEmpty else { return normalise(lines) }

        // 沒有這個 key。找 `patch:`。
        if let p = lines.firstIndex(where: {
            indentOf($0) == 0 && $0.trimmingCharacters(in: .whitespaces).hasPrefix("patch:")
        }) {
            var out = Array(lines[0...p])
            if let h = header { out.append(h) }
            out.append(contentsOf: body)
            out.append(contentsOf: lines[(p + 1)...])
            return normalise(out)
        }

        // 連 patch: 都沒有。
        var out: [String] = []
        let trimmedText = text.trimmingCharacters(in: .whitespacesAndNewlines)
        if !trimmedText.isEmpty {
            out.append(contentsOf: trimmedText.components(separatedBy: "\n"))
            out.append("")
        }
        out.append("patch:")
        if let h = header { out.append(h) }
        out.append(contentsOf: body)
        return normalise(out)
    }

    private static func normalise(_ lines: [String]) -> String {
        let joined = lines.joined(separator: "\n")
        let trimmed = String(joined.reversed().drop { $0 == "\n" || $0 == " " }.reversed())
        return trimmed.isEmpty ? "" : trimmed + "\n"
    }

    /// 拆掉巢狀的 `menu:` → `page_size:`(只在它是我們唯一放進去的東西時)。
    static func removeNestedMenuPageSize(_ text: String) -> String {
        var lines = text.components(separatedBy: "\n")
        guard let menuIdx = lines.firstIndex(where: {
            indentOf($0) == 2 && $0.trimmingCharacters(in: .whitespaces) == "menu:"
        }) else { return text }
        var end = menuIdx + 1
        var children: [String] = []
        while end < lines.count, indentOf(lines[end]) > 2,
              !lines[end].trimmingCharacters(in: .whitespaces).isEmpty {
            children.append(lines[end].trimmingCharacters(in: .whitespaces))
            end += 1
        }
        // 底下不只 page_size 一項就不要動 —— 那是使用者自己寫的東西。
        guard children.count == 1, children[0].hasPrefix("page_size:") else { return text }
        lines.removeSubrange(menuIdx..<end)
        return lines.joined(separator: "\n")
    }
}
