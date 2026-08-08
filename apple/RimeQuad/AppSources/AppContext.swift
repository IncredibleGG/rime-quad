//
//  AppContext.swift — 全行程共用的東西(引擎、設定、主題、候選窗、按鍵映射)
//
//  IMKit 會為每一個宿主 app 建一個 IMKInputController,但引擎與候選窗只該有一份,
//  所以它們在這裡。`rs_init()` 明文規定「行程內只可呼叫一次」。
//
//  這一輪多了兩件事:
//    · **設定**(`settings`)。設定介面是另一個行程,改完會發一則
//      DistributedNotification,這裡收到就重讀並套用。
//    · **IPC 的接聽端**。設定介面不能自己碰 librime(兩個行程同時寫使用者目錄
//      會弄壞詞庫),所以部署由這裡代勞,並回報進度。
//

import AppKit

final class AppContext {

    static let shared = AppContext()

    let panel = CandidatePanel()
    let mapper = KeyMapper(resolver: RimeEngine.Resolver())
    let themes: ThemeStore
    let settings: SettingsStore

    /// 隨附資料:`RimeQuad.app/Contents/Resources/SharedSupport`
    let sharedDataDir: URL
    /// 使用者資料。
    ///
    /// 刻意**不用** `~/Library/Rime` —— 那是 Squirrel 的目錄,
    /// 兩個輸入法共用同一份使用者詞典與 installation.yaml 會互相踩,
    /// 而使用者完全看不出是誰改的。
    let userDataDir: URL
    let themesDir: URL

    /// 目前的輸入模式(繁 / 簡 / 不知道)。由 IMKit 在切換輸入來源時告訴我們。
    var inputModeScript: ScriptVariant = .unspecified

    private var ipc: IPCResponder?

    private init() {
        let res = Bundle.main.resourceURL ?? URL(fileURLWithPath: ".")
        sharedDataDir = res.appendingPathComponent("SharedSupport")
        themesDir = res.appendingPathComponent("themes")
        let home = FileManager.default.homeDirectoryForCurrentUser
        userDataDir = home.appendingPathComponent("Library/Application Support/RimeQuad")
        themes = ThemeStore(searchPaths: [
            userDataDir.appendingPathComponent("themes"),   // §2.3 使用者目錄優先
            themesDir,                                       // 隨附
        ])
        settings = SettingsStore(url: userDataDir.appendingPathComponent(SettingsStore.fileName))
    }

    func start() {
        _ = RimeEngine.shared.start(sharedDataDir: sharedDataDir, userDataDir: userDataDir,
                                    logDir: nil) { status in
            // ⚠ 這個 closure 已經被 RimeEngine dispatch 回主執行緒了。
            DeployGate.shared.note(status)
            switch status {
            case .success: NSLog("RimeQuad: 部署完成")
            case .failure:
                // rs_last_error() 在部署失敗時是空字串(librime 不提供原因)。
                // 所以這裡只能說「失敗了」,不能假裝知道為什麼。
                NSLog("RimeQuad: 部署失敗(librime 不提供原因,請檢查方案與詞庫)")
            case .running, .idle: break
            }
        }
        RimeEngine.shared.deploy()
        ipc = IPCResponder()
        ipc?.start()
    }

    /// 把設定套到目前這個 session 上。建 session 之後、以及設定變更時各叫一次。
    func applySettings() {
        let opts = SessionOptions.resolve(settings: settings.current,
                                          inputModeScript: inputModeScript)
        for (name, value) in opts {
            RimeEngine.shared.setOption(name, value)
        }
    }

    /// 依輸入模式與設定挑方案。回傳實際切過去的 id(沒切就是 nil)。
    @discardableResult
    func applySchemaForInputMode() -> String? {
        let enabled = RimeConfigPatch.readSchemaList(userDir: userDataDir)
        let installed = SchemaCatalog.scan(userDir: userDataDir, sharedDir: sharedDataDir,
                                           languageTags: InstalledRegistry(userDir: userDataDir)
                                               .languageTags())
        let byId = Dictionary(uniqueKeysWithValues: installed.map { ($0.id, $0.entry) })
        // 以 schema_list 的順序為準:那是使用者排的切換順序。
        let entries: [SchemaEntry] = enabled.map {
            byId[$0] ?? SchemaEntry(id: $0, name: $0, languageTag: nil)
        }
        let s = settings.current
        let resolution = InputModeBinding.resolve(
            script: inputModeScript,
            enabled: entries,
            pinnedForMode: inputModeScript == .hans ? s.pinnedSchemaHans : s.pinnedSchemaHant,
            pinnedGlobal: s.pinnedSchemaId,
            followMode: s.followInputMode)
        if let id = resolution.schemaId {
            RimeEngine.shared.selectSchema(id)
        }
        // simplification 由 applySettings 一起處理 —— 兩邊都設會讓
        // 「文字」頁的明確選擇被輸入模式蓋掉。
        applySettings()
        return resolution.schemaId
    }
}

/// IMKServer 的持有者。⚠ 沒有這個強參照,連線會在 main.swift 走完之後被回收,
/// 症狀是「輸入法選得到但完全沒反應」。
enum ServerHolder {
    static var instance: AnyObject?
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_ notification: Notification) {
        AppContext.shared.start()
        DistributedNotificationCenter.default.addObserver(
            self, selector: #selector(appearanceChanged),
            name: NSNotification.Name("AppleInterfaceThemeChangedNotification"), object: nil)
        DistributedNotificationCenter.default.addObserver(
            self, selector: #selector(settingsChanged),
            name: NSNotification.Name(SettingsStore.changedNotification), object: nil)
    }

    @objc private func appearanceChanged() {
        AppContext.shared.themes.reload()
    }

    /// 設定介面改了東西。**立刻**重讀並套用 —— 使用者剛按下去,
    /// 他預期切回文字框就是新的行為,而不是「重開輸入法之後」。
    @objc private func settingsChanged() {
        guard AppContext.shared.settings.reload() else { return }
        AppContext.shared.themes.reload()
        AppContext.shared.applySettings()
    }
}

