//
//  SettingsApp.swift — 設定視窗的殼與狀態
//
//  ── 這是另一個 .app ────────────────────────────────────────────────────
//  同一份執行檔,兩個 bundle:輸入法本體(`LSBackgroundOnly`,不能到前景)
//  與設定介面(一般 app,拿得到鍵盤焦點)。`main.swift` 依 bundle id 分岔。
//  詳見 apple/README.md §6 與 IPC.swift 的檔頭。
//

import AppKit

// MARK: - 路徑

enum SettingsPaths {
    static var userDir: URL {
        FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Application Support/RimeQuad")
    }
    /// 設定 app 在 `RimeQuad.app/Contents/Resources/RimeQuadSettings.app`,
    /// 所以往上四層就是輸入法的 bundle。
    static var hostAppURL: URL? {
        let me = Bundle.main.bundleURL
        let host = me.deletingLastPathComponent()   // Resources
            .deletingLastPathComponent()            // Contents
            .deletingLastPathComponent()            // RimeQuad.app
        return host.pathExtension == "app" ? host : nil
    }
    static var sharedDir: URL {
        (hostAppURL?.appendingPathComponent("Contents/Resources/SharedSupport"))
            ?? userDir.appendingPathComponent("SharedSupport")
    }
    static var themesDir: URL {
        (hostAppURL?.appendingPathComponent("Contents/Resources/themes"))
            ?? userDir.appendingPathComponent("themes")
    }
    static var workDir: URL {
        FileManager.default.temporaryDirectory.appendingPathComponent("RimeQuadStore")
    }
}

// MARK: - 透過 IPC 部署

/// 把部署請求送給輸入法本體,阻塞等它回覆。
///
/// 找不到對方時**不假裝成功**,也不自己去 `rs_init()` —— 兩個行程同時寫
/// 同一個使用者目錄,壞掉的是使用者的詞庫(見 IPC.swift 檔頭)。
final class RemoteDeployer: Deployer {

    private let center = DistributedNotificationCenter.default()

    func deployAndWait(onTick: @escaping (Int) -> Void) -> DeployOutcome {
        let request = IPCRequest(verb: .deploy)
        let waiter = IPCWaiter(requestId: request.id) {
            Double(DispatchTime.now().uptimeNanoseconds) / 1_000_000_000
        }
        let lock = NSLock()

        let token = center.addObserver(forName: Notification.Name(IPC.replyName),
                                       object: nil, queue: nil) { note in
            guard let reply = IPC.decodeReply(note.userInfo as? [String: String]) else { return }
            lock.lock(); defer { lock.unlock() }
            waiter.accept(reply)
        }
        defer { center.removeObserver(token) }

        // 對方可能還沒啟動(使用者還沒把 RimeQuad 選成輸入來源)。叫醒它。
        if let host = SettingsPaths.hostAppURL {
            let cfg = NSWorkspace.OpenConfiguration()
            cfg.activates = false
            NSWorkspace.shared.openApplication(at: host, configuration: cfg)
        }

        center.postNotificationName(Notification.Name(IPC.requestName), object: nil,
                                    userInfo: IPC.encode(request),
                                    deliverImmediately: true)

        while true {
            RunLoop.current.run(mode: .default, before: Date().addingTimeInterval(0.1))
            lock.lock()
            let state = waiter.tick()
            lock.unlock()
            switch state {
            case .running(let ms, _): onTick(ms)
            case .finished(let reply):
                if reply.kind == .ok { return .success(elapsedMs: reply.elapsedMs) }
                return .failure(elapsedMs: reply.elapsedMs,
                                lastError: ([reply.text] + reply.details).joined(separator: " / "))
            case .noResponder:
                return .notStarted(reason:
                    "輸入法本體沒有回應。請先到「系統設定 › 鍵盤 › 輸入來源」把 RimeQuad 加進去並選用它,再試一次。")
            case .stalled:
                return .timeout(elapsedMs: 0)
            case .waitingForFirstReply: break
            }
        }
    }
}

// MARK: - 狀態

/// 設定視窗的所有可變狀態。每一次變更都會叫 `onChange`,由視窗重畫。
final class SettingsModel {

    let store: SettingsStore
    let engine: StoreEngine
    let logFile: NetworkLogFile
    private(set) var lang: UiLanguage

    /// 使用者在方案頁勾選但還沒按「套用」的狀態。
    /// 每勾一次就部署一次的話,勾三個方案要等三次十幾秒。
    var pendingSchemaList: [String]?

    var index: SchemaIndex?
    var indexError: String?
    var indexWarnings: [String] = []
    var loadingIndex = false

    var phrases: [UserPhrase] = []

    var onChange: (() -> Void)?
    var onBusy: ((String, String, Double) -> Void)?
    var onIdle: (() -> Void)?
    var onResult: ((StoreOutcome) -> Void)?

    private let queue = DispatchQueue(label: "org.rimequad.settings.work")
    private(set) var busy = false

