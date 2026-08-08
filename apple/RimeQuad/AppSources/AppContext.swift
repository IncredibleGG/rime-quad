//
//  AppContext.swift — 全行程共用的東西（引擎、主題、候選窗、按鍵映射）
//
//  IMKit 會為每一個宿主 app 建一個 IMKInputController，但引擎與候選窗只該有一份，
//  所以它們在這裡。`rs_init()` 明文規定「行程內只可呼叫一次」。
//

import AppKit

final class AppContext {

    static let shared = AppContext()

    let panel = CandidatePanel()
    let mapper = KeyMapper(resolver: RimeEngine.Resolver())
    let themes: ThemeStore

    /// 隨附資料：`RimeQuad.app/Contents/Resources/SharedSupport`
    let sharedDataDir: URL
    /// 使用者資料。
    ///
    /// 刻意**不用** `~/Library/Rime` —— 那是 Squirrel 的目錄，
    /// 兩個輸入法共用同一份使用者詞典與 installation.yaml 會互相踩，
    /// 而使用者完全看不出是誰改的。
    let userDataDir: URL
    let themesDir: URL

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
    }

    func start() {
        _ = RimeEngine.shared.start(sharedDataDir: sharedDataDir, userDataDir: userDataDir,
                                    logDir: nil) { status in
            // ⚠ 這個 closure 已經被 RimeEngine dispatch 回主執行緒了。
            switch status {
            case .success: NSLog("RimeQuad: 部署完成")
            case .failure:
                // rs_last_error() 在部署失敗時是空字串（librime 不提供原因）。
                // 所以這裡只能說「失敗了」，不能假裝知道為什麼。
                NSLog("RimeQuad: 部署失敗（librime 不提供原因，請檢查方案與詞庫）")
            case .running, .idle: break
            }
        }
        RimeEngine.shared.deploy()
    }
}

/// IMKServer 的持有者。⚠ 沒有這個強參照，連線會在 main.swift 走完之後被回收，
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
    }

    @objc private func appearanceChanged() {
        AppContext.shared.themes.reload()
    }
}
