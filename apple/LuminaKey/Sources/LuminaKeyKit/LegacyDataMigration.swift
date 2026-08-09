//
//  LegacyDataMigration.swift — 改名之後,舊的使用者資料目錄怎麼辦
//
//  ── 為什麼需要這個檔案 ──────────────────────────────────────────────────
//  產品從開發代號 `RimeQuad` 改成 `LuminaKey`,使用者資料目錄跟著從
//  `~/Library/Application Support/RimeQuad` 換成 `.../LuminaKey`。
//  **改了目錄名 = 使用者的詞庫、設定、裝過的方案全部找不到了**,而症狀是
//  「升級之後一切回到出廠狀態」—— 沒有錯誤訊息,只是東西不見了。
//
//  bundle id 也一起改了,所以舊版的 .app 不會被新版覆蓋:它還留在
//  `~/Library/Input Methods/RimeQuad.app`,而且**可能正在跑**。
//  這件事決定了底下三個設計:
//
//    1. **複製,不是搬移。** 舊的那一份原封不動留著。就算這裡整段出錯,
//       使用者也還能把舊版選回來繼續用。
//    2. **只做一次,由「新目錄還不存在」這件事本身當旗標。** 不另外寫狀態檔:
//       多一個狀態檔就多一種「旗標說做過了但其實沒有」的壞法。
//    3. **不搬 `*.userdb`。** 那是 LevelDB 目錄,而舊版可能正開著它。
//       複製一個開著的 LevelDB 會得到一份**壞掉但看起來正常**的資料庫,
//       而 librime 部署失敗時 `rs_last_error()` 是空字串 —— 使用者會看到
//       「部署失敗」四個字,沒有原因,而且每次啟動都失敗。
//       寧可讓「學到的字頻」重新累積(它本來就會自己長回來),
//       也不要把一顆不定時炸彈搬進新目錄。
//
//  ⚠ 使用者**自己加的詞**(`custom_phrase.txt`,純文字)、設定、裝過的方案
//    與它們的索引都會搬過去。真正會失去的只有 librime 學到的使用頻率。
//    這個取捨寫在 apple/README.md §2。
//
//  ⚠ 這一層是**純邏輯 + FileManager**,沒有 AppKit,有單元測試
//    (`LegacyDataMigrationTests`,含一個變異測試證明它真的在斷言)。
//

import Foundation

public enum LegacyDataMigration {

    /// 改名前的目錄名。**這個字串不可以跟著改名腳本一起被取代** ——
    /// 它指的是磁碟上已經存在的舊目錄,不是產品現在的名字。
    public static let legacyDirectoryName = "RimeQuad"
    public static let currentDirectoryName = "LuminaKey"

    /// 改名前的市集安裝紀錄檔名。搬過去時要一起換成現在的名字,
    /// 否則 `InstalledRegistry` 讀不到它,使用者裝過的方案會變成「沒裝過」
    /// (而檔案還在,於是下次安裝會撞到已經存在的檔案)。
    public static let legacyRegistryFileName = "rimequad-store.json"

    /// 名稱完全相符就不搬。全部都是 librime 自己會重建、或搬過去有害的。
    public static let skippedNames: Set<String> = [
        "build",              // 部署產生的快取,下次部署自己會重建
        "sync",               // 同步暫存
        "installation.yaml",  // 安裝 id。複製過去 = 兩份安裝共用同一個 id
        "user.yaml",          // librime 自動產生
        ".DS_Store",
    ]

    /// 名稱裡含這一段就不搬(`luna_pinyin.userdb`、`*.userdb.txt`、
    /// `*.userdb.kct` 都算)。理由見檔頭第 3 點。
    public static let skippedNameFragment = ".userdb"

    // MARK: - 純判斷(沒有檔案系統,好測)

    public static func shouldSkip(_ name: String) -> Bool {
        if name.isEmpty { return true }
        if skippedNames.contains(name) { return true }
        if name.contains(skippedNameFragment) { return true }
        return false
    }

    /// 搬過去之後叫什麼名字。只有安裝紀錄要改名,其餘原名照搬。
    public static func targetName(for name: String) -> String {
        name == legacyRegistryFileName ? InstalledRegistry.fileName : name
    }

    // MARK: - 真的搬

    public enum Outcome: Equatable {
        /// 新目錄已經在了(含全新安裝)。什麼都不做。
        case alreadyPresent
        /// 沒有舊目錄可搬。
        case noLegacyData
        /// 搬了幾個項目過去。
        case migrated(items: Int)
        /// 出錯了。**呼叫端不該因此中止啟動** —— 最壞的結果就是使用者
        /// 得到一個乾淨的新目錄,而舊的那一份仍然完好。
        case failed(String)
    }

    /// 一次性、非破壞性的搬遷。
    ///
    /// - Parameter appSupportDir: `~/Library/Application Support`。
    ///
    /// 先搬進一個暫存目錄再整個 rename 過去,所以中途失敗**不會**留下一個
    /// 半滿的新目錄把下一次重試擋掉。
    @discardableResult
    public static func run(appSupportDir: URL,
                           fm: FileManager = .default) -> Outcome {
        let legacy = appSupportDir.appendingPathComponent(legacyDirectoryName)
        let target = appSupportDir.appendingPathComponent(currentDirectoryName)

        if fm.fileExists(atPath: target.path) { return .alreadyPresent }

        var isDir: ObjCBool = false
        guard fm.fileExists(atPath: legacy.path, isDirectory: &isDir), isDir.boolValue else {
            return .noLegacyData
        }

        let staging = appSupportDir
            .appendingPathComponent(".\(currentDirectoryName)-migrating-\(UUID().uuidString)")
        do {
            try fm.createDirectory(at: staging, withIntermediateDirectories: true)
        } catch {
            return .failed("建不出暫存目錄: \(error.localizedDescription)")
        }

        var moved = 0
        do {
            let names = try fm.contentsOfDirectory(atPath: legacy.path).sorted()
            for name in names where !shouldSkip(name) {
                try fm.copyItem(at: legacy.appendingPathComponent(name),
                                to: staging.appendingPathComponent(targetName(for: name)))
                moved += 1
            }
            try fm.moveItem(at: staging, to: target)
        } catch {
            // 另一個行程可能剛好搶先建好了新目錄 —— 那不是失敗。
            try? fm.removeItem(at: staging)
            if fm.fileExists(atPath: target.path) { return .alreadyPresent }
            return .failed(error.localizedDescription)
        }
        return .migrated(items: moved)
    }

    /// 給呼叫端印進 log 的一句話。`nil` = 沒有值得說的事。
    public static func logLine(_ outcome: Outcome) -> String? {
        switch outcome {
        case .alreadyPresent, .noLegacyData:
            return nil
        case .migrated(let n):
            return "把 \(legacyDirectoryName) 的 \(n) 個項目搬進 \(currentDirectoryName)"
                + "(舊目錄原封不動留著;學到的字頻不搬,理由見 LegacyDataMigration 檔頭)"
        case .failed(let why):
            return "舊資料搬遷失敗,改用全新的使用者目錄: \(why)"
        }
    }
}
