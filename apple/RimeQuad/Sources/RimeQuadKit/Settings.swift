//
//  Settings.swift — 使用者設定的資料模型、讀寫與遷移
//
//  ── 三條原則(與 Android 的 UserPrefs 同源,四端共用)────────────────────
//
//  1. **「沒設過」與「設成預設值」是兩件事。** 幾乎每一個欄位都是 Optional 或
//     帶一個 `.follow*` 的列舉值。理由:主題／方案日後會更新,把一份預設值的
//     副本存起來,等於把使用者永遠釘在他當初看到的那一版上,而他完全不知道
//     自己被釘住了。
//
//  2. **這個檔案只放「UI 層的偏好」。** 凡是 librime 自己會讀的東西
//     (已啟用的方案清單、每頁候選數)一律寫進 `default.custom.yaml`,
//     因為那才是四端共用、而且使用者用別的 RIME 前端也看得懂的地方。
//     哪一項在哪裡,見 docs/settings-model.md §2 的表。
//
//  3. **兩個行程共用一份檔案。** 設定介面與輸入法本體是不同的行程
//     (見 apple/README.md §6),所以設定不放 UserDefaults 而放一份 JSON,
//     改完發一則 DistributedNotification。UserDefaults 的跨行程同步時機
//     沒有保證,而「改了設定要等一下才生效」這種事查不出原因。
//
//  ⚠ 本檔不 import AppKit,也不碰 librime。讀寫是純函式,有單元測試。
//

import Foundation

// MARK: - 列舉

/// 深淺色。
public enum AppearanceMode: String, Codable, CaseIterable, Sendable {
    case followSystem, light, dark
}

/// 候選字級。**刻意不給滑桿也不給 pt 數。**
/// 使用者要的是「大一點」,不是「17.5 pt」;而且 pt 數在不同螢幕上意思不同。
public enum CandidateScale: String, Codable, CaseIterable, Sendable {
    case small, medium, large, extraLarge

    /// 乘在主題宣告的字級上。1.0 = 主題原本的大小。
    public var factor: Double {
        switch self {
        case .small: return 0.85
        case .medium: return 1.0
        case .large: return 1.2
        case .extraLarge: return 1.45
        }
    }
}

/// 「跟著主題」是第三態,不是 false。
public enum ThemeBoolPref: String, Codable, CaseIterable, Sendable {
    case followTheme, on, off

    public func resolved(themeValue: Bool) -> Bool {
        switch self {
        case .followTheme: return themeValue
        case .on: return true
        case .off: return false
        }
    }
}

public enum OrientationPref: String, Codable, CaseIterable, Sendable {
    case followTheme, horizontal, vertical
}

/// 簡繁。`followInputMode` 是預設值 —— 使用者在系統的輸入來源選了
/// 「簡體」或「繁體」,那就是他的意思,不必再問一次。
public enum VariantPref: String, Codable, CaseIterable, Sendable {
    case followInputMode, traditional, simplified
}

/// 標點與字寬。`followSchema` = 不干預,讓方案自己的預設生效。
public enum PunctuationPref: String, Codable, CaseIterable, Sendable {
    case followSchema, full, half
}

public enum ShapePref: String, Codable, CaseIterable, Sendable {
    case followSchema, halfShape, fullShape
}

/// 介面語言。`system` = 跟著 macOS。
public enum UiLanguage: String, Codable, CaseIterable, Sendable {
    case system, zhHant = "zh-Hant", zhHans = "zh-Hans", en
}

// MARK: - 模型

public struct RimeQuadSettings: Equatable, Sendable {

    /// 目前的結構版本。**遷移的唯一依據**,不要拿欄位存不存在來猜。
    public static let currentVersion = 1

    public var version: Int = RimeQuadSettings.currentVersion

    // ── 方案 ──────────────────────────────────────────────
    /// 「輸入模式(繁/簡)決定用哪個方案與簡繁」的總開關。
    public var followInputMode: Bool = true
    /// 不分模式,一律用這個方案。
    public var pinnedSchemaId: String?
    /// 繁體輸入模式底下用這個。優先於 `pinnedSchemaId`。
    public var pinnedSchemaHant: String?
    /// 簡體輸入模式底下用這個。
    public var pinnedSchemaHans: String?

    // ── 外觀 ──────────────────────────────────────────────
    /// 主題家族 id(不含 `-light` / `-dark` 後綴)。nil = 預設。
    /// 存家族而不是存具體那一份,深淺才能是另一個獨立的控制項。
    public var themeFamily: String?
    public var appearance: AppearanceMode = .followSystem
    public var candidateScale: CandidateScale = .medium
    public var candidateOrientation: OrientationPref = .followTheme
    public var showStatusBar: ThemeBoolPref = .followTheme
    public var showCandidateLabels: ThemeBoolPref = .followTheme

    // ── 文字 ──────────────────────────────────────────────
    public var variant: VariantPref = .followInputMode
    public var punctuation: PunctuationPref = .followSchema
    public var shape: ShapePref = .followSchema

