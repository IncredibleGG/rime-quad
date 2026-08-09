//
//  SettingsTests.swift — 設定的讀寫、遷移,以及**介面說得像人話**
//

import XCTest
@testable import LuminaKeyKit

private final class MemoryIO: SettingsFileIO {
    var data: Data?
    func read(_ url: URL) -> Data? { data }
    func write(_ d: Data, to url: URL) throws { data = d }
}

final class SettingsTests: XCTestCase {

    private let url = URL(fileURLWithPath: "/dev/null/settings.json")

    func testDefaultsAreOfflineAndNonIntrusive() {
        let s = LuminaKeySettings()
        // 離線是這個專案的定位,不是一個可有可無的預設值。
        XCTAssertFalse(s.networkEnabled)
        XCTAssertTrue(s.followInputMode)
        XCTAssertEqual(s.variant, .followInputMode)
        XCTAssertEqual(s.punctuation, .followSchema)
        XCTAssertEqual(s.appearance, .followSystem)
        XCTAssertNil(s.themeFamily)
        XCTAssertNil(s.pinnedSchemaId)
    }

    func testRoundTrip() {
        var s = LuminaKeySettings()
        s.networkEnabled = true
        s.candidateScale = .large
        s.pinnedSchemaHans = "pinyin_simp"
        s.themeFamily = "sakura"
        s.uiLanguage = .en
        XCTAssertEqual(SettingsCodec.decode(SettingsCodec.encode(s)), s)
    }

    /// nil **不寫成 null**,而是整個鍵不存在。
    /// 「沒設過」與「設成 null」在別的工具眼裡是兩件事,我們只有一種語義。
    func testNilFieldsAreOmitted() {
        let json = String(data: SettingsCodec.encode(LuminaKeySettings()), encoding: .utf8)!
        XCTAssertFalse(json.contains("pinned_schema"))
        XCTAssertFalse(json.contains("theme_family"))
        XCTAssertFalse(json.contains("null"))
    }

    /// **讀進來絕對不能失敗。** 一份壞掉的設定檔會讓使用者調過的每一項
    /// 都消失,而且沒有訊息。所以壞掉的欄位各自回落。
    func testCorruptFieldsFallBackIndividually() {
        let json = """
        { "version": 1, "network_enabled": "yes", "candidate_scale": "gigantic",
          "appearance": 42, "pinned_schema": "", "variant": "simplified" }
        """
        let s = SettingsCodec.decode(Data(json.utf8))
        XCTAssertTrue(s.networkEnabled)                 // "yes" 讀得懂
        XCTAssertEqual(s.candidateScale, .medium)        // 不認得 → 預設
        XCTAssertEqual(s.appearance, .followSystem)      // 型別不對 → 預設
        XCTAssertNil(s.pinnedSchemaId)                   // 空字串 = 沒設過
        XCTAssertEqual(s.variant, .simplified)           // 好的欄位照樣生效
    }

    func testGarbageIsNotFatal() {
        XCTAssertEqual(SettingsCodec.decode(Data("not json at all".utf8)), LuminaKeySettings())
        XCTAssertEqual(SettingsCodec.decode(Data("[1,2,3]".utf8)), LuminaKeySettings())
        XCTAssertEqual(SettingsCodec.decode(Data()), LuminaKeySettings())
    }

    func testMigrationFillsVersion() {
        let s = SettingsCodec.decode(Data("{\"network_enabled\": true}".utf8))
        XCTAssertEqual(s.version, LuminaKeySettings.currentVersion)
        XCTAssertTrue(s.networkEnabled)
    }

    /// 未來版本的檔案**只讀不寫**。兩台機器同步時,舊版覆寫新版
    /// 會把新機器上調好的東西洗掉,而且沒有任何跡象。
    func testFutureVersionIsNotWrittenBack() {
        let io = MemoryIO()
        io.data = Data("{\"version\": 99, \"network_enabled\": true}".utf8)
        let store = SettingsStore(url: url, io: io)
        XCTAssertTrue(SettingsMigration.isFromFutureVersion(store.current))
        let before = io.data
        XCTAssertFalse(store.update { $0.candidateScale = .large })
        XCTAssertEqual(io.data, before, "未來版本的檔案不得被覆寫")
        // 但記憶體裡的值仍然更新了,使用者這一次的操作看得到效果。
        XCTAssertEqual(store.current.candidateScale, .large)
    }

    func testUpdateReturnsFalseWhenNothingChanged() {
        let store = SettingsStore(url: url, io: MemoryIO())
        XCTAssertTrue(store.update { $0.candidateScale = .large })
        XCTAssertFalse(store.update { $0.candidateScale = .large })
    }

