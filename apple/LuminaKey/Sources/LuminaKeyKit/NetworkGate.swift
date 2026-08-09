//
//  ═══════════════════════════════════════════════════════════════════════
//   NetworkGate.swift — 全 app **唯一**的連網出口
//  ═══════════════════════════════════════════════════════════════════════
//
//  這個檔案是為了「經得起審計」而存在的。想確認這個輸入法有沒有偷偷連網,
//  不必讀完整個專案 —— 讀這一個檔案就夠了。
//
//  ── 為什麼光有一顆開關不夠 ──────────────────────────────────────────────
//  macOS 的一般 app 不需要宣告網路權限就連得上網(沙盒之外沒有這一層),
//  所以我們不能拿「權限清單上沒有網路」來說服任何人 —— 那句話在 macOS 上
//  根本沒有對應的東西。我們改成把「你要相信我們」換成「你自己查」,三層:
//
//    1. **單一出口**(本檔)。整個 apple/ 只有這裡碰得到 URLSession。
//       驗證方式是一行 grep,不必讀程式碼:
//
//           grep -rn 'URLSession\|NWConnection\|CFSocket\|Network\.framework' \
//               apple/LuminaKey --include=*.swift
//
//       結果**必須**只出現在本檔。多一處就是這個定位破功。
//       `apple/scripts/verify_single_egress.sh` 在 CI 上把它變成一條斷言,
//       而且它自己有反向測試(植入一行假的 URLSession,斷言會紅)。
//
//    2. **預設拒絕**([policy] 的初值是 `{ false }`)。不是「忘了設定就放行」,
//       而是「沒有人明確安裝過政策就一律拒絕」。就算安裝政策的那一行被誤刪,
//       行為也是**完全離線**而不是完全開放。
//
//    3. **連網紀錄**([NetworkLogFile])。每一次真的建立的連線都留一筆:
//       時間、主機、用途、結果、位元組數。轉址的每一跳都各記一筆 ——
//       「它到底連了哪些主機」不能被一次轉址藏起來。
//
//  ── 執行緒 ──────────────────────────────────────────────────────────────
//  所有公開函式都是**同步阻塞**的,一律由呼叫端放到背景執行緒上跑。
//  這是刻意的:市集的整條流程(下載 → 驗 sha256 → 解壓 → 預檢 → 部署)
//  本來就是一條線,寫成 async 只會讓「使用者中途把開關關掉」這件事更難處理。
//
//  ⚠ 本檔不呼叫任何 `rs_*`,與 rime_shell.h 的執行緒約定無關。
//

import Foundation
import CryptoKit

public enum NetworkGate {

    // MARK: - 常數

    public static let connectTimeout: TimeInterval = 15
    public static let resourceTimeout: TimeInterval = 300
    /// 索引是一份清單,不該比這更大。
    public static let maxTextBytes: Int64 = 8 * 1024 * 1024
    public static let maxRedirects = 5

    /// User-Agent。**刻意不含本專案的名字。**
    ///
    /// 在有審查的網路環境下,一行 `luminakey-macos` 就足以讓使用者被標記為
    /// 「正在使用這個輸入法」—— 被動觀察者連解密都不必。使用者可能因為
    /// 「裝了哪個 app」被關注,而那是我們替他送出去的,不是他選的。
    ///
    /// 拿掉不會變成「沒有 User-Agent」:URLSession 會自己補一個含
    /// app 名稱、版本、CFNetwork 與 Darwin 版本的字串,熵更高不是更低。
    /// 所以送一個**固定、不含任何裝置資訊**的常數值。
    ///
    /// 這不是偽裝成瀏覽器:TLS 握手特徵(JA3)本來就跟瀏覽器不一樣,
    /// 硬湊一個瀏覽器 UA 只會讓「UA 與 TLS 指紋對不上」變成新的特徵。
    /// 目標只有一個 —— **不要自報家門**。
    ///
    /// 瞞不了的部分:DNS 查詢與 TLS 的 SNI 都是明文,網路上的觀察者一定
    /// 看得到我們連的是哪個網域。改 User-Agent 改變不了這件事,
    /// 我們也沒有在介面上假裝改得了。
    public static let userAgent = "Mozilla/5.0"

    // MARK: - 開關與紀錄的接線

    /// 「現在可以連網嗎」。**預設拒絕。**
    ///
    /// 刻意是一個 closure 而不是一個布林欄位:使用者可能在下載進行到一半時
    /// 把開關關掉,每次連線前重新問一次才會立刻生效。
    public static var policy: () -> Bool = { false }