    init() {
        let userDir = SettingsPaths.userDir
        store = SettingsStore(url: userDir.appendingPathComponent(SettingsStore.fileName))
        engine = StoreEngine(userDir: userDir, sharedDir: SettingsPaths.sharedDir,
                             workDir: SettingsPaths.workDir, deployer: RemoteDeployer())
        logFile = NetworkLogFile(directory: userDir)
        lang = L10n.resolve(store.current.uiLanguage)

        // ⚠ 連網政策接在這裡,而且只接這一處。NetworkGate 預設拒絕,
        //    這一行沒跑到的話行為是**完全離線**,不是完全開放。
        NetworkGate.policy = { [weak self] in self?.store.current.networkEnabled ?? false }
        NetworkGate.recorder = { [weak self] entry in self?.logFile.append(entry) }

        phrases = UserPhrases.read(userDir: userDir).phrases
    }

    var settings: RimeQuadSettings { store.current }

    func edit(_ mutate: (inout RimeQuadSettings) -> Void) {
        guard store.update(mutate) else { return }
        lang = L10n.resolve(store.current.uiLanguage)
        // 輸入法本體要立刻知道。它在另一個行程,所以走通知。
        DistributedNotificationCenter.default().postNotificationName(
            Notification.Name(SettingsStore.changedNotification), object: nil,
            userInfo: nil, deliverImmediately: true)
        onChange?()
    }

    // MARK: - 方案

    var installedSchemas: [InstalledSchema] {
        SchemaCatalog.scan(userDir: SettingsPaths.userDir, sharedDir: SettingsPaths.sharedDir,
                           languageTags: engine.registry.languageTags())
    }

    var schemaRows: [SchemaCatalog.Row] {
        SchemaCatalog.rows(installed: installedSchemas,
                           enabled: pendingSchemaList ?? engine.enabledSchemas)
    }

    var hasPendingSchemaChanges: Bool {
        guard let pending = pendingSchemaList else { return false }
        return pending != engine.enabledSchemas
    }

    // MARK: - 背景作業

    /// 所有會花時間的事情都走這裡:一次只准一件,而且一定有覆蓋層。
    func run(_ title: T, _ work: @escaping (@escaping (StoreProgress) -> Void) -> StoreOutcome) {
        guard !busy else { return }
        busy = true
        let lang = self.lang
        onBusy?(title[lang], "", -1)
        queue.async { [weak self] in
            let outcome = work { progress in
                guard let self else { return }
                let (t, d, f) = SettingsModel.describe(progress, lang: lang)
                DispatchQueue.main.async { self.onBusy?(t, d, f) }
            }
            DispatchQueue.main.async {
                guard let self else { return }
                self.busy = false
                self.pendingSchemaList = nil
                self.onIdle?()
                self.onResult?(outcome)
                self.onChange?()
            }
        }
    }

    static func describe(_ p: StoreProgress, lang: UiLanguage) -> (String, String, Double) {
        switch p {
        case .downloading(let name, let i, let total, let read, let bytes):
            return (T("下載中(\(i)/\(total))", "下载中(\(i)/\(total))",
                      "Downloading (\(i)/\(total))")[lang],
                    bytes > 0 ? "\(name) — \(formatBytes(read)) / \(formatBytes(bytes))"
                              : "\(name) — \(formatBytes(read))",
                    p.fraction)
        case .verifying(let name):
            return (T("檢查內容", "检查内容", "Verifying")[lang], name, -1)
        case .extracting(let name):
            return (T("解壓縮", "解压缩", "Extracting")[lang], name, -1)
        case .preflight:
            return (T("檢查相依檔案", "检查依赖文件", "Checking dependencies")[lang], "", -1)
        case .deploying(let ms):
            return (T("重新整理字詞", "重新整理字词", "Rebuilding dictionaries")[lang],
                    T("已經 \(ms / 1000) 秒 —— 第一次會比較久",
                      "已经 \(ms / 1000) 秒 —— 第一次会比较久",
                      "\(ms / 1000)s elapsed — the first run takes longer")[lang], -1)
        case .rollingBack(let reason):
            return (T("正在回復原本的設定", "正在恢复原本的设置", "Restoring your settings")[lang],
                    reason, -1)
        }
    }

    // MARK: - 市集

    var indexURL: String {
        settings.storeIndexUrl ?? SettingsModel.defaultIndexURL
    }

    /// 內建的索引位址。R2 目前是**測試階段**的位置,日後要改走
    /// GitHub Releases(見專案的長期記事)。改這裡不需要動格式。
    static let defaultIndexURL = "https://pub-rimequad.r2.dev/rime/schemas/index.json"