// MARK: - 部署的閘門

/// 把非同步的 `rs_deploy()` 變成「呼叫並等到真的有結果」。
///
/// ⚠ **`armed` 這個旗標是必要的,不是防禦性程式碼。** 部署回呼是行程層級的,
///   上一次部署的 `success` 會在我們登記監聽的當下就被看到。不擋住的話,
///   這一次的失敗會被讀成成功,而**回滾永遠不會發生**。
final class DeployGate: Deployer {

    static let shared = DeployGate()

    private let lock = NSLock()
    private var armed = false
    private var result: DeployStatus?

    /// 部署上限。Android 端實測:模擬器三本詞庫 7.2 秒、S24U 首次 12.5 秒。
    /// 桌面應該更快,但大詞庫的方案在真機上跑到幾十秒是常態,所以上限開大。
    static let timeout: TimeInterval = 600

    func note(_ status: DeployStatus) {
        lock.lock(); defer { lock.unlock() }
        guard armed else { return }
        if status == .success || status == .failure { result = status }
    }

    func deployAndWait(onTick: @escaping (Int) -> Void) -> DeployOutcome {
        guard RimeEngine.shared.isInitialised else {
            return .notStarted(reason: "librime 尚未初始化")
        }
        lock.lock()
        result = nil
        armed = true
        lock.unlock()
        defer {
            lock.lock(); armed = false; lock.unlock()
        }

        // 單調時鐘。使用者調整系統時間或夏令時切換時,牆上時鐘會倒退。
        let start = DispatchTime.now().uptimeNanoseconds
        func elapsedMs() -> Int {
            Int((DispatchTime.now().uptimeNanoseconds - start) / 1_000_000)
        }

        RimeEngine.shared.deploy()

        while true {
            RunLoop.current.run(mode: .default, before: Date().addingTimeInterval(0.2))
            lock.lock()
            let r = result
            lock.unlock()
            if let r {
                if r == .success { return .success(elapsedMs: elapsedMs()) }
                // 部署失敗時 rs_last_error() 是空字串(coordination.md §4)。
                return .failure(elapsedMs: elapsedMs(), lastError: "")
            }
            let ms = elapsedMs()
            onTick(ms)
            if Double(ms) / 1000 > DeployGate.timeout {
                return .timeout(elapsedMs: ms)
            }
        }
    }
}

// MARK: - IPC 的接聽端

/// 設定介面送過來的請求。**只有這個行程碰 librime。**
final class IPCResponder {

    private let center = DistributedNotificationCenter.default()
    private let queue = DispatchQueue(label: "org.rimequad.ipc")
    private var busy = false

    func start() {
        center.addObserver(forName: Notification.Name(IPC.requestName), object: nil,
                           queue: .main) { [weak self] note in
            guard let self,
                  let req = IPC.decodeRequest(note.userInfo as? [String: String]) else { return }
            self.handle(req)
        }
    }

    private func reply(_ r: IPCReply) {
        center.postNotificationName(Notification.Name(IPC.replyName), object: nil,
                                    userInfo: IPC.encode(r), deliverImmediately: true)
    }

    private func handle(_ req: IPCRequest) {
        switch req.verb {
        case .ping:
            reply(IPCReply(id: req.id, kind: .pong))

        case .reloadSettings:
            _ = AppContext.shared.settings.reload()
            AppContext.shared.applySettings()
            reply(IPCReply(id: req.id, kind: .ok))

        case .selectSchema:
            RimeEngine.shared.selectSchema(req.arg)
            AppContext.shared.applySettings()
            reply(IPCReply(id: req.id, kind: .ok, text: req.arg))

        case .deploy:
            guard !busy else {
                reply(IPCReply(id: req.id, kind: .fail,
                               text: "已經有一個重新整理在進行中,請等它完成。"))
                return
            }
            busy = true
            // 先回一則 pong:設定介面的 handshake 逾時要在 4 秒內收到東西,
            // 而部署的第一則進度可能要更久。
            reply(IPCReply(id: req.id, kind: .pong, text: "開始"))
            queue.async { [weak self] in
                guard let self else { return }
                let outcome = DeployGate.shared.deployAndWait { ms in
                    self.reply(IPCReply(id: req.id, kind: .progress, elapsedMs: ms))
                }
                DispatchQueue.main.async {
                    self.busy = false
                    // 部署之後舊 session 已經失效,重建它並重新套設定。
                    RimeEngine.shared.closeSession()
                    RimeEngine.shared.openSession()
                    AppContext.shared.applySchemaForInputMode()
                    switch outcome {
                    case .success(let ms):
                        self.reply(IPCReply(id: req.id, kind: .ok, elapsedMs: ms))
                    case .failure(let ms, let err):
                        self.reply(IPCReply(id: req.id, kind: .fail, elapsedMs: ms,
                                            text: "重新整理失敗",
                                            details: err.isEmpty
                                                ? ["librime 沒有提供失敗原因"] : [err]))
                    case .timeout(let ms):
                        self.reply(IPCReply(id: req.id, kind: .fail, elapsedMs: ms,
                                            text: "重新整理超過時間上限"))
                    case .notStarted(let reason):
                        self.reply(IPCReply(id: req.id, kind: .fail, text: "沒有辦法開始",
                                            details: [reason]))
                    }
                }
            }
        }
    }
}
