import XCTest
@testable import LuminaKeyKit

/// 改名之後,`~/Library/Application Support/RimeQuad` 裡的東西怎麼辦。
///
/// ⚠ 這一組**真的動檔案系統**(暫存目錄),因為要測的就是檔案有沒有搬過去。
///   純判斷的部分(`shouldSkip` / `targetName`)另外測,那兩個不碰磁碟。
final class LegacyDataMigrationTests: XCTestCase {

    private var root: URL!

    override func setUpWithError() throws {
        root = FileManager.default.temporaryDirectory
            .appendingPathComponent("luminakey-migrate-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    }

    override func tearDownWithError() throws {
        try? FileManager.default.removeItem(at: root)
    }

    // MARK: - 純判斷

    func testSkipsOnlyWhatLibrimeRebuilds() {
        for name in ["build", "sync", "installation.yaml", "user.yaml", ".DS_Store",
                     "luna_pinyin.userdb", "luna_pinyin.userdb.txt", "t9_pinyin.userdb.kct"] {
            XCTAssertTrue(LegacyDataMigration.shouldSkip(name), "\(name) 應該不搬")
        }
        for name in ["custom_phrase.txt", "settings.json", "themes",
                     "luna_pinyin_tw.custom.yaml", "default.custom.yaml",
                     "rimequad-store.json", "opencc"] {
            XCTAssertFalse(LegacyDataMigration.shouldSkip(name), "\(name) 應該要搬")
        }
    }

    /// 安裝紀錄跟著改名 —— 少了這一條,使用者裝過的方案會變成「沒裝過」,
    /// 而檔案還躺在目錄裡。
    func testRegistryFileIsRenamed() {
        XCTAssertEqual(LegacyDataMigration.targetName(for: "rimequad-store.json"),
                       InstalledRegistry.fileName)
        XCTAssertEqual(InstalledRegistry.fileName, "luminakey-store.json",
                       "改了檔名就要一起改 legacyRegistryFileName 的對照")
        XCTAssertEqual(LegacyDataMigration.targetName(for: "custom_phrase.txt"),
                       "custom_phrase.txt")
    }

    // MARK: - 真的搬

    private func write(_ rel: String, _ text: String) throws {
        let url = root.appendingPathComponent(rel)
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(),
                                                withIntermediateDirectories: true)
        try Data(text.utf8).write(to: url)
    }

    private func exists(_ rel: String) -> Bool {
        FileManager.default.fileExists(atPath: root.appendingPathComponent(rel).path)
    }

    private func text(_ rel: String) -> String? {
        try? String(contentsOf: root.appendingPathComponent(rel), encoding: .utf8)
    }

    private func makeLegacyTree() throws {
        try write("RimeQuad/custom_phrase.txt", "測試詞彙鑰匙\tzhuang\t99\n")
        try write("RimeQuad/settings.json", "{\"version\":1}")
        try write("RimeQuad/rimequad-store.json", "{\"packages\":[]}")
        try write("RimeQuad/luna_pinyin_tw.custom.yaml", "patch: {}\n")
        try write("RimeQuad/themes/mine.yaml", "id: mine\n")
        try write("RimeQuad/luna_pinyin.userdb/CURRENT", "leveldb")
        try write("RimeQuad/installation.yaml", "installation_id: old\n")
        try write("RimeQuad/build/luna_pinyin.table.bin", "cache")
    }

    func testMigratesUserOwnedDataAndRenamesRegistry() throws {
        try makeLegacyTree()

        let outcome = LegacyDataMigration.run(appSupportDir: root)
        guard case .migrated(let n) = outcome else {
            return XCTFail("預期 .migrated,拿到 \(outcome)")
        }
        XCTAssertEqual(n, 5, "五個項目:詞庫、設定、安裝紀錄、方案 patch、themes/")

        XCTAssertEqual(text("LuminaKey/custom_phrase.txt"), "測試詞彙鑰匙\tzhuang\t99\n")
        XCTAssertEqual(text("LuminaKey/settings.json"), "{\"version\":1}")
        XCTAssertTrue(exists("LuminaKey/luna_pinyin_tw.custom.yaml"))
        XCTAssertTrue(exists("LuminaKey/themes/mine.yaml"), "目錄要整個搬")

        XCTAssertTrue(exists("LuminaKey/\(InstalledRegistry.fileName)"), "安裝紀錄改名搬過去")
        XCTAssertFalse(exists("LuminaKey/rimequad-store.json"), "舊名不該還在新目錄裡")
    }

    /// 不搬的那三類,一個都不准出現在新目錄裡。
    func testDoesNotCarryOverRebuildableState() throws {
        try makeLegacyTree()
        _ = LegacyDataMigration.run(appSupportDir: root)

        XCTAssertFalse(exists("LuminaKey/luna_pinyin.userdb"),
                       "LevelDB 不搬 —— 舊版可能正開著它,複製會得到一份壞掉的 DB")
        XCTAssertFalse(exists("LuminaKey/installation.yaml"), "安裝 id 不搬")
        XCTAssertFalse(exists("LuminaKey/build"), "部署快取不搬")
    }

    /// **非破壞性**:舊目錄必須原封不動。
    func testLegacyDirectoryIsLeftIntact() throws {
        try makeLegacyTree()
        _ = LegacyDataMigration.run(appSupportDir: root)

        XCTAssertTrue(exists("RimeQuad/custom_phrase.txt"))
        XCTAssertTrue(exists("RimeQuad/rimequad-store.json"))
        XCTAssertTrue(exists("RimeQuad/luna_pinyin.userdb/CURRENT"))
    }

    func testNoLegacyDirectoryIsNotAnError() {
        XCTAssertEqual(LegacyDataMigration.run(appSupportDir: root), .noLegacyData)
        XCTAssertFalse(exists("LuminaKey"), "沒有舊資料時不該憑空建出新目錄")
    }

    /// 已經有新目錄就完全不碰它 —— 包括「新目錄裡的檔案比舊的舊」的情況。
    func testExistingCurrentDirectoryIsNeverOverwritten() throws {
        try makeLegacyTree()
        try write("LuminaKey/settings.json", "{\"version\":9}")

        XCTAssertEqual(LegacyDataMigration.run(appSupportDir: root), .alreadyPresent)
        XCTAssertEqual(text("LuminaKey/settings.json"), "{\"version\":9}")
        XCTAssertFalse(exists("LuminaKey/custom_phrase.txt"), "不該補搬")
    }

    func testRunningTwiceIsIdempotent() throws {
        try makeLegacyTree()
        guard case .migrated = LegacyDataMigration.run(appSupportDir: root) else {
            return XCTFail("第一次應該搬")
        }
        XCTAssertEqual(LegacyDataMigration.run(appSupportDir: root), .alreadyPresent)
    }

    /// 失敗與「沒事發生」在 log 上要分得出來。
    func testLogLineOnlySpeaksWhenSomethingHappened() {
        XCTAssertNil(LegacyDataMigration.logLine(.alreadyPresent))
        XCTAssertNil(LegacyDataMigration.logLine(.noLegacyData))
        XCTAssertNotNil(LegacyDataMigration.logLine(.migrated(items: 3)))
        XCTAssertNotNil(LegacyDataMigration.logLine(.failed("磁碟滿了")))
    }
}
