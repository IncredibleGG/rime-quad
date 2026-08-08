//
//  RimeEngine.swift — rime_shell 門面的 Swift 包裝
//
//  ── 這一層唯一的職責是「把契約變成型別」 ────────────────────────────────
//  core/include/rime_shell.h 的檔頭有三條約定，前兩條靠人記住一定會出事：
//
//   1. **每個輸入事件只 acquire 一次。** commit 在 acquire 當下就被消費掉，
//      不是 release。所以「先 acquire 看狀態、做點事、再 acquire 讀結果」
//      會遺失第一次的 commit_text。
//      → 這裡只暴露 `snapshot()`，它 acquire 一次、**立刻把所有字串複製成
//        Swift 值**、然後 release。呼叫端拿到的是純值，想看幾次看幾次。
//
//   2. **回傳的指標在下一次 acquire / release 之前才有效。**
//      → 同上，複製出來就沒有懸空指標的問題。
//
//   3. **rs_deploy_callback 不在呼叫端的執行緒上。**
//      → `deploy()` 的回呼一律 dispatch 回 main，呼叫端拿到的永遠是主執行緒。
//

import Foundation

public struct RimeCandidate {
    public let text: String
    public let comment: String
    public let label: String
}

public struct RimeSnapshot {
    public var preedit = ""
    public var selStart = 0
    public var selEnd = 0
    public var caret = 0
    public var candidates: [RimeCandidate] = []
    public var pageNo = 0
    public var highlighted = -1
    public var isLastPage = true
    public var status = EngineStatus()
    /// 本次應上屏的文字。**已經被消費掉了**，下一次 acquire 不會再有。
    public var commitText: String?

    public var compositionState: CompositionState {
        CompositionState(menuCount: candidates.count, isComposing: status.isComposing)
    }
}

public enum DeployStatus: Int32 {
    case idle = 0, running = 1, success = 2, failure = 3
}

private var deployHandler: ((DeployStatus) -> Void)?

private func deployTrampoline(status: rs_deploy_status, userdata: UnsafeMutableRawPointer?) {
    // 刻意用相等比較而不是 rawValue：C 的 anonymous enum + typedef 在不同
    // Swift 版本下會被匯入成 enum 或 RawRepresentable struct，rawValue 的型別
    // 因此不穩定，而 == 兩種都成立。
    // if/else 而不是 switch：兩種匯入形式（Swift enum / RawRepresentable struct）
    // 底下 == 都成立，switch 的 case 模式則不一定。
    let s: DeployStatus
    if status == RS_DEPLOY_RUNNING { s = .running }
    else if status == RS_DEPLOY_SUCCESS { s = .success }
    else if status == RS_DEPLOY_FAILURE { s = .failure }
    else { s = .idle }
    // ⚠ 這裡**不在**主執行緒上。碰 UI 之前一定要切回去。
    DispatchQueue.main.async { deployHandler?(s) }
}

public final class RimeEngine {

    public static let shared = RimeEngine()

    private var session: rs_session = 0
    private(set) public var isInitialised = false

    private init() {}

    /// ABI 協商。實作端與呼叫端的版本不符就拒絕載入 —— 這是唯一擋得住
    /// 「dylib 與上層不同步」的關卡。
    public static func abiMatches() -> Bool {
        rs_abi_version() == Int32(RIME_SHELL_ABI_VERSION)
    }

    @discardableResult
    public func start(sharedDataDir: URL, userDataDir: URL, logDir: URL?,
                      onDeploy: @escaping (DeployStatus) -> Void) -> Bool {
        guard !isInitialised else { return true }
        guard RimeEngine.abiMatches() else {
            NSLog("RimeQuad: ABI 不符（實作端 \(rs_abi_version())，呼叫端 \(RIME_SHELL_ABI_VERSION)）")
            return false
        }
        try? FileManager.default.createDirectory(at: userDataDir, withIntermediateDirectories: true)
        deployHandler = onDeploy

        var ok = false
        sharedDataDir.path.withCString { shared in
            userDataDir.path.withCString { user in
                let run: (UnsafePointer<CChar>?) -> Void = { log in
                    "rime.macos".withCString { app in
                        var setup = rs_setup()
                        setup.shared_data_dir = shared
                        setup.user_data_dir = user
                        setup.log_dir = log
                        setup.app_name = app
                        setup.on_deploy = deployTrampoline
                        setup.userdata = nil
                        ok = rs_init(&setup)
                    }
                }
                if let logDir {
                    logDir.path.withCString { run($0) }
                } else {
                    run(nil)
                }
            }
        }
        isInitialised = ok
        if !ok { NSLog("RimeQuad: rs_init 失敗：\(String(cString: rs_last_error()))") }
        return ok
    }