    // ── 連網 ──────────────────────────────────────────────
    /// **預設關閉。** 這是這個專案的定位,不是一個可有可無的預設值。
    public var networkEnabled: Bool = false
    /// 方案市集的索引位址。nil = 用內建的。
    public var storeIndexUrl: String?

    // ── 介面 ──────────────────────────────────────────────
    public var uiLanguage: UiLanguage = .system

    /// 「已經看過第一次的說明」。不是設定,是一件發生過的事實 ——
    /// 所以「全部回復預設」會保留它(見 `resetKeepingFacts`)。
    public var seenFirstRunNotice: Bool = false

    public init() {}
}

// MARK: - JSON 編解碼

/// 刻意手寫而不用 `Codable` 的合成實作。
///
/// 理由只有一個:**讀進來絕對不能失敗**。合成的 `Decodable` 會因為一個型別
/// 對不上的欄位就整份丟出錯誤,而一份壞掉的設定檔會讓使用者的輸入法
/// 回到出廠狀態 —— 他調的每一項都不見了,而且沒有訊息。
/// 這裡的每一個欄位各自回落到預設值,壞掉的那一個欄位壞掉就好。
public enum SettingsCodec {

    public static func decode(_ data: Data) -> RimeQuadSettings {
        guard let obj = try? JSONSerialization.jsonObject(with: data),
              let dict = obj as? [String: Any] else { return RimeQuadSettings() }
        return decode(dict)
    }

    public static func decode(_ d: [String: Any]) -> RimeQuadSettings {
        var s = RimeQuadSettings()
        s.version = int(d["version"]) ?? RimeQuadSettings.currentVersion

        s.followInputMode = bool(d["follow_input_mode"]) ?? s.followInputMode
        s.pinnedSchemaId = str(d["pinned_schema"])
        s.pinnedSchemaHant = str(d["pinned_schema_hant"])
        s.pinnedSchemaHans = str(d["pinned_schema_hans"])

        s.themeFamily = str(d["theme_family"])
        s.appearance = enumOf(d["appearance"]) ?? s.appearance
        s.candidateScale = enumOf(d["candidate_scale"]) ?? s.candidateScale
        s.candidateOrientation = enumOf(d["candidate_orientation"]) ?? s.candidateOrientation
        s.showStatusBar = enumOf(d["show_status_bar"]) ?? s.showStatusBar
        s.showCandidateLabels = enumOf(d["show_candidate_labels"]) ?? s.showCandidateLabels

        s.variant = enumOf(d["variant"]) ?? s.variant
        s.punctuation = enumOf(d["punctuation"]) ?? s.punctuation
        s.shape = enumOf(d["shape"]) ?? s.shape

        s.networkEnabled = bool(d["network_enabled"]) ?? s.networkEnabled
        s.storeIndexUrl = str(d["store_index_url"])

        s.uiLanguage = enumOf(d["ui_language"]) ?? s.uiLanguage
        s.seenFirstRunNotice = bool(d["seen_first_run_notice"]) ?? s.seenFirstRunNotice

        return SettingsMigration.migrate(s)
    }

    public static func encode(_ s: RimeQuadSettings) -> Data {
        var d: [String: Any] = [
            "version": RimeQuadSettings.currentVersion,
            "follow_input_mode": s.followInputMode,
            "appearance": s.appearance.rawValue,
            "candidate_scale": s.candidateScale.rawValue,
            "candidate_orientation": s.candidateOrientation.rawValue,
            "show_status_bar": s.showStatusBar.rawValue,
            "show_candidate_labels": s.showCandidateLabels.rawValue,
            "variant": s.variant.rawValue,
            "punctuation": s.punctuation.rawValue,
            "shape": s.shape.rawValue,
            "network_enabled": s.networkEnabled,
            "ui_language": s.uiLanguage.rawValue,
            "seen_first_run_notice": s.seenFirstRunNotice,
        ]
        // nil 一律**不寫進檔案**,而不是寫 null。
        // 「這個鍵不存在」與「這個鍵是 null」在別的工具眼裡是兩件事,
        // 而我們的語義只有一種:沒設過。
        if let v = s.pinnedSchemaId { d["pinned_schema"] = v }
        if let v = s.pinnedSchemaHant { d["pinned_schema_hant"] = v }
        if let v = s.pinnedSchemaHans { d["pinned_schema_hans"] = v }
        if let v = s.themeFamily { d["theme_family"] = v }
        if let v = s.storeIndexUrl { d["store_index_url"] = v }

        let opts: JSONSerialization.WritingOptions = [.prettyPrinted, .sortedKeys]
        return (try? JSONSerialization.data(withJSONObject: d, options: opts)) ?? Data("{}".utf8)
    }