    /// 連網紀錄的接收端。nil = 沒接(單元測試的預設狀態)。
    public static var recorder: ((NetworkLogEntry) -> Void)?

    /// 政策自己爆掉時視為**關閉**。fail-closed:讀不到設定檔、行程正在收尾 ——
    /// 任何一種意外都不該變成「那就連網吧」。
    public static var isEnabled: Bool {
        policy()
    }

    public struct Blocked: Error, Equatable {
        public let purpose: NetworkPurpose
        public let label: String
    }

    // MARK: - 結果型別

    public enum Result<T> {
        case ok(T)
        /// - Parameter blocked: true = 被開關擋下來的,**不是**網路失敗。
        ///   UI 必須分得出來:「連不上伺服器」要顯示重試,
        ///   「開關是關的」要顯示一顆開啟開關的按鈕。
        case err(message: String, blocked: Bool)

        public var value: T? { if case .ok(let v) = self { return v }; return nil }
        public var isBlocked: Bool { if case .err(_, let b) = self { return b }; return false }
        public var errorMessage: String? {
            if case .err(let m, _) = self { return m }; return nil
        }
    }

    public struct Downloaded: Equatable, Sendable {
        public let url: URL
        public let sha256: String
        public let bytes: Int64
    }

    // MARK: - 公開 API

    /// 取一份文字(索引 index.json)。
    public static func fetchText(_ urlString: String, purpose: NetworkPurpose,
                                 label: String = "",
                                 maxBytes: Int64 = maxTextBytes) -> Result<String> {
        // 開關關閉時連暫存檔都不要建 —— 什麼都不做才是「離線」。
        if let blocked = blockedOrNil(purpose, label) as Result<String>? { return blocked }
        let tmp = FileManager.default.temporaryDirectory
            .appendingPathComponent("rime-net-\(UUID().uuidString).txt")
        defer { try? FileManager.default.removeItem(at: tmp) }
        switch download(urlString, to: tmp, maxBytes: maxBytes, purpose: purpose, label: label) {
        case .err(let m, let b): return .err(message: m, blocked: b)
        case .ok:
            guard let s = try? String(contentsOf: tmp, encoding: .utf8) else {
                return .err(message: "回應不是合法的 UTF-8 文字", blocked: false)
            }
            return .ok(s)
        }
    }

    /// 下載到 `dest` 並回報 sha256。
    ///
    /// **呼叫端負責比對** —— 「該不該接受」是政策問題,不是傳輸問題。
    ///
    /// sha256 邊下載邊算:摘要在複製迴圈裡更新,下載結束的同時就知道結果。
    /// 不符時檔案立刻刪掉,連 ArchiveGuard 都不會看到它
    /// (docs/schema-store.md §1:「不符即整包丟棄,**不可先解壓再說**」)。
    ///
    /// `maxBytes` 是傳輸層的硬牆,與索引宣告的 `size` 無關:
    /// 索引宣告的大小也是遠端給的,若遠端送來一個無窮長的回應,
    /// 光是「下載完再驗 sha256」就足以塞爆使用者的磁碟。
    public static func download(_ urlString: String, to dest: URL, maxBytes: Int64,
                                purpose: NetworkPurpose, label: String = "",
                                onProgress: ((Int64, Int64) -> Void)? = nil) -> Result<Downloaded> {
        if let blocked = blockedOrNil(purpose, label) as Result<Downloaded>? { return blocked }

        guard let url = URL(string: urlString), let scheme = url.scheme?.lowercased() else {
            return .err(message: "網址無效:\(urlString)", blocked: false)
        }
        guard scheme == "http" || scheme == "https" else {
            // 還沒連線,所以不記錄 —— 紀錄裡只該有真的發生過的連線。
            return .err(message: "只支援 http/https,收到 \(scheme)", blocked: false)
        }

        let sink = ByteSink(dest: dest, maxBytes: maxBytes, onProgress: onProgress)
        let delegate = GateDelegate(purpose: purpose, label: label, sink: sink)

        let config = URLSessionConfiguration.ephemeral
        config.timeoutIntervalForRequest = connectTimeout
        config.timeoutIntervalForResource = resourceTimeout
        // 快取一份索引在磁碟上等於留下「你查過什麼」的痕跡,而它省不了多少事。
        config.urlCache = nil
        config.requestCachePolicy = .reloadIgnoringLocalAndRemoteCacheData
        config.httpCookieStorage = nil
        config.httpShouldSetCookies = false
        config.httpAdditionalHeaders = [
            "User-Agent": userAgent,
            // identity:讓位元組計數與大小上限有意義。
            "Accept-Encoding": "identity",
        ]

        let session = URLSession(configuration: config, delegate: delegate, delegateQueue: nil)
        defer { session.finishTasksAndInvalidate() }

        var request = URLRequest(url: url)
        request.httpMethod = "GET"

        let sem = DispatchSemaphore(value: 0)
        delegate.onFinish = { sem.signal() }
        let task = session.dataTask(with: request)
        delegate.task = task
        task.resume()
        sem.wait()

        if let failure = delegate.failure {
            sink.discard()
            return .err(message: failure, blocked: delegate.failedBecauseBlocked)
        }
        guard let done = sink.finish() else {
            record(url.host ?? "?", purpose, label, .failed, 0, "寫入失敗")
            return .err(message: "無法寫入下載的檔案", blocked: false)
        }
        record(url.host ?? "?", purpose, label, .ok, done.bytes, "")
        return .ok(done)
    }

