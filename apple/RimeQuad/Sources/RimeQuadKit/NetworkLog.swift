//
//  NetworkLog.swift — 連網紀錄
//
//  這份紀錄是給**使用者**看的,不是給我們除錯用的。它要回答的問題只有一個:
//  「你說你不連網,那你到底連過哪裡?」
//
//  ── 這裡刻意不記什麼 ────────────────────────────────────────────────────
//  · **不記使用者輸入的任何內容。** 一個字都不記。輸入內容根本不會離開
//    librime 與候選窗,也不會進入這一層。
//  · **不記 URL 的路徑與查詢字串。** 只記主機名,加一個我們自己給的用途標籤。
//    一個標榜隱私的 app 若為了「透明」而把使用者的行為細節寫成一份本機檔案,
//    那是自打嘴巴。
//  · **不記被開關擋下來的嘗試。** 見 NetworkGate.blockedOrNil 的註解 ——
//    「開關從沒開過 → 紀錄是空的」這句話必須成立,那正是使用者驗證我們的方式。
//

import Foundation

public enum NetworkPurpose: String, Sendable, CaseIterable {
    case storeIndex, storePackage, updateManifest

    public var label: T {
        switch self {
        case .storeIndex:
            return T("取得方案清單", "获取方案清单", "Fetching the schema list")
        case .storePackage:
            return T("下載方案套件", "下载方案套件", "Downloading a schema package")
        case .updateManifest:
            return T("檢查更新", "检查更新", "Checking for updates")
        }
    }
}

public enum NetworkOutcome: String, Sendable {
    case ok, failed, redirected
}

public struct NetworkLogEntry: Equatable, Sendable {
    public let at: Date
    /// **只有主機名**,沒有路徑、沒有查詢字串。
    public let host: String
    public let purpose: NetworkPurpose
    /// 人看得懂的補充(例如方案的名字)。由呼叫端給,不從 URL 抽。
    public let label: String
    public let outcome: NetworkOutcome
    public let bytes: Int64
    public let detail: String

    public init(at: Date = Date(), host: String, purpose: NetworkPurpose, label: String,
                outcome: NetworkOutcome, bytes: Int64, detail: String) {
        self.at = at
        self.host = host
        self.purpose = purpose
        self.label = label
        self.outcome = outcome
        self.bytes = bytes
        self.detail = detail
    }
}

/// 一行一筆的 JSON(JSON Lines)。
///
/// 為什麼不是一份大 JSON 陣列:附加一筆只要 append 一行,不必讀進來整份再寫回去。
/// 行程被砍掉時最多壞掉最後一行,而讀取端會跳過讀不懂的行 ——
/// 一份大陣列被截斷則是整份都讀不出來。
public final class NetworkLogFile {

    public static let fileName = "network-log.jsonl"
    /// 超過就從最舊的開始丟。紀錄是給人看的,不是稽核日誌;
    /// 幾千行之後沒有人會捲到底。
    public static let maxEntries = 500

    public let url: URL
    private let queue = DispatchQueue(label: "org.rimequad.netlog")

    public init(directory: URL) {
        self.url = directory.appendingPathComponent(NetworkLogFile.fileName)
    }

    public func append(_ e: NetworkLogEntry) {
        queue.sync {
            let line = NetworkLogFile.encode(e) + "\n"
            guard let data = line.data(using: .utf8) else { return }
            let fm = FileManager.default
            try? fm.createDirectory(at: url.deletingLastPathComponent(),
                                    withIntermediateDirectories: true)
            if let h = try? FileHandle(forWritingTo: url) {
                defer { try? h.close() }
                _ = try? h.seekToEnd()
                try? h.write(contentsOf: data)
            } else {
                try? data.write(to: url)
            }
        }
    }

    public func read() -> [NetworkLogEntry] {
        queue.sync {
            guard let text = try? String(contentsOf: url, encoding: .utf8) else { return [] }
            let all = text.components(separatedBy: "\n").compactMap(NetworkLogFile.decode)
            return Array(all.suffix(NetworkLogFile.maxEntries))
        }
    }

    public func clear() {
        queue.sync { try? FileManager.default.removeItem(at: url) }
    }

    /// 修剪。呼叫時機是 UI 打開紀錄那一頁,不是每次 append ——
    /// append 要快,而且它可能發生在下載的迴圈裡。
    public func trim() {
        queue.sync {
            guard let text = try? String(contentsOf: url, encoding: .utf8) else { return }
            let lines = text.components(separatedBy: "\n").filter { !$0.isEmpty }
            guard lines.count > NetworkLogFile.maxEntries else { return }
            let kept = lines.suffix(NetworkLogFile.maxEntries).joined(separator: "\n") + "\n"
            try? kept.write(to: url, atomically: true, encoding: .utf8)
        }
    }

    // MARK: - 編解碼(純函式,有測試)

    public static func encode(_ e: NetworkLogEntry) -> String {
        let obj: [String: Any] = [
            "at": Int(e.at.timeIntervalSince1970 * 1000),
            "host": e.host,
            "purpose": e.purpose.rawValue,
            "label": e.label,
            "outcome": e.outcome.rawValue,
            "bytes": e.bytes,
            "detail": e.detail,
        ]
        guard let d = try? JSONSerialization.data(withJSONObject: obj, options: [.sortedKeys]),
              let s = String(data: d, encoding: .utf8) else { return "{}" }
        return s
    }

    public static func decode(_ line: String) -> NetworkLogEntry? {
        let t = line.trimmingCharacters(in: .whitespaces)
        guard !t.isEmpty, let d = t.data(using: .utf8),
              let obj = try? JSONSerialization.jsonObject(with: d) as? [String: Any],
              let host = obj["host"] as? String,
              let purposeRaw = obj["purpose"] as? String,
              let purpose = NetworkPurpose(rawValue: purposeRaw),
              let outcomeRaw = obj["outcome"] as? String,
              let outcome = NetworkOutcome(rawValue: outcomeRaw) else { return nil }
        let ms = (obj["at"] as? NSNumber)?.doubleValue ?? 0
        return NetworkLogEntry(
            at: Date(timeIntervalSince1970: ms / 1000),
            host: host, purpose: purpose,
            label: obj["label"] as? String ?? "",
            outcome: outcome,
            bytes: (obj["bytes"] as? NSNumber)?.int64Value ?? 0,
            detail: obj["detail"] as? String ?? "")
    }
}
