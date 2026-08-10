//
//  UserDataSeed.swift — 第一次啟動時把隨附的使用者初始配置放進使用者目錄
//
//  ── 這一檔修的是什麼 ────────────────────────────────────────────────────
//  `scripts/collect_data.sh` 會產生 `core/data/user/default.custom.yaml`，
//  它把 `schema_list` 限縮成**這個專案實際打包的四個方案**
//  （luna_pinyin_tw / bopomofo_tw / luna_pinyin / t9_pinyin）。
//  上游 rime-prelude 的 `default.yaml` 列的是另外六個，其中 cangjie5 與 quick5
//  我們根本沒有打包。
//
//  Windows 端把那個檔案裝進使用者目錄（`windows/service/main.cc`，
//  `verify_installer.sh` 有斷言）；**這一端從來沒有**：`build_app.sh` 只
//  複製了 `core/data/shared`。後果在真的 Mac 上是三件事一起發生：
//
//    1. librime 照上游那一份部署 → cangjie5／quick5 找不到而報錯，
//       而**朙月拼音·臺灣正體、注音·臺灣正體、九宮格根本不在啟用清單裡**。
//    2. 設定 › 輸入方案：一列都沒有打勾（`enabledSchemas` 讀不到那個檔案）。
//    3. `applySchemaForInputMode()` 拿到空清單 → 繁／簡輸入來源綁定不做事。
//
//  ⚠ CI 一直是綠的，因為 `rime_console` 是**直接**把 `core/data/user` 當
//    使用者目錄傳進去的 —— 它驗的那份資料，使用者手上那一份 .app 從來沒有。
//
//  ── 規則 ────────────────────────────────────────────────────────────────
//  **只補不覆蓋。** 使用者（或別的 RIME 前端）自己寫過的 `default.custom.yaml`
//  絕對不能被我們洗掉：那裡面是他勾選的方案順序與每頁候選數。
//  所以這一支是冪等的，兩個行程各叫一次也無所謂（同 `LegacyDataMigration`）。
//

import Foundation

public enum UserDataSeed {

    /// `.app` 裡放範本的位置（相對於 `Contents/Resources`）。
    /// ⚠ 這個名字同時出現在 `apple/scripts/build_app.sh` 與
    ///   `apple/scripts/verify_app_bundle.sh`，改一個要改三個。
    public static let templateDirectoryName = "UserTemplate"

    public struct Outcome: Equatable, Sendable {
        /// 這一次真的複製過去的檔名。
        public let copied: [String]
        /// 已經存在，所以沒有動的檔名。**這不是錯誤**，是常態。
        public let kept: [String]
        /// 複製失敗的檔名。
        public let failed: [String]
        /// 範本目錄不存在或是空的 —— 代表這份 .app 的內容不完整。
        public let templateMissing: Bool

        public static let noTemplate =
            Outcome(copied: [], kept: [], failed: [], templateMissing: true)
    }

    /// 把 `templateDir` 底下的檔案補進 `userDir`。
    ///
    /// 只處理**第一層的一般檔案**：範本裡今天只有 `default.custom.yaml`，
    /// 而遞迴複製會在有人不小心把整個 shared 目錄放進去時搬幾十 MB。
    /// 需要目錄時再加，並且要同時加測試。
    @discardableResult
    public static func run(templateDir: URL, userDir: URL,
                           fm: FileManager = .default) -> Outcome {
        let names = ((try? fm.contentsOfDirectory(atPath: templateDir.path)) ?? [])
            .filter { !$0.hasPrefix(".") }
            .sorted()
        guard !names.isEmpty else { return .noTemplate }

        try? fm.createDirectory(at: userDir, withIntermediateDirectories: true)

        var copied: [String] = []
        var kept: [String] = []
        var failed: [String] = []
        for name in names {
            let src = templateDir.appendingPathComponent(name)
            var isDir: ObjCBool = false
            guard fm.fileExists(atPath: src.path, isDirectory: &isDir), !isDir.boolValue else {
                continue
            }
            let dst = userDir.appendingPathComponent(name)
            if fm.fileExists(atPath: dst.path) { kept.append(name); continue }
            do {
                try fm.copyItem(at: src, to: dst)
                copied.append(name)
            } catch {
                failed.append(name)
            }
        }
        return Outcome(copied: copied, kept: kept, failed: failed, templateMissing: false)
    }

    /// 值得寫進 log 的那一行；沒事發生時回 nil（不要每次啟動都吐一行）。
    public static func logLine(_ o: Outcome) -> String? {
        if o.templateMissing {
            return "使用者初始配置範本不存在（.app 內容不完整）—— "
                + "librime 會照上游 default.yaml 部署，啟用清單會是錯的"
        }
        var parts: [String] = []
        if !o.copied.isEmpty { parts.append("補上 \(o.copied.joined(separator: ", "))") }
        if !o.failed.isEmpty { parts.append("複製失敗 \(o.failed.joined(separator: ", "))") }
        return parts.isEmpty ? nil : "使用者初始配置：" + parts.joined(separator: "；")
    }
}