    // MARK: - 不連網的小工具

    /// 把套件的 `file` 欄位解析成完整 URL。**純字串運算,不連線**,
    /// 但仍然放在本檔:`URL(string:` 這個 token 只要出現在第二個檔案,
    /// 檔頭那條 grep 就失去了「一眼看完」的價值。
    ///
    /// 三段回落(規範 §1 說 `base_url` 是絕對網址,但索引也可能被鏡像):
    ///   1. `file` 本身就是完整 URL → 直接用
    ///   2. 有 `base_url` → 以它為基底
    ///   3. 都沒有 → 以**索引檔自己的位置**為基底
    public static func resolveURL(indexURL: String, baseURL: String?, file: String) -> String {
        if file.hasPrefix("http://") || file.hasPrefix("https://") { return file }
        let index = URL(string: indexURL)
        if let base = baseURL, !base.trimmingCharacters(in: .whitespaces).isEmpty {
            let withSlash = base.hasSuffix("/") ? base : base + "/"
            if let baseURL = URL(string: withSlash, relativeTo: index),
               let full = URL(string: file, relativeTo: baseURL) {
                return full.absoluteURL.absoluteString
            }
        }
        if let index, let full = URL(string: file, relativeTo: index) {
            return full.absoluteURL.absoluteString
        }
        return file
    }

    /// 給紀錄與 UI 用的主機名。解不開時回原字串的前 60 字,不回 nil。
    public static func hostOf(_ urlString: String) -> String {
        URL(string: urlString)?.host ?? String(urlString.prefix(60))
    }

    // MARK: - 內部

    private static func blockedOrNil<T>(_ purpose: NetworkPurpose, _ label: String) -> Result<T>? {
        guard !isEnabled else { return nil }
        // 刻意**不**記進連網紀錄:被擋下來的嘗試不是一次連網。
        // 記進去的話,「開關從沒開過 → 紀錄是空的」這句話就不成立了,
        // 而那句話正是使用者驗證我們的方式。
        return .err(message: "連網開關是關閉的", blocked: true)
    }

    static func record(_ host: String, _ purpose: NetworkPurpose, _ label: String,
                       _ outcome: NetworkOutcome, _ bytes: Int64, _ detail: String) {
        recorder?(NetworkLogEntry(host: host, purpose: purpose, label: label,
                                  outcome: outcome, bytes: bytes, detail: detail))
    }
}

// MARK: - 位元組水槽

/// 邊寫檔、邊算 sha256、邊數位元組。
final class ByteSink {
    private let dest: URL
    private let maxBytes: Int64
    private let onProgress: ((Int64, Int64) -> Void)?
    private var handle: FileHandle?
    private var hasher = SHA256()
    private(set) var written: Int64 = 0
    private(set) var expected: Int64 = -1
    private(set) var overflowed = false

    init(dest: URL, maxBytes: Int64, onProgress: ((Int64, Int64) -> Void)?) {
        self.dest = dest
        self.maxBytes = maxBytes
        self.onProgress = onProgress
    }

    func declare(total: Int64) -> Bool {
        expected = total
        return !(total > 0 && total > maxBytes)
    }

    /// 回傳 false = 超過上限,呼叫端必須中止。
    func consume(_ data: Data) -> Bool {
        if handle == nil {
            let fm = FileManager.default
            try? fm.createDirectory(at: dest.deletingLastPathComponent(),
                                    withIntermediateDirectories: true)
            fm.createFile(atPath: dest.path, contents: nil)
            handle = try? FileHandle(forWritingTo: dest)
        }
        written += Int64(data.count)
        if written > maxBytes { overflowed = true; return false }
        hasher.update(data: data)
        try? handle?.write(contentsOf: data)
        onProgress?(written, expected)
        return true
    }