    /// 「全部回復預設」把設定歸零,但**不把已經發生過的事實歸零**。
    func testResetKeepsFacts() {
        let store = SettingsStore(url: url, io: MemoryIO())
        store.update { s in
            s.seenFirstRunNotice = true
            s.candidateScale = .extraLarge
            s.networkEnabled = true
        }
        store.resetKeepingFacts()
        XCTAssertTrue(store.current.seenFirstRunNotice, "看過說明是事實,不是設定")
        XCTAssertEqual(store.current.candidateScale, .medium)
        XCTAssertFalse(store.current.networkEnabled)
    }

    func testIsPristineIgnoresFacts() {
        let store = SettingsStore(url: url, io: MemoryIO())
        XCTAssertTrue(store.isPristine)
        store.update { $0.seenFirstRunNotice = true }
        XCTAssertTrue(store.isPristine, "只看過說明不算改過設定")
        store.update { $0.candidateScale = .small }
        XCTAssertFalse(store.isPristine)
    }

    func testCandidateScaleFactors() {
        XCTAssertEqual(CandidateScale.medium.factor, 1.0)
        XCTAssertLessThan(CandidateScale.small.factor, 1.0)
        XCTAssertGreaterThan(CandidateScale.large.factor, 1.0)
        XCTAssertGreaterThan(CandidateScale.extraLarge.factor, CandidateScale.large.factor)
    }

    func testThemeBoolPrefFollowsTheme() {
        XCTAssertTrue(ThemeBoolPref.followTheme.resolved(themeValue: true))
        XCTAssertFalse(ThemeBoolPref.followTheme.resolved(themeValue: false))
        XCTAssertTrue(ThemeBoolPref.on.resolved(themeValue: false))
        XCTAssertFalse(ThemeBoolPref.off.resolved(themeValue: true))
    }
}

// MARK: - 介面本身的性質

/// 這一組測的不是程式對不對,是**介面說不說人話**。
///
/// 使用者的原話:「你這個第一屏 99% 的人看不懂」。原因不是選項多,
/// 是每個選項都用實作的詞在講話。把它變成 CI 上的斷言,
/// 才不會下一次新增設定時又忘記。
final class SettingsCatalogTests: XCTestCase {

    func testEveryItemHasPlainLanguageInAllThreeLanguages() {
        for item in SettingsCatalog.allItems {
            for lang in [UiLanguage.zhHant, .zhHans, .en] {
                XCTAssertFalse(item.title[lang].trimmingCharacters(in: .whitespaces).isEmpty,
                               "\(item.id) 的標題在 \(lang.rawValue) 是空的")
                XCTAssertFalse(item.blurb[lang].trimmingCharacters(in: .whitespaces).isEmpty,
                               "\(item.id) 的白話說明在 \(lang.rawValue) 是空的")
            }
        }
    }

    /// ⚠ **不要把 YAML 欄位名搬到畫面上。** 這是使用者看不懂第一屏的直接原因。
    func testNoImplementationJargonOnScreen() {
        let banned = ["schema_list", "page_size", "simplification", "ascii_punct",
                      "full_shape", "custom_phrase", "default.custom", "yaml", "YAML",
                      "librime", "rs_deploy", "sha256", "JSON"]
        for item in SettingsCatalog.allItems {
            for lang in [UiLanguage.zhHant, .zhHans, .en] {
                let text = item.title[lang] + " " + item.blurb[lang]
                for word in banned {
                    XCTAssertFalse(text.contains(word),
                                   "\(item.id) 在 \(lang.rawValue) 出現了實作的詞:\(word)")
                }
            }
            if case .choice(let choices) = item.kind {
                for c in choices {
                    for lang in [UiLanguage.zhHant, .zhHans, .en] {
                        for word in banned {
                            XCTAssertFalse(c.label[lang].contains(word),
                                           "\(item.id) 的選項出現了實作的詞:\(word)")
                        }
                    }
                }
            }
        }
    }

    func testPageAndItemIdsAreUnique() {
        let pageIds = SettingsCatalog.pages.map(\.id)
        XCTAssertEqual(Set(pageIds).count, pageIds.count)
        let itemIds = SettingsCatalog.allItems.map(\.id)
        XCTAssertEqual(Set(itemIds).count, itemIds.count)
    }

    func testEveryPageHasItems() {
        for page in SettingsCatalog.pages {
            XCTAssertFalse(page.items.isEmpty, "\(page.id) 是一頁空白")
            for lang in [UiLanguage.zhHant, .zhHans, .en] {
                XCTAssertFalse(page.title[lang].isEmpty)
                XCTAssertFalse(page.subtitle[lang].isEmpty)
            }
        }
    }

