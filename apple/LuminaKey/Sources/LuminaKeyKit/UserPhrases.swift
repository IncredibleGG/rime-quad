//
//  UserPhrases.swift — 使用者自己加的詞
//
//  ── 為什麼是 custom_phrase.txt 而不是 librime 的使用者詞典 ────────────────
//  librime 的使用者詞典(userdb)是 LevelDB,只有 librime 自己讀得懂,
//  而 `core/include/rime_shell.h` 的 ABI **沒有**匯出它的增刪介面
//  (要加得由協調端動 core/,四端一起改)。
//
//  RIME 本身另有一條路:`table_translator@custom_phrase` + `db_class: stabledb`,
//  讀的是使用者目錄裡一份**純文字 TSV**。它有三個對這個專案剛好的性質:
//
//    1. **四端都做得到。** 純文字,沒有二進位格式、沒有位元組序、沒有版本鎖。
//       Android 端的 task #39 可以直接讀同一個檔案。
//    2. **使用者看得懂、改得動、備份得了。** 一個標榜「你的資料是你的」的
//       輸入法,詞庫不該是一坨只有我們解得開的東西。
//    3. **是 RIME 的既有機制,不是我們發明的。** 使用者哪天換去鼠鬚管或
//       小狼毫,這份檔案照樣有效。
//
//  格式的規範性描述在 docs/settings-model.md §5。**改這裡就要改那裡。**
//
//  ⚠ 這一層只管檔案內容。要生效必須 (a) 每個方案有對應的 .custom.yaml 掛載,
//    (b) 重新部署。兩件事分別由 `schemaPatchText` 與呼叫端負責。
//

import Foundation

/// 一筆詞。
public struct UserPhrase: Equatable, Sendable {
    /// 上屏的文字。
    public let text: String
    /// 打什麼會出現它。空白分隔的音節,例如 `ni hao`。
    public let code: String
    /// 越大越前面。預設 1。
    public let weight: Int

    public init(text: String, code: String, weight: Int = 1) {
        self.text = text
        self.code = code
        self.weight = weight
    }

    /// 同一個「詞 + 編碼」視為同一筆 —— 匯入時用它去重。
    public var identity: String { text + "\u{1}" + code }
}

public enum UserPhraseError: Error, Equatable {
    /// 這一行有問題,以及為什麼。行號從 1 起算。
    case badLine(line: Int, reason: T)
}

public enum UserPhrases {

    /// 詞庫檔名。**必須**與 `schemaPatchText` 裡的 `user_dict` 相同。
    public static let fileName = "custom_phrase.txt"

    /// 檔頭。匯出的檔案帶著它,讓收到檔案的人知道這是什麼。
    /// 讀取時任何 `#` 開頭的行都會被跳過,所以它不影響解析。
    public static let header = """
        # LuminaKey 使用者詞庫 / user phrases — format 1
        # 每行三欄,用 TAB 分隔:  詞<TAB>編碼<TAB>權重
        # 編碼是你要打的拼音/字根,音節之間用空格。權重越大越前面。
        # 這是 RIME 的 custom_phrase 格式,四端通用,也能給其他 RIME 前端使用。
        """

    /// 一筆詞的上限。不是為了省空間,是為了讓一個壞掉的檔案不會變成
    /// 一個吃光記憶體的檔案。
    public static let maxEntries = 50_000
    public static let maxTextLength = 64
    public static let maxCodeLength = 64
    public static let maxFileBytes = 8 * 1024 * 1024

    // MARK: - 讀

    public struct ParseResult: Equatable, Sendable {
        public var phrases: [UserPhrase] = []
        /// 讀不懂的行。**不中斷解析** —— 一行寫壞不該讓整本詞庫消失。
        public var problems: [(line: Int, reason: T)] = []

        public static func == (a: ParseResult, b: ParseResult) -> Bool {
            a.phrases == b.phrases && a.problems.map(\.line) == b.problems.map(\.line)
        }
    }

