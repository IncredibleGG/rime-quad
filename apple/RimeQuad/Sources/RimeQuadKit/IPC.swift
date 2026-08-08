//
//  IPC.swift — 設定介面 ⇄ 輸入法本體
//
//  ── 為什麼要有跨行程通訊 ────────────────────────────────────────────────
//  設定視窗**不能**跟輸入法跑在同一個行程裡:輸入法的 Info.plist 是
//  `LSBackgroundOnly`,那種行程照定義不能被帶到前景,一個有文字輸入框的
//  設定視窗在裡面拿不到鍵盤焦點。所以設定是另一個 .app(見 apple/README.md §6)。
//
//  分開之後就有一件事必須送過去:**部署**。理由不是懶,是正確性 ——
//  librime 的使用者目錄同一時間只該有一個行程在寫。兩邊各自 `rs_init()`
//  然後同時部署,壞掉的是使用者的詞庫,而且沒有任何錯誤訊息。
//
//  所以規則是:**只有輸入法本體碰 librime。** 設定介面負責改檔案
//  (那是純檔案操作,沒有併發問題),改完請對方部署,對方回報進度與結果。
//
//  ⚠ 本檔只有**編碼與狀態機**,是純邏輯、有測試。真的送出通知的那幾行在
//  AppSources / SettingsSources,各自只有幾行。
//

import Foundation

public enum IPCVerb: String, Sendable, CaseIterable {
    /// 重新部署。回覆會有進度。
    case deploy
    /// 切到某個方案。
    case selectSchema
    /// 重讀 settings.json。
    case reloadSettings
    /// 你在嗎。
    case ping
}

public enum IPCReplyKind: String, Sendable {
    case progress, ok, fail, pong
}

public struct IPCRequest: Equatable, Sendable {
    public let id: String
    public let verb: IPCVerb
    public let arg: String

    public init(id: String = UUID().uuidString, verb: IPCVerb, arg: String = "") {
        self.id = id
        self.verb = verb
        self.arg = arg
    }
}

public struct IPCReply: Equatable, Sendable {
    public let id: String
    public let kind: IPCReplyKind
    /// 進度時是經過的毫秒數,結束時是總耗時。
    public let elapsedMs: Int
    /// 給使用者看的一句話。已經是當地語言了 —— 送出的那一端才知道語言設定。
    public let text: String
    public let details: [String]

    public init(id: String, kind: IPCReplyKind, elapsedMs: Int = 0,
                text: String = "", details: [String] = []) {
        self.id = id
        self.kind = kind
        self.elapsedMs = elapsedMs
        self.text = text
        self.details = details
    }
}

public enum IPC {

    /// DistributedNotificationCenter 的通知名。
    public static let requestName = "org.rimequad.ipc.request"
    public static let replyName = "org.rimequad.ipc.reply"

    /// 輸入法本體多久沒回應就當它不在。
    ///
    /// 這個值不是部署的上限(部署可能跑好幾分鐘),而是**「有沒有人接電話」**
    /// 的上限:第一則進度回覆必須在這之內出現。之後只要進度還在流動就不算逾時。
    public static let handshakeTimeout: TimeInterval = 4

    /// 進度停止流動多久算它死了。部署本身可以很久,但它每 200ms 就該回一次。
    public static let stallTimeout: TimeInterval = 90

    // MARK: - 編碼

    /// userInfo 只用 `[String: String]`。
    ///
    /// DistributedNotificationCenter 會把 userInfo 序列化成 property list 送過去,
    /// 放進自訂型別會在對面變成 nil,而且**不會有錯誤** —— 只是訊息永遠不到。
    public static func encode(_ r: IPCRequest) -> [String: String] {
        ["id": r.id, "verb": r.verb.rawValue, "arg": r.arg]
    }

    public static func decodeRequest(_ d: [String: String]?) -> IPCRequest? {
        guard let d, let id = d["id"], !id.isEmpty,
              let raw = d["verb"], let verb = IPCVerb(rawValue: raw) else { return nil }
        return IPCRequest(id: id, verb: verb, arg: d["arg"] ?? "")
    }

    public static func encode(_ r: IPCReply) -> [String: String] {
        [
            "id": r.id,
            "kind": r.kind.rawValue,
            "elapsed": String(r.elapsedMs),
            "text": r.text,
            // 細節可能有好幾行。用 \u{1} 串起來 —— 它不可能出現在人寫的訊息裡。
            "details": r.details.joined(separator: "\u{1}"),
        ]
    }

    public static func decodeReply(_ d: [String: String]?) -> IPCReply? {
        guard let d, let id = d["id"], !id.isEmpty,
              let raw = d["kind"], let kind = IPCReplyKind(rawValue: raw) else { return nil }
        let details = (d["details"] ?? "").isEmpty
            ? []
            : (d["details"] ?? "").components(separatedBy: "\u{1}")
        return IPCReply(id: id, kind: kind,
                        elapsedMs: Int(d["elapsed"] ?? "") ?? 0,
                        text: d["text"] ?? "",
                        details: details)
    }
}

// MARK: - 等待狀態機

/// 「送出去之後到底發生什麼」的純邏輯。
///
/// 分成 handshake 與 stall 兩段逾時,是因為兩者的意思完全不同,而使用者
/// 需要的下一步也不同:
///   · 沒接上 → 「請先在系統設定把 RimeQuad 選成輸入來源」
///   · 接上了但卡住 → 「請重新啟動輸入法」
/// 用同一個「逾時」訊息會讓第一種情況的使用者去做完全沒用的事。
public final class IPCWaiter {

    public enum State: Equatable {
        case waitingForFirstReply
        case running(elapsedMs: Int, text: String)
        case finished(IPCReply)
        case noResponder
        case stalled
    }

    public let requestId: String
    private(set) public var state: State = .waitingForFirstReply
    private var lastActivity: TimeInterval

    /// 注入時間來源,測試才不必真的等。**必須是單調時鐘** ——
    /// 使用者調整系統時間或夏令時間切換時,牆上時鐘會倒退。
    private let now: () -> TimeInterval

    public init(requestId: String, now: @escaping () -> TimeInterval) {
        self.requestId = requestId
        self.now = now
        self.lastActivity = now()
    }

    /// 收到一則回覆。回傳 true = 這一則跟我們有關。
    @discardableResult
    public func accept(_ reply: IPCReply) -> Bool {
        guard reply.id == requestId else { return false }
        // 已經結束了就不再改變狀態 —— 遲到的進度不該把結果蓋掉。
        if case .finished = state { return true }
        lastActivity = now()
        switch reply.kind {
        case .progress, .pong:
            state = .running(elapsedMs: reply.elapsedMs, text: reply.text)
        case .ok, .fail:
            state = .finished(reply)
        }
        return true
    }

    /// 定時呼叫。回傳目前狀態。
    @discardableResult
    public func tick() -> State {
        if case .finished = state { return state }
        let idle = now() - lastActivity
        switch state {
        case .waitingForFirstReply:
            if idle > IPC.handshakeTimeout { state = .noResponder }
        case .running:
            if idle > IPC.stallTimeout { state = .stalled }
        default: break
        }
        return state
    }

    public var isSettled: Bool {
        switch state {
        case .finished, .noResponder, .stalled: return true
        default: return false
        }
    }
}