    // ── 寬鬆取值 ──────────────────────────────────────────
    static func str(_ v: Any?) -> String? {
        guard let s = v as? String, !s.isEmpty else { return nil }
        return s
    }
    static func int(_ v: Any?) -> Int? {
        if let i = v as? Int { return i }
        if let n = v as? NSNumber { return n.intValue }
        if let s = v as? String { return Int(s) }
        return nil
    }
    static func bool(_ v: Any?) -> Bool? {
        if let b = v as? Bool { return b }
        if let n = v as? NSNumber { return n.boolValue }
        if let s = v as? String {
            if ["true", "yes", "1", "on"].contains(s.lowercased()) { return true }
            if ["false", "no", "0", "off"].contains(s.lowercased()) { return false }
        }
        return nil
    }
    static func enumOf<T: RawRepresentable>(_ v: Any?) -> T? where T.RawValue == String {
        guard let s = v as? String else { return nil }
        return T(rawValue: s)
    }
}

// MARK: - 遷移

public enum SettingsMigration {

    /// 把任何一版的設定帶到 `currentVersion`。
    ///
    /// ⚠ **未來版本的檔案不得被降級寫回。** 使用者可能在兩台機器之間同步
    /// (`~/Library/Application Support/RimeQuad/` 會被 iCloud/Dropbox 帶走),
    /// 舊版看到新版的檔案時,能讀的欄位照讀,但**不覆寫** ——
    /// 覆寫的話新機器上調好的東西會被舊機器洗掉,而且沒有任何跡象。
    /// 這個判斷交給 `SettingsStore.save` 的 `isDowngrade`。
    public static func migrate(_ input: RimeQuadSettings) -> RimeQuadSettings {
        var s = input
        if s.version < 1 {
            // v0 = 沒有版本欄位的最早期檔案。當時沒有任何欄位語義變更,
            // 所以只補上版本號。這一格存在是為了讓下一次遷移有樣本可抄。
            s.version = 1
        }
        s.version = max(s.version, 1)
        return s
    }

    /// 讀進來的版本比本版新 → 不要寫回去。
    public static func isFromFutureVersion(_ s: RimeQuadSettings) -> Bool {
        s.version > RimeQuadSettings.currentVersion
    }
}

// MARK: - 存取

/// 設定檔的讀寫。**與檔案系統的唯一接觸點**,方便測試時換掉。
public protocol SettingsFileIO {
    func read(_ url: URL) -> Data?
    func write(_ data: Data, to url: URL) throws
}

public struct DiskSettingsIO: SettingsFileIO {
    public init() {}
    public func read(_ url: URL) -> Data? { try? Data(contentsOf: url) }
    public func write(_ data: Data, to url: URL) throws {
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(),
                                                withIntermediateDirectories: true)
        // 原子寫入:半份設定檔比沒有設定檔更糟 —— 它讀得起來,只是內容是錯的。
        try data.write(to: url, options: .atomic)
    }
}

public final class SettingsStore {

    public static let fileName = "settings.json"
    /// 兩個行程之間的通知。設定改了 → 輸入法本體立刻重讀。
    public static let changedNotification = "org.rimequad.settings.changed"

    private let url: URL
    private let io: SettingsFileIO
    private(set) public var current: RimeQuadSettings

    public init(url: URL, io: SettingsFileIO = DiskSettingsIO()) {
        self.url = url
        self.io = io
        self.current = io.read(url).map(SettingsCodec.decode) ?? RimeQuadSettings()
    }

    /// 從磁碟重讀。回傳是否真的變了 —— 沒變就不必叫 UI 重畫。
    @discardableResult
    public func reload() -> Bool {
        let fresh = io.read(url).map(SettingsCodec.decode) ?? RimeQuadSettings()
        let changed = fresh != current
        current = fresh
        return changed
    }

    /// 改一項並存檔。回傳是否真的寫了。
    @discardableResult
    public func update(_ mutate: (inout RimeQuadSettings) -> Void) -> Bool {
        var next = current
        mutate(&next)
        guard next != current else { return false }
        // 讀到的是更新版的檔案時只讀不寫(見 SettingsMigration 的註解)。
        guard !SettingsMigration.isFromFutureVersion(current) else {
            current = next
            return false
        }
        next.version = RimeQuadSettings.currentVersion
        current = next
        try? io.write(SettingsCodec.encode(next), to: url)
        return true
    }

    /// 「全部回復預設」。
    ///
    /// **已經發生過的事實不歸零。** 使用者按這顆按鈕的意思是「設定歸零」,
    /// 不是「假裝我沒用過這個 app」;把 `seenFirstRunNotice` 一起清掉的話,
    /// 他會再被攔一次第一次說明,而他明明看過了。
    ///
    /// **連網開關也不歸零成「開」** —— 預設本來就是關,這裡只是再說一次。
    @discardableResult
    public func resetKeepingFacts() -> Bool {
        update { s in
            let seen = s.seenFirstRunNotice
            s = RimeQuadSettings()
            s.seenFirstRunNotice = seen
        }
    }

    /// 使用者到底有沒有改過任何東西(不算「已經發生過的事實」)。
    /// UI 用它決定「回復預設」要不要是灰的 —— 一顆按了什麼都不會發生的按鈕
    /// 比沒有這顆按鈕更糟。
    public var isPristine: Bool {
        var probe = current
        probe.seenFirstRunNotice = false
        var base = RimeQuadSettings()
        base.version = probe.version
        return probe == base
    }
}