    public static func parse(_ text: String) -> ParseResult {
        var out = ParseResult()
        for (i, raw) in text.components(separatedBy: "\n").enumerated() {
            let lineNo = i + 1
            // \r 一定要先拿掉:Windows 端匯出的檔案帶 CRLF,留著 \r 會變成
            // 權重欄位「1\r」解析失敗,而錯誤訊息看起來像是使用者打錯字。
            let line = raw.replacingOccurrences(of: "\r", with: "")
            let trimmed = line.trimmingCharacters(in: .whitespaces)
            if trimmed.isEmpty || trimmed.hasPrefix("#") { continue }

            let cols = line.components(separatedBy: "\t")
            guard cols.count >= 2 else {
                out.problems.append((lineNo, T("這一行沒有用 TAB 分成兩欄以上",
                                               "这一行没有用 TAB 分成两栏以上",
                                               "This line is not TAB-separated into at least two columns")))
                continue
            }
            let word = cols[0].trimmingCharacters(in: .whitespaces)
            let code = cols[1].trimmingCharacters(in: .whitespaces)
                .lowercased()
                // 多個空格收成一個,不然 `ni  hao` 與 `ni hao` 會變成兩筆。
                .components(separatedBy: .whitespaces).filter { !$0.isEmpty }.joined(separator: " ")
            guard !word.isEmpty else {
                out.problems.append((lineNo, T("第一欄(詞)是空的", "第一栏(词)是空的",
                                               "The first column (the word) is empty")))
                continue
            }
            guard !code.isEmpty else {
                out.problems.append((lineNo, T("第二欄(編碼)是空的", "第二栏(编码)是空的",
                                               "The second column (the spelling) is empty")))
                continue
            }
            guard word.count <= maxTextLength, code.count <= maxCodeLength else {
                out.problems.append((lineNo, T("這一行太長了", "这一行太长了", "This line is too long")))
                continue
            }
            let weight = cols.count >= 3 ? (Int(cols[2].trimmingCharacters(in: .whitespaces)) ?? 1) : 1
            out.phrases.append(UserPhrase(text: word, code: code, weight: max(0, weight)))
            if out.phrases.count >= maxEntries { break }
        }
        return out
    }

    public static func read(userDir: URL) -> ParseResult {
        let url = userDir.appendingPathComponent(fileName)
        guard let data = try? Data(contentsOf: url), data.count <= maxFileBytes,
              let text = String(data: data, encoding: .utf8) else { return ParseResult() }
        return parse(text)
    }

    // MARK: - 寫

    public static func serialise(_ phrases: [UserPhrase]) -> String {
        var s = header + "\n"
        for p in phrases {
            s += "\(p.text)\t\(p.code)\t\(p.weight)\n"
        }
        return s
    }

    public static func write(_ phrases: [UserPhrase], userDir: URL) throws {
        try FileManager.default.createDirectory(at: userDir, withIntermediateDirectories: true)
        try serialise(phrases).write(to: userDir.appendingPathComponent(fileName),
                                     atomically: true, encoding: .utf8)
    }

    // MARK: - 增刪與合併

    /// 加一筆。已經有同樣的「詞 + 編碼」就更新權重,**不會變成兩筆**。
    /// 回傳新的清單與「這是不是一筆新的」。
    public static func adding(_ p: UserPhrase, to list: [UserPhrase]) -> (list: [UserPhrase], isNew: Bool) {
        var out = list
        if let i = out.firstIndex(where: { $0.identity == p.identity }) {
            out[i] = p
            return (out, false)
        }
        // 新的加在最前面:使用者剛加的詞應該一眼看得到,
        // 而不是被埋在一份幾千筆的清單最底下。
        out.insert(p, at: 0)
        return (out, true)
    }

    public static func removing(identity: String, from list: [UserPhrase]) -> [UserPhrase] {
        list.filter { $0.identity != identity }
    }

    /// 匯入。**合併,不取代** —— 使用者按的是「匯入」,不是「用這份蓋掉我的」。
    ///
    /// 衝突時取權重大的那一筆;權重相同時保留原有的(匯入不該改變既有排序)。
    public struct MergeResult: Equatable, Sendable {
        public let list: [UserPhrase]
        public let added: Int
        public let updated: Int
        public let skipped: Int
    }

