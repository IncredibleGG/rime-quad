//
//  KeyRemapStore.swift — 換鍵存在哪裡
//
//  ── 為什麼是這個檔名、這個目錄 ──────────────────────────────────────────
//  `<user_data_dir>/luminakey-layouts.json`，**與 Android 端同一個檔名、
//  同一份格式**（`android/.../keyboard/UserLayoutStore.kt`）。
//
//  這不是巧合也不是節省：使用者在手機上把 a 跟 s 對調，換到電腦前面按 a
//  就該得到 s。兩端各存各的話，這件事永遠不會發生，而且**不會有任何錯誤
//  訊息** —— 畫面上只是「電腦沒有照我調的做」，他會以為是自己記錯了。
//
//  ⚠ 改名前的舊檔名 `rimequad-layouts.json` **讀得到就接手**。漏掉這一條的
//    下場是升級之後使用者調過的鍵位全部回到原樣：檔案還躺在磁碟上，
//    只是沒有人再去讀它。Android 端有同一條（`LEGACY_FILE_NAME`）。
//
//  ── 為什麼不放 settings.json ────────────────────────────────────────────
//  因為 settings.json 是**本端的 UI 偏好**（見 Settings.swift 檔頭第 2 條），
//  而換鍵是四端共用的使用者資料。放進去等於宣告它不跨端，而那正是使用者
//  在這個功能上最直接的期待。
//
//  ── 兩個行程 ────────────────────────────────────────────────────────────
//  設定介面與輸入法本體是不同的行程。設定介面寫檔之後發一則
//  DistributedNotification（[KeyRemapStore.changedNotification]），輸入法本體
//  收到就重讀並重新編譯那張表。與設定檔走的是同一套機制，理由見 Settings.swift。
//

import Foundation

public final class KeyRemapStore {

    /// 四端共用的檔名。
    public static let fileName = "luminakey-layouts.json"

    /// 產品改名前的檔名。**只讀不寫。**
    public static let legacyFileName = "rimequad-layouts.json"

    /// 換鍵改了 → 輸入法本體立刻重讀。
    public static let changedNotification = "org.luminakey.layouts.changed"

    private let url: URL
    private let legacyURL: URL
    private let io: SettingsFileIO

    private(set) public var document: RemapDocument
    /// 目前生效的那張表。編譯是純函式，這裡只是把結果留著不必每次按鍵重算。
    private(set) public var compiled: RemapCompilation

    public init(userDir: URL, io: SettingsFileIO = DiskSettingsIO()) {
        self.url = userDir.appendingPathComponent(KeyRemapStore.fileName)
        self.legacyURL = userDir.appendingPathComponent(KeyRemapStore.legacyFileName)
        self.io = io
        self.document = RemapDocument()
        self.compiled = RemapCompilation(table: .identity, cycles: [], notices: [], editable: true)
        reload()
    }

    /// 從磁碟重讀並重新編譯。回傳表有沒有真的變 —— 沒變就不必驚動任何人。
    @discardableResult
    public func reload() -> Bool {
        let before = compiled.table
        // 先新名字，沒有才退回改名前的那一份。
        let data = io.read(url) ?? io.read(legacyURL)
        document = data.map(RemapDocument.decode) ?? RemapDocument()
        compiled = DesktopRemap.compile(document)
        return compiled.table != before
    }

    /// 寫回磁碟。
    ///
    /// 寫成功之後才收掉改名前的那一份：兩份同時在的話，之後每一次讀都得
    /// 決定要信哪一份，而那個決定遲早會做錯一次。
    @discardableResult
    public func save(_ next: RemapDocument) -> Bool {
        do {
            try io.write(next.encode(), to: url)
        } catch {
            return false
        }
        if FileManager.default.fileExists(atPath: legacyURL.path) {
            try? FileManager.default.removeItem(at: legacyURL)
        }
        document = next
        compiled = DesktopRemap.compile(next)
        return true
    }

    /// 這台電腦上有沒有任何換鍵可以還原。
    public var hasAnythingToRestore: Bool {
        !compiled.cycles.isEmpty || !compiled.notices.isEmpty
    }
}