    /// **每一個設定欄位都必須在目錄裡出現。** 新增一個欄位而忘了給它
    /// 白話說明與分頁,這一條會紅 —— 那正是「使用者看不懂」的來源。
    func testEverySettingsFieldAppearsInTheCatalog() {
        let covered = Set(SettingsCatalog.allItems.compactMap(\.field))
        var missing: [String] = []
        for child in Mirror(reflecting: LuminaKeySettings()).children {
            guard let label = child.label else { continue }
            if SettingsCatalog.internalFields.contains(label) { continue }
            if !covered.contains(label) { missing.append(label) }
        }
        XCTAssertTrue(missing.isEmpty,
                      "這些設定沒有出現在任何一頁,使用者改不到:\(missing.joined(separator: ", "))")
    }

    /// 目錄裡宣告的欄位名必須真的存在。打錯字的話上面那條會被繞過去。
    func testCatalogFieldsExistOnTheModel() {
        let real = Set(Mirror(reflecting: LuminaKeySettings()).children.compactMap(\.label))
        for item in SettingsCatalog.allItems {
            guard let f = item.field else { continue }
            XCTAssertTrue(real.contains(f), "\(item.id) 指向不存在的欄位 \(f)")
        }
    }

    /// 桌面端沒有「手感」那一頁 —— 震動、音效、長按都是軟鍵盤專屬,
    /// 做一頁空的比不做更糟。
    func testNoMobileOnlyPage() {
        XCTAssertNil(SettingsCatalog.page(id: "feel"))
        XCTAssertNotNil(SettingsCatalog.page(id: "schemas"))
        XCTAssertNotNil(SettingsCatalog.page(id: "network"))
    }

    /// ⚠ **「自己加的詞」上架了,而上架是有條件的。**
    ///
    /// 這一頁曾經被刻意拿下來:`verify_user_dict.sh` 用真的 librime 證明
    /// 「加了詞之後候選裡沒有那個詞」。一頁按鈕都按得下去、加完還看得到、
    /// 而回去打字什麼都不會發生的設定頁,比沒有這一頁更糟。
    ///
    /// 2026-08-09 找到原因(編碼欄的空格,見 `UserPhrases` 檔頭)並修好,
    /// 那一關在 CI 裡也從 `continue-on-error` 改成硬失敗,所以它回到線上。
    ///
    /// **這條斷言現在釘的是反過來的那件事:它必須在側欄裡,而且真的有內容。**
    /// 哪天有人又把它從 `pages` 拿掉,這條會紅,而拿掉它的正當理由只有一個 ——
    /// `verify_user_dict.sh` 紅了。那時請連同這段註解一起改。
    func testDictionaryPageIsShipped() {
        XCTAssertNotNil(SettingsCatalog.page(id: "dictionary"),
                        "「自己加的詞」那一頁不在側欄裡")
        XCTAssertEqual(SettingsCatalog.dictionaryPage.id, "dictionary")
        XCTAssertFalse(SettingsCatalog.dictionaryPage.items.isEmpty)
        // 那一頁的三件事都要在:看清單、匯出、匯入。
        let ids = Set(SettingsCatalog.dictionaryPage.items.map(\.id))
        XCTAssertEqual(ids, ["dictionary.list", "dictionary.export", "dictionary.import"])
        // ⚠ 這一頁的說明必須講到「不要空格」—— 那是使用者唯一會踩、
        //   而且踩了完全沒有跡象的地方。少了這一句,這一頁就是在教人做錯。
        let blurb = SettingsCatalog.dictionaryPage.items[0].blurb
        XCTAssertTrue(blurb.hant.contains("空格"), "加詞的說明沒有提醒不要空格")
        XCTAssertTrue(blurb.en.lowercased().contains("space"), "英文版沒有提醒不要空格")
    }
}

final class LocalizationTests: XCTestCase {

    func testSystemLanguageFallsBackToEnglishNotChinese() {
        // 看不懂中文的人看到中文完全沒有辦法自救;
        // 看得懂中文的人看到英文至少還能對照。
        XCTAssertEqual(L10n.resolve(.en), .en)
        XCTAssertEqual(L10n.resolve(.zhHant), .zhHant)
    }

    func testFormatUsesPositionalArguments() {
        let t = T("{0} 之後有 {1}", "{0} 之后有 {1}", "{1} after {0}")
        XCTAssertEqual(t.format(.zhHant, "甲", "乙"), "甲 之後有 乙")
        XCTAssertEqual(t.format(.en, "A", "B"), "B after A")
    }
}