    func loadIndex() {
        guard !loadingIndex else { return }
        // 開關關著時什麼都不做:使用者沒有做錯任何事,畫面上該看到的是
        // 說明卡而不是一個轉圈與一段紅字。
        guard NetworkGate.isEnabled else {
            index = nil; indexError = nil; onChange?(); return
        }
        loadingIndex = true
        onChange?()
        let url = indexURL
        queue.async { [weak self] in
            let result = NetworkGate.fetchText(url, purpose: .storeIndex)
            DispatchQueue.main.async {
                guard let self else { return }
                self.loadingIndex = false
                switch result {
                case .err(let m, let blocked):
                    self.index = nil
                    self.indexError = blocked ? nil : m
                case .ok(let text):
                    switch IndexParser.parse(text) {
                    case .err(let m): self.index = nil; self.indexError = m
                    case .ok(let idx, let warnings):
                        self.index = idx; self.indexWarnings = warnings; self.indexError = nil
                    }
                }
                self.onChange?()
            }
        }
    }
}

// MARK: - App

/// 輸入法本體要求設定視窗做一件事(開在某一頁、順便重新整理)。
///
/// 為什麼不是只用命令列參數:`NSWorkspace.openApplication` 對**已經在跑**的
/// app 不會重新送一次 argv,所以第二次點「重新整理字詞…」會什麼都不做 ——
/// 又是一顆按了沒反應的選單項。所以冷啟動讀 argv,熱啟動走通知,兩條都接。
enum SettingsCommand {
    static let notification = "org.rimequad.settings.command"

    static func parse(_ args: [String]) -> (page: String?, redeploy: Bool) {
        var page: String?
        var redeploy = false
        for a in args {
            if a.hasPrefix("--page=") { page = String(a.dropFirst("--page=".count)) }
            if a == "--redeploy" { redeploy = true }
        }
        return (page, redeploy)
    }
}

final class SettingsAppDelegate: NSObject, NSApplicationDelegate {

    private var window: SettingsWindowController?

    func applicationDidFinishLaunching(_ note: Notification) {
        NSApp.setActivationPolicy(.regular)
        let command = SettingsCommand.parse(CommandLine.arguments)
        let wc = SettingsWindowController()
        window = wc
        wc.showWindow(nil)
        NSApp.activate(ignoringOtherApps: true)
        wc.apply(command: command)

        DistributedNotificationCenter.default().addObserver(
            forName: Notification.Name(SettingsCommand.notification), object: nil,
            queue: .main) { [weak self] note in
                let info = note.userInfo as? [String: String] ?? [:]
                let page = info["page"]
                let redeploy = info["redeploy"] == "1"
                self?.window?.showWindow(nil)
                NSApp.activate(ignoringOtherApps: true)
                self?.window?.apply(command: (page: page, redeploy: redeploy))
            }
    }

    /// 使用者從選單列再點一次「設定」時把視窗帶回來,而不是什麼都不做。
    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows: Bool) -> Bool {
        window?.showWindow(nil)
        NSApp.activate(ignoringOtherApps: true)
        return true
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ app: NSApplication) -> Bool { true }
}

enum SettingsAppMain {
    static func run() -> Never {
        let app = NSApplication.shared
        let delegate = SettingsAppDelegate()
        app.delegate = delegate
        // 有選單列才有 ⌘Q / ⌘W / 複製貼上。少了它,使用者在詞庫頁
        // 連貼上都做不到,而那不會有任何錯誤訊息。
        app.mainMenu = SettingsMenu.build()
        app.run()
        exit(0)
    }
}

enum SettingsMenu {
    static func build() -> NSMenu {
        let main = NSMenu()

        let appItem = NSMenuItem()
        let appMenu = NSMenu()
        appMenu.addItem(withTitle: "About RimeQuad", action: nil, keyEquivalent: "")
        appMenu.addItem(.separator())
        appMenu.addItem(withTitle: "Hide RimeQuad Settings",
                        action: #selector(NSApplication.hide(_:)), keyEquivalent: "h")
        appMenu.addItem(withTitle: "Quit RimeQuad Settings",
                        action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        appItem.submenu = appMenu
        main.addItem(appItem)

        let editItem = NSMenuItem()
        let edit = NSMenu(title: "Edit")
        edit.addItem(withTitle: "Undo", action: Selector(("undo:")), keyEquivalent: "z")
        edit.addItem(withTitle: "Redo", action: Selector(("redo:")), keyEquivalent: "Z")
        edit.addItem(.separator())
        edit.addItem(withTitle: "Cut", action: #selector(NSText.cut(_:)), keyEquivalent: "x")
        edit.addItem(withTitle: "Copy", action: #selector(NSText.copy(_:)), keyEquivalent: "c")
        edit.addItem(withTitle: "Paste", action: #selector(NSText.paste(_:)), keyEquivalent: "v")
        edit.addItem(withTitle: "Select All", action: #selector(NSText.selectAll(_:)),
                     keyEquivalent: "a")
        editItem.submenu = edit
        main.addItem(editItem)

        let windowItem = NSMenuItem()
        let win = NSMenu(title: "Window")
        win.addItem(withTitle: "Close", action: #selector(NSWindow.performClose(_:)),
                    keyEquivalent: "w")
        win.addItem(withTitle: "Minimise", action: #selector(NSWindow.performMiniaturize(_:)),
                    keyEquivalent: "m")
        windowItem.submenu = win
        main.addItem(windowItem)

        return main
    }
}