    public static func merging(_ incoming: [UserPhrase], into list: [UserPhrase]) -> MergeResult {
        var out = list
        var index = Dictionary(uniqueKeysWithValues: list.enumerated().map { ($1.identity, $0) })
        var added = 0, updated = 0, skipped = 0
        for p in incoming {
            if let i = index[p.identity] {
                if p.weight > out[i].weight { out[i] = p; updated += 1 } else { skipped += 1 }
            } else {
                guard out.count < maxEntries else { skipped += 1; continue }
                out.append(p)
                index[p.identity] = out.count - 1
                added += 1
            }
        }
        return MergeResult(list: out, added: added, updated: updated, skipped: skipped)
    }

    // MARK: - 掛載到方案

    /// LuminaKey 自動產生的 `<schema>.custom.yaml` 內容。
    ///
    /// ⚠ **只有在這個檔案不存在、或存在但是我們寫的時候才可以覆蓋。**
    /// 使用者自己寫的 `<schema>.custom.yaml` 常常有他調了很久的東西
    /// (按鍵綁定、翻頁鍵、模糊音),蓋掉它是不可逆的損失。
    /// 判定方式是第一行的標記,見 `isOurs`。
    public static let marker = "# luminakey-managed: custom_phrase v1"

    /// 產品改名之前(RimeQuad)我們自己寫出去的標記。
    ///
    /// ⚠ **少了這一行,改名就會把使用者的掛載檔變成「使用者自己的檔案」** ——
    /// `isOurs` 回 false,於是「我的詞庫」再也不會更新它,UI 說的是
    /// 「這是你自己的檔案,沒有動」,而使用者從頭到尾沒改過那個檔案。
    /// 搬遷會把舊的 `<schema>.custom.yaml` 原樣帶過來(見 LegacyDataMigration),
    /// 所以這裡必須認得舊標記。
    public static let legacyMarkers = ["# rimequad-managed: custom_phrase v1"]

    public static func schemaPatchText() -> String {
        """
        \(marker)
        # 這個檔案由 LuminaKey 的「我的詞庫」自動產生。
        # 如果你想自己改這個方案的設定,請先把第一行的標記刪掉 ——
        # 之後 LuminaKey 就不會再動這個檔案了(你的詞庫也會停止對這個方案生效)。
        patch:
          "engine/translators/@next": table_translator@custom_phrase
          custom_phrase:
            dictionary: ""
            user_dict: custom_phrase
            db_class: stabledb
            enable_completion: false
            enable_sentence: false
            initial_quality: 99
        """ + "\n"
    }

    public static func isOurs(_ text: String) -> Bool {
        guard let first = text.components(separatedBy: "\n").first?
            .trimmingCharacters(in: .whitespaces) else { return false }
        return first == marker || legacyMarkers.contains(first)
    }

    public enum MountOutcome: Equatable, Sendable {
        case written
        case alreadyOurs
        /// 使用者自己的檔案,沒有動。附上檔名讓 UI 說得出是哪一個。
        case skippedUserOwned(String)
        case failed(String)
    }

    /// 讓 `schemaId` 這個方案讀得到使用者詞庫。
    public static func mount(schemaId: String, userDir: URL,
                             fm: FileManager = .default) -> MountOutcome {
        let name = "\(schemaId).custom.yaml"
        let url = userDir.appendingPathComponent(name)
        if let existing = try? String(contentsOf: url, encoding: .utf8) {
            guard isOurs(existing) else { return .skippedUserOwned(name) }
            if existing == schemaPatchText() { return .alreadyOurs }
        }
        do {
            try fm.createDirectory(at: userDir, withIntermediateDirectories: true)
            try schemaPatchText().write(to: url, atomically: true, encoding: .utf8)
            return .written
        } catch {
            return .failed(error.localizedDescription)
        }
    }
}
