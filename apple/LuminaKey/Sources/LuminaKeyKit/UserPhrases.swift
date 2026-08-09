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
//  ── 2026-08-09:「加了詞卻打不出來」的真正原因 ──────────────────────────
//  這一頁曾經被下架整整一輪,因為 `apple/scripts/verify_user_dict.sh` 用真的
//  librime 驗出「加了詞,候選裡沒有那個詞」。當時猜的三個原因
//  (patch 沒生效 / stabledb 要先編譯 / `dictionary: ""` 沒有 prism)
//  **全部是錯的**。用 host 版 librime 把四種寫法各跑一次之後,答案是第二欄:
//
//      我們寫的是  `測試詞彙鑰匙<TAB>zhuang zhuang zhuang`  ← 打不出來
//      要寫的是    `測試詞彙鑰匙<TAB>zhuangzhuangzhuang`    ← 打得出來
//
//  librime 那一側:`TableTranslator::Query()` 把**使用者按出來的原始字串**
//  直接拿去 `UserDictionary::LookupWords()`,而 stabledb 的鍵是
//  `table_db.cc` 的 `rime_table_entry_parser()` 組出來的 `編碼 + " \t" + 詞`。
//  兩邊要**逐字元相等**才算命中(`key[len] == ' '`)。
//  中間有空格 = 永遠不會命中,而且**沒有任何錯誤訊息** —— 檔案讀進去了、
//  詞條也載入了,只是永遠查不到。這正是本專案反覆踩的那一類缺陷。
//
//  所以編碼欄的正規化是這一層最要緊的一條規則,見 `normaliseCode`。
//

import Foundation

/// 一筆詞。
public struct UserPhrase: Equatable, Sendable {
    /// 打出來的文字。
    public let text: String
    /// **使用者實際會按的那一串鍵,原樣,不加空格。**
    ///
    /// ⚠ 這一欄曾經寫成「空白分隔的音節,例如 `ni hao`」,而那是錯的,
    /// 錯了整整一輪 —— 詳見 `normaliseCode` 的實測依據。正確的是 `nihao`。
    public let code: String
    /// 越大越前面。預設 1。
    public let weight: Int