    public func deploy() { _ = rs_deploy() }

    public func openSession() {
        guard isInitialised else { return }
        if session != 0 && rs_session_alive(session) { return }
        session = rs_session_create()
    }

    public func closeSession() {
        guard session != 0 else { return }
        rs_session_destroy(session)
        session = 0
    }

    public var hasSession: Bool { session != 0 && rs_session_alive(session) }

    // ── 輸入 ────────────────────────────────────────────────

    public func process(keysym: Int32, modifiers: UInt32) -> Bool {
        guard hasSession else { return false }
        return rs_process_key(session, keysym, modifiers)
    }

    public func selectCandidate(_ index: Int32) -> Bool {
        hasSession ? rs_select_candidate(session, index) : false
    }

    public func highlightCandidate(_ index: Int32) -> Bool {
        hasSession ? rs_highlight_candidate(session, index) : false
    }

    public func changePage(backward: Bool) -> Bool {
        hasSession ? rs_change_page(session, backward) : false
    }

    public func clearComposition() -> Bool {
        hasSession ? rs_clear_composition(session) : false
    }

    public func commitComposition() -> Bool {
        hasSession ? rs_commit_composition(session) : false
    }

    public func setOption(_ name: String, _ value: Bool) {
        guard hasSession else { return }
        name.withCString { _ = rs_set_option(session, $0, value) }
    }

    public func getOption(_ name: String) -> Bool {
        guard hasSession else { return false }
        return name.withCString { rs_get_option(session, $0) }
    }

    public func selectSchema(_ id: String) {
        guard hasSession else { return }
        id.withCString { _ = rs_select_schema(session, $0) }
    }

    public func schemaList() -> [(id: String, name: String)] {
        let total = rs_schema_list(nil, nil, 0)
        guard total > 0 else { return [] }
        var ids = [UnsafePointer<CChar>?](repeating: nil, count: Int(total))
        var names = [UnsafePointer<CChar>?](repeating: nil, count: Int(total))
        let got = rs_schema_list(&ids, &names, total)
        var out: [(String, String)] = []
        for i in 0..<Int(min(got, total)) {
            guard let idp = ids[i], let np = names[i] else { continue }
            out.append((String(cString: idp), String(cString: np)))
        }
        return out
    }

    // ── 快照 ────────────────────────────────────────────────

    /// **一個輸入事件只呼叫一次。** 見檔頭。
    public func snapshot() -> RimeSnapshot? {
        guard hasSession, let raw = rs_snapshot_acquire(session) else { return nil }
        defer { rs_snapshot_release(session) }
        let s = raw.pointee

        var out = RimeSnapshot()
        out.preedit = s.composition.preedit.map { String(cString: $0) } ?? ""
        out.selStart = Int(s.composition.sel_start)
        out.selEnd = Int(s.composition.sel_end)
        out.caret = Int(s.composition.caret)

        if let items = s.menu.items, s.menu.count > 0 {
            out.candidates = (0..<Int(s.menu.count)).map { i in
                let c = items[i]
                return RimeCandidate(text: c.text.map { String(cString: $0) } ?? "",
                                     comment: c.comment.map { String(cString: $0) } ?? "",
                                     label: c.label.map { String(cString: $0) } ?? "")
            }
        }
        out.pageNo = Int(s.menu.page_no)
        out.highlighted = Int(s.menu.highlighted)
        out.isLastPage = s.menu.is_last_page

        var st = EngineStatus()
        st.schemaId = s.status.schema_id.map { String(cString: $0) } ?? ""
        st.schemaName = s.status.schema_name.map { String(cString: $0) } ?? ""
        st.isComposing = s.status.is_composing
        st.isAsciiMode = s.status.is_ascii_mode
        st.isFullShape = s.status.is_full_shape
        st.isSimplified = s.status.is_simplified
        st.isAsciiPunct = s.status.is_ascii_punct
        st.isDisabled = s.status.is_disabled
        out.status = st

        out.commitText = s.commit_text.map { String(cString: $0) }
        return out
    }

    /// keysym 名稱 → 值。純查表，不需要 rs_init()。
    public struct Resolver: KeysymResolver {
        public init() {}
        public func keysym(named: String) -> Int32 {
            named.withCString { rs_keysym_by_name($0) }
        }
    }
}