    func finish() -> NetworkGate.Downloaded? {
        try? handle?.close()
        handle = nil
        let digest = hasher.finalize().map { String(format: "%02x", $0) }.joined()
        guard FileManager.default.fileExists(atPath: dest.path) else { return nil }
        return NetworkGate.Downloaded(url: dest, sha256: digest, bytes: written)
    }

    func discard() {
        try? handle?.close()
        handle = nil
        try? FileManager.default.removeItem(at: dest)
    }
}

// MARK: - URLSession 委派

final class GateDelegate: NSObject, URLSessionDataDelegate {

    private let purpose: NetworkPurpose
    private let label: String
    private let sink: ByteSink
    private var redirects = 0

    var task: URLSessionTask?
    var onFinish: (() -> Void)?
    private(set) var failure: String?
    private(set) var failedBecauseBlocked = false

    init(purpose: NetworkPurpose, label: String, sink: ByteSink) {
        self.purpose = purpose
        self.label = label
        self.sink = sink
    }

    private func fail(_ message: String, blocked: Bool = false) {
        if failure == nil { failure = message; failedBecauseBlocked = blocked }
        task?.cancel()
    }

    func urlSession(_ session: URLSession, dataTask: URLSessionDataTask,
                    didReceive response: URLResponse,
                    completionHandler: @escaping (URLSession.ResponseDisposition) -> Void) {
        let host = response.url?.host ?? "?"
        guard let http = response as? HTTPURLResponse else {
            NetworkGate.record(host, purpose, label, .failed, 0, "不是 HTTP 回應")
            fail("不是 HTTP 回應")
            completionHandler(.cancel); return
        }
        guard http.statusCode == 200 else {
            NetworkGate.record(host, purpose, label, .failed, 0, "HTTP \(http.statusCode)")
            fail("HTTP \(http.statusCode)")
            completionHandler(.cancel); return
        }
        guard sink.declare(total: response.expectedContentLength) else {
            NetworkGate.record(host, purpose, label, .failed, 0, "宣告大小超過上限")
            fail("回應宣告的大小超過上限")
            completionHandler(.cancel); return
        }
        completionHandler(.allow)
    }

    func urlSession(_ session: URLSession, dataTask: URLSessionDataTask, didReceive data: Data) {
        // 每一塊都重新問一次開關:使用者可能在下載途中把它關掉,
        // 那一下必須真的中斷,而不是等這一輪跑完。
        guard NetworkGate.isEnabled else {
            fail("連網開關在下載途中被關閉,已中止", blocked: true)
            return
        }
        if !sink.consume(data) {
            let host = dataTask.originalRequest?.url?.host ?? "?"
            NetworkGate.record(host, purpose, label, .failed, sink.written, "超過大小上限,已中止")
            fail("下載量超過上限,已中止")
        }
    }

    /// 轉址。**每一跳都各記一筆** —— 下一跳可能是完全不同的主機,
    /// 不記的話紀錄就會漏掉使用者最想知道的那一件事。
    func urlSession(_ session: URLSession, task: URLSessionTask,
                    willPerformHTTPRedirection response: HTTPURLResponse,
                    newRequest request: URLRequest,
                    completionHandler: @escaping (URLRequest?) -> Void) {
        let from = response.url?.host ?? "?"
        let to = request.url?.host ?? "?"
        redirects += 1
        guard redirects <= NetworkGate.maxRedirects else {
            NetworkGate.record(from, purpose, label, .failed, 0, "轉址過多")
            fail("轉址次數過多")
            completionHandler(nil); return
        }
        guard NetworkGate.isEnabled else {
            fail("連網開關在轉址途中被關閉,已中止", blocked: true)
            completionHandler(nil); return
        }
        let scheme = request.url?.scheme?.lowercased() ?? ""
        guard scheme == "http" || scheme == "https" else {
            NetworkGate.record(from, purpose, label, .failed, 0, "轉址到不支援的協定")
            fail("轉址到不支援的協定:\(scheme)")
            completionHandler(nil); return
        }
        NetworkGate.record(from, purpose, label, .redirected, 0,
                           "HTTP \(response.statusCode) → \(to)")
        completionHandler(request)
    }

    func urlSession(_ session: URLSession, task: URLSessionTask,
                    didCompleteWithError error: Error?) {
        if let error, failure == nil {
            let host = task.originalRequest?.url?.host ?? "?"
            let short = error.localizedDescription
                .components(separatedBy: "\n").first.map { String($0.prefix(120)) } ?? "?"
            NetworkGate.record(host, purpose, label, .failed, 0, short)
            failure = "下載失敗:\(short)"
        }
        onFinish?()
    }
}