    public init(text: String, code: String, weight: Int = 1) {
        self.text = text
        self.code = UserPhrases.normaliseCode(code)
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
        # 編碼寫「你按的那一串鍵」,原樣、不加空格:黃小明 → huangxiaoming。
        # ⚠ 寫成 huang xiao ming 是查不到的 —— 中間有空格就永遠不會命中。
        # 權重越大越前面。這是 RIME 的 custom_phrase 格式,四端通用,
        # 也能給其他 RIME 前端使用。
        """

    /// 一筆詞的上限。不是為了省空間,是為了讓一個壞掉的檔案不會變成
    /// 一個吃光記憶體的檔案。
    public static let maxEntries = 50_000
    public static let maxTextLength = 64
    public static let maxCodeLength = 64
    public static let maxFileBytes = 8 * 1024 * 1024

    // MARK: - 編碼欄

    /// 把編碼正規化成「使用者實際會按的那一串鍵」。
    ///
    /// 兩件事:**轉小寫**、**拿掉所有空白**。兩件都是必要的,而且都是實測過的:
    ///
    /// | 檔案裡寫的 | 打 `zhuangzhuangzhuang` 的結果 |
    /// |---|---|
    /// | `zhuang zhuang zhuang` | ✗ 查不到(空格永遠不會命中) |
    /// | `ZHUANGzhuangzhuang`   | ✗ 查不到(librime 只 trim,不轉小寫) |
    /// | `zhuangzhuangzhuang`   | ✓ 排在第一個 |
    ///
    /// ⚠ **這裡放行了空格就等於這一頁又變回「按得下去但什麼都不會發生」。**
    /// 這條規則有反向測試(`testSpaceSeparatedCodeIsCollapsed`)與
    /// `run_kit_tests.sh` 的一格變異看著。
    ///
    /// 全形空白也要拿掉:使用者從別處貼上來的編碼很可能帶著它,
    /// 而它跟半形空格一樣是「永遠查不到」,只是更看不出來。
    public static func normaliseCode(_ raw: String) -> String {
        var out = String.UnicodeScalarView()
        for s in raw.lowercased().unicodeScalars
        where !CharacterSet.whitespacesAndNewlines.contains(s) {
            out.append(s)
        }
        return String(out)
    }

    /// 編碼欄有沒有明顯打不出來的東西。`nil` = 沒問題。
    ///
    /// 只擋**確定不可能命中**的兩種,不猜方案的字母表 ——
    /// 拼音的 `qqq`、注音的 `su3cl3`、五筆的 `aaaa` 都是合法的編碼,
    /// 想從這一層判斷「這個方案打不打得出來」一定會誤傷(SchemaPreflight 同理)。
    ///
    /// ⚠ 最常見的一種是**兩欄填反**:把「黃小明」填進編碼欄。
    /// 那一筆會安安靜靜地存進去、出現在清單裡、然後永遠不會被打出來。
    public static func codeProblem(_ code: String) -> T? {
        let c = normaliseCode(code)
        if c.isEmpty {
            return T("要填你會怎麼打這個詞。", "要填你会怎么打这个词。",
                     "Fill in how you would type it.")
        }
        if c.count > maxCodeLength {
            return T("太長了,最多 \(maxCodeLength) 個字元。",
                     "太长了,最多 \(maxCodeLength) 个字符。",
                     "Too long — at most \(maxCodeLength) characters.")
        }
        // 鍵盤上按得出來的才算數。中文字、注音符號本身都不是你按的鍵。
        if c.unicodeScalars.contains(where: { $0.value < 0x20 || $0.value > 0x7E }) {
            return T("這一欄要填你按的那幾個鍵(像 huangxiaoming),不是中文字。",
                     "这一栏要填你按的那几个键(像 huangxiaoming),不是中文字。",
                     "This box takes the keys you press (like huangxiaoming), not Chinese characters.")
        }
        return nil
    }

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
            // ⚠ 空格全部拿掉,不是收成一個。
            // 舊版收成一個空格,於是 `ni hao` 原樣寫進檔案,而 librime 永遠查不到它。
            // 別的 RIME 前端匯出的檔案也可能是空白分隔的,這裡一併接住。
            let code = normaliseCode(cols[1])
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

    /// ⚠ **讀進來會正規化,但不會回頭改磁碟上那份檔案。**
    ///
    /// 也就是說,一份手寫的、編碼帶空格的 `custom_phrase.txt`,在清單裡會顯示成
    /// 收斂後的樣子(`nihao`),而檔案裡仍然是 `ni hao` —— librime 讀的是檔案,
    /// 所以那幾筆**仍然打不出來,而畫面看起來是對的**。
    ///
    /// 之所以還是這樣做,而不是載入時就把檔案改寫掉:
    /// 改寫會用 `serialise()` 整份重寫,**使用者自己寫在檔案裡的註解會全部消失**。
    /// 為了修一份「本來就從來沒有生效過」的檔案而毀掉使用者寫的東西,不划算。
    ///
    /// 這個窗口在下列任一動作之後就關上了,因為它們都會用正規化過的內容重寫檔案:
    /// 加一個詞、刪一個詞、匯入。**匯入尤其重要** —— 行動端匯出的檔案
    /// 如果照著舊規範寫成空白分隔,匯入這一步會把它接住。
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

    /// 產品改名之前(舊名 RimeQuad)我們自己寫出去的標記。
    ///
    /// ⚠ **少了這一行,改名就會把使用者的掛載檔變成「使用者自己的檔案」** ——
    /// `isOurs` 回 false,於是「我的詞庫」再也不會更新它,UI 說的是
    /// 「這是你自己的檔案,沒有動」,而使用者從頭到尾沒改過那個檔案。
    /// 搬遷會把舊的 `<schema>.custom.yaml` 原樣帶過來(見 LegacyDataMigration),
    /// 所以這裡必須認得舊標記。
    public static let legacyMarkers = ["# rimequad-managed: custom_phrase v1"] // 舊名

    /// - Parameter mountsTranslator: 要不要把翻譯器**接到方案的流程上**。
    ///   已經自帶的方案要傳 `false`,理由見 `schemaAlreadyMountsPhrases`。
    ///
    /// ⚠ 用陣列拼字串而不是多行字面值,是因為 YAML 的縮排在這裡是語意的一部分,
    ///   而多行字面值的縮排取決於結尾 `"""` 的位置 —— 那是排版一改就會壞、
    ///   壞了又只在真的部署時才看得出來的東西。
    public static func schemaPatchText(mountsTranslator: Bool = true) -> String {
        var lines: [String] = [
            marker,
            "# 這個檔案由 LuminaKey 的「自己加的詞」自動產生。",
            "# 如果你想自己改這個方案的設定,請先把第一行的標記刪掉 ——",
            "# 之後 LuminaKey 就不會再動這個檔案了(你加的詞也會停止對這個方案生效)。",
            "patch:",
        ]
        if mountsTranslator {
            lines.append("  \"engine/translators/@next\": table_translator@custom_phrase")
        }
        lines += [
            "  custom_phrase:",
            "    dictionary: \"\"",
            "    user_dict: custom_phrase",
            "    db_class: stabledb",
            "    enable_completion: false",
            "    enable_sentence: false",
            "    initial_quality: 99",
        ]
        return lines.joined(separator: "\n") + "\n"
    }

    /// 這個方案自己是不是已經接好了 `table_translator@custom_phrase`。
    ///
    /// ⚠ **為什麼要問這一題:** RIME 的 patch 只有「接上去」(`@next`),
    /// 沒有「沒有才接」。內建的朙月拼音、注音都**本來就自帶**這個翻譯器,
    /// 於是我們再接一次就變成接了兩個 —— 兩個都讀同一份檔案,
    /// 同一個詞就會在選字窗裡出現**兩次**。
    ///
    /// 內建的兩個方案剛好都有 `uniquifier` 把重複的收掉,所以這件事
    /// 一直沒被看見;實測把 `uniquifier` 拿掉之後,同一個詞立刻變成兩筆
    /// (第 1、2 名都是它)。市集裡的方案沒有人保證有 `uniquifier`。
    ///
    /// ⚠ **判斷不出來時回 `false`(= 照接不誤)。**
    /// 兩種錯法的代價不對等:誤判成「已經有了」→ 不接 → 使用者加的詞
    /// 對這個方案**完全沒有作用,而且沒有任何跡象**;誤判成「沒有」→ 多接一個
    /// → 最壞是清單裡看到兩筆一樣的。前者是這個專案抓過五次的那一種缺陷,
    /// 後者只是難看。所以寧可多接。
    ///
    /// ⚠ **絕對不能把我們自己寫的檔案當成證據。** `luna_pinyin_tw.schema.yaml`
    /// 的 `__patch` 就指向 `luna_pinyin_tw.custom.yaml` —— 那正是我們寫的那一份。
    /// 照著追下去的話,第二次掛載會看到第一次留下的那一行、判定「已經有了」、
    /// 於是把它拿掉,掛載就這樣自己消失。帶標記的檔案一律跳過。
    public static func schemaAlreadyMountsPhrases(
        schemaId: String, searchDirs: [URL], fm: FileManager = .default
    ) -> Bool {
        let needle = "table_translator@custom_phrase"
        var queue = ["\(schemaId)\(SchemaCatalog.suffix)"]
        var seen = Set(queue)
        var examined = 0

        while let name = queue.first {
            queue.removeFirst()
            examined += 1
            if examined > 16 { return false }   // 環或超深的繼承鏈:別猜,照接
            guard let dir = searchDirs.first(where: {
                      fm.fileExists(atPath: $0.appendingPathComponent(name).path)
                  }),
                  let text = try? String(contentsOf: dir.appendingPathComponent(name),
                                         encoding: .utf8)
            else { continue }

            if isOurs(text) { continue }        // 我們自己寫的,不算證據
            if text.contains(needle) { return true }

            var refs: [(String, SchemaPreflight.Kind)] = []
            SchemaPreflight.collect(text: text, into: &refs)
            for (ref, kind) in refs where kind == .config {
                // 我們寫的那一份掛載檔叫這個名字,連讀都不用讀。
                if ref == "\(schemaId).custom.yaml" { continue }
                if seen.insert(ref).inserted { queue.append(ref) }
            }
        }
        return false
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

    /// 讓 `schemaId` 這個方案讀得到使用者加的詞。
    ///
    /// - Parameter searchDirs: 找方案檔的順序,**必須**與 librime 一致:
    ///   `[使用者目錄, 隨附目錄]`。反過來的話,使用者換過的方案檔會被隨附的
    ///   那一份遮住,判斷出來的結果跟實際載入的東西不一致(同 SchemaPreflight)。
    public static func mount(schemaId: String, userDir: URL, searchDirs: [URL],
                             fm: FileManager = .default) -> MountOutcome {
        let name = "\(schemaId).custom.yaml"
        let url = userDir.appendingPathComponent(name)
        let mountsTranslator = !schemaAlreadyMountsPhrases(
            schemaId: schemaId, searchDirs: searchDirs, fm: fm)
        let wanted = schemaPatchText(mountsTranslator: mountsTranslator)
        if let existing = try? String(contentsOf: url, encoding: .utf8) {
            guard isOurs(existing) else { return .skippedUserOwned(name) }
            if existing == wanted { return .alreadyOurs }
        }
        do {
            try fm.createDirectory(at: userDir, withIntermediateDirectories: true)
            try wanted.write(to: url, atomically: true, encoding: .utf8)
            return .written
        } catch {
            return .failed(error.localizedDescription)
        }
    }
}
