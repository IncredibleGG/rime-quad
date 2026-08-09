//
//  LuminaKeyInputController.swift — IMKInputController
//
//  ⚠ 類別名稱 `LuminaKeyInputController` 同時出現在三個地方,改一個就要改三個:
//     · 這裡的 @objc(...) —— Swift 的類別名會被 mangle 成
//       `_TtC9LuminaKey24LuminaKeyInputController`,IMKit 用 NSClassFromString
//       找它,**不加 @objc 就一定找不到**(症狀是輸入法裝得起來、選得到、
//       但一個字都打不出來,而且完全沒有錯誤訊息)。
//     · Info.plist 的 InputMethodServerControllerClass。
//     · apple/scripts/verify_app_bundle.sh 的斷言。
//

import AppKit
import InputMethodKit

@objc(LuminaKeyInputController)
final class LuminaKeyInputController: IMKInputController {

    private let engine = RimeEngine.shared
    private var mapper: KeyMapper { AppContext.shared.mapper }
    private var tracker = ModifierTracker()

    // ───────────────────────── 生命週期 ─────────────────────────

    override func activateServer(_ sender: Any!) {
        super.activateServer(sender)
        tracker.reset()
        engine.openSession()
        AppContext.shared.panel.onSelect = { [weak self] idx in
            self?.selectCandidate(index: idx, client: sender)
        }
        AppContext.shared.themes.reload()
        // 使用者可能在別的 app 裡改了設定,或剛剛才在系統設定裡換了輸入來源。
        _ = AppContext.shared.settings.reload()
        readInputMode(client: sender)
        AppContext.shared.applySchemaForInputMode()
    }

    override func deactivateServer(_ sender: Any!) {
        // 切走時把組字結束掉。留著會在下一個 app 裡冒出半截 preedit。
        commitComposition(sender)
        AppContext.shared.panel.hide()
        super.deactivateServer(sender)
    }

    override func commitComposition(_ sender: Any!) {
        guard engine.hasSession else { return }
        if engine.commitComposition() {
            // commit 之後必須 acquire 才拿得到文字(見 rime_shell.h)。
            if let snap = engine.snapshot() { deliver(snap, to: sender) }
        }
        AppContext.shared.panel.hide()
    }

    // ───────────────────────── 輸入模式 ─────────────────────────
    //
    // ⚠ 這一段修的是真機回報的缺陷:使用者選了 `…LuminaKey.Hans`(簡體),
    //   打 `hao le` 卻得到「號」這個繁體字 —— 輸入模式與方案沒有綁在一起。
    //   判定規則是純邏輯,在 LuminaKeyKit/InputModeBinding.swift,有單元測試。

    /// IMKit 在使用者切換輸入模式時呼叫這裡。
    ///
    /// **不比對 tag 的數值。** `kTextServiceInputModePropertyTag` 在不同的
    /// SDK 匯入形式下型別不穩定,而拿錯 tag 的症狀是「什麼都沒發生」——
    /// 完全沒有錯誤訊息的那一類。所以改成:任何字串型的值都餵給
    /// `InputModeBinding.script(forInputSourceID:)`,認不出來它會回
    /// `.unspecified`,那時我們什麼都不做。
    override func setValue(_ value: Any!, forTag tag: Int, client sender: Any!) {
        if let mode = value as? String {
            let script = InputModeBinding.script(forInputSourceID: mode)
            if script != .unspecified, script != AppContext.shared.inputModeScript {
                AppContext.shared.inputModeScript = script
                AppContext.shared.applySchemaForInputMode()
            }
        }
        super.setValue(value, forTag: tag, client: sender)
    }

    /// `kTextServiceInputModePropertyTag`(四字元碼 `'imim'`)。
    ///
    /// 刻意寫成字面值而不是引用 Carbon 的那個符號:它在不同 SDK 下的
    /// Swift 匯入型別不穩定(有時是 Int、有時是 Int32、有時根本沒匯入),
    /// 而**拿錯 tag 的後果只是問不到值**,不是壞掉 —— 那時仍然有
    /// `setValue(_:forTag:client:)` 這條路。用字面值換掉一個編譯期的風險。
    private static let inputModeTag = 0x696D_696D

    private func readInputMode(client sender: Any!) {
        // 啟動時 IMKit 不一定呼叫過 setValue,所以主動問一次。
        guard let mode = self.value(forTag: LuminaKeyInputController.inputModeTag,
                                    client: sender) as? String else { return }
        let script = InputModeBinding.script(forInputSourceID: mode)
        if script != .unspecified {
            AppContext.shared.inputModeScript = script
        }
    }

    // ───────────────────────── 按鍵 ─────────────────────────

    override func handle(_ event: NSEvent!, client sender: Any!) -> Bool {
        guard let event, engine.hasSession else { return false }
        switch event.type {
        case .keyDown:
            return process(event: event, isKeyUp: false, client: sender)
        case .flagsChanged:
            return processFlags(event: event, client: sender)
        default:
            return false
        }
    }

    private func macEvent(_ e: NSEvent, isKeyUp: Bool,
                          flagsOverride: MacModifierFlags? = nil) -> MacKeyEvent {
        MacKeyEvent(keyCode: e.keyCode,
                    charactersIgnoringModifiers: e.charactersIgnoringModifiers ?? "",
                    flags: flagsOverride
                        ?? MacModifierFlags(rawValue: e.modifierFlags.rawValue),
                    isKeyUp: isKeyUp)
    }

    private func process(event: NSEvent, isKeyUp: Bool, client sender: Any!) -> Bool {
        // Command 組合鍵一律讓宿主處理。⌘C / ⌘V 被輸入法吃掉是最惹人厭的 bug 之一,
        // 而 librime 的預設 keybinding 裡沒有任何 Command 組合。
        //
        // ⚠ **Control 與 F 鍵不在此列,而且刻意如此。** librime 內建的方案選單
        //   (switcher)綁的正是 `Control+grave` 與 `F4`(rime-prelude 的
        //   default.yaml)。老 RIME 使用者按下去會期待它在,而那是打字打到一半
        //   換方案最快的路徑。這兩個鍵只要送得進 librime 就會動,不需要我們
        //   自己畫選單 —— switcher 的選項就是一般的候選,候選窗照樣畫得出來。
        if event.modifierFlags.contains(.command) { return false }

        guard let stroke = mapper.stroke(for: macEvent(event, isKeyUp: isKeyUp)) else {
            return false
        }
        let consumed = engine.process(keysym: stroke.keysym, modifiers: stroke.modifiers)

        // ⚠ **每個輸入事件只 acquire 一次。** commit 在 acquire 當下就被消費,
        //   分兩次拿會遺失第一次的 commit_text。
        guard let snap = engine.snapshot() else { return consumed }
        deliver(snap, to: sender)
        return consumed
    }

    private func processFlags(event: NSEvent, client sender: Any!) -> Bool {
        let flags = MacModifierFlags(rawValue: event.modifierFlags.rawValue)
        guard let t = tracker.transition(keyCode: event.keyCode, flags: flags) else {
            return false
        }
        guard let stroke = mapper.stroke(for: macEvent(event, isKeyUp: t.isKeyUp,
                                                       flagsOverride: t.flags)) else {
            return false
        }
        _ = engine.process(keysym: stroke.keysym, modifiers: stroke.modifiers)
        if let snap = engine.snapshot() { deliver(snap, to: sender) }
        // 修飾鍵本身**永遠不消費**:回傳 true 會讓宿主收不到 Shift,
        // 選字、拖曳、快捷鍵全部壞掉。
        return false
    }

    // ───────────────────────── 上屏與候選窗 ─────────────────────────

    /// 一次快照的完整處置。這是唯一會碰 client 與候選窗的地方。
    private func deliver(_ snap: RimeSnapshot, to sender: Any!) {
        if let commit = snap.commitText, !commit.isEmpty {
            insert(commit, client: sender)
        }

        switch CommitPolicy.decide(snap.compositionState) {
        case .commit:
            // 轉換完成待確認(注音類方案選字後會停在這裡)。
            if engine.commitComposition(), let after = engine.snapshot() {
                if let t = after.commitText, !t.isEmpty { insert(t, client: sender) }
                updatePanel(after, client: sender)
                return
            }
        case .keepComposing, .idle:
            break
        }
        updatePanel(snap, client: sender)
    }

    private func updatePanel(_ snap: RimeSnapshot, client sender: Any!) {
        let panel = AppContext.shared.panel
        guard CommitPolicy.shouldShowPanel(snap.compositionState) else {
            setMarkedText("", client: sender)
            panel.hide()
            return
        }
        // 組字串**一定**交給宿主(規範 §8.7 的桌面端裁決):插入點定位、
        // 宿主自己的重繪、無障礙工具讀得到組字內容,都只有這條路徑做得到。
        setMarkedText(snap.preedit, client: sender)
        panel.show(theme: AppContext.shared.themes.current, snapshot: snap,
                   caretRect: caretRect(client: sender))
    }

    private func insert(_ text: String, client sender: Any!) {
        guard let client = sender as? IMKTextInput else { return }
        client.insertText(text, replacementRange: NSRange(location: NSNotFound, length: 0))
    }

    private func setMarkedText(_ text: String, client sender: Any!) {
        guard let client = sender as? IMKTextInput else { return }
        if text.isEmpty {
            client.setMarkedText("", selectionRange: NSRange(location: 0, length: 0),
                                 replacementRange: NSRange(location: NSNotFound, length: 0))
        } else {
            let attr = NSAttributedString(string: text, attributes: [
                .markedClauseSegment: 0,
                .underlineStyle: NSUnderlineStyle.single.rawValue,
            ])
            client.setMarkedText(attr,
                                 selectionRange: NSRange(location: text.utf16.count, length: 0),
                                 replacementRange: NSRange(location: NSNotFound, length: 0))
        }
    }

    private func caretRect(client sender: Any!) -> NSRect {
        var rect = NSRect.zero
        if let client = sender as? IMKTextInput {
            _ = client.attributes(forCharacterIndex: 0, lineHeightRectangle: &rect)
        }
        if rect == .zero {
            // 宿主不回報插入點(部分 Electron / Java app):退到滑鼠位置,
            // 總比把候選窗畫在螢幕左下角好。
            let p = NSEvent.mouseLocation
            rect = NSRect(x: p.x, y: p.y, width: 1, height: 16)
        }
        return rect
    }

    private func selectCandidate(index: Int, client sender: Any!) {
        guard engine.selectCandidate(Int32(index)) else { return }
        guard let snap = engine.snapshot() else { return }
        deliver(snap, to: sender)
    }

    // ───────────────────────── 選單 ─────────────────────────
    //
    // 這是使用者打開設定的**主要入口**,也是 macOS 上唯一不需要學、
    // 而且不會跟任何 app 的快捷鍵衝突的位置(選單列上的輸入法圖示)。
    //
    // ⚠ 這裡的每一項都必須真的做事。**做不到的就不畫出來** ——
    //   這個專案抓到過四個「按了沒反應」的鍵,共同點是畫面完全正常。

    override func menu() -> NSMenu! {
        let lang = L10n.resolve(AppContext.shared.settings.current.uiLanguage)
        let menu = NSMenu(title: "LuminaKey")

        let settings = NSMenuItem(title: T("設定…", "设置…", "Settings…")[lang],
                                  action: #selector(openSettings(_:)), keyEquivalent: "")
        settings.target = self
        menu.addItem(settings)
        menu.addItem(.separator())

        // 方案切換。目前這一個打勾 —— 沒有打勾的話使用者不知道自己在哪。
        let currentId = engine.snapshot()?.status.schemaId ?? ""
        let schemas = engine.schemaList()
        if schemas.isEmpty {
            let none = NSMenuItem(title: T("(還沒有任何方案)", "(还没有任何方案)",
                                           "(no schemas yet)")[lang],
                                  action: nil, keyEquivalent: "")
            none.isEnabled = false
            menu.addItem(none)
        } else {
            for (id, name) in schemas {
                let item = NSMenuItem(title: name.isEmpty ? id : name,
                                      action: #selector(selectSchemaFromMenu(_:)),
                                      keyEquivalent: "")
                item.representedObject = id
                item.target = self
                item.state = (id == currentId) ? .on : .off
                menu.addItem(item)
            }
        }

        menu.addItem(.separator())
        // ⚠ 這一項刻意帶著刪節號:它**開啟設定視窗並在那裡執行**,
        //   而不是在這裡默默跑十幾秒。`rs_deploy()` 是非同步的,
        //   在選單裡按下去只會立刻返回,畫面上什麼都不會發生 ——
        //   那正是 Android 端真機回報過的「按了沒反應」。
        let redeploy = NSMenuItem(title: T("重新整理字詞…", "重新整理字词…",
                                           "Rebuild dictionaries…")[lang],
                                  action: #selector(redeployFromMenu(_:)), keyEquivalent: "")
        redeploy.target = self
        menu.addItem(redeploy)

        let about = NSMenuItem(title: T("關於 LuminaKey", "关于 LuminaKey", "About LuminaKey")[lang],
                               action: #selector(openAbout(_:)), keyEquivalent: "")
        about.target = self
        menu.addItem(about)
        return menu
    }

    @objc private func selectSchemaFromMenu(_ sender: NSMenuItem) {
        guard let id = sender.representedObject as? String else { return }
        engine.selectSchema(id)
        // 使用者從選單明確選了方案 —— 記下來,免得下一次 activateServer
        // 又依輸入模式把它換掉。這與設定介面裡的「一律用這個方案」是同一格。
        AppContext.shared.settings.update { s in
            switch AppContext.shared.inputModeScript {
            case .hant: s.pinnedSchemaHant = id
            case .hans: s.pinnedSchemaHans = id
            case .unspecified: s.pinnedSchemaId = id
            }
        }
        AppContext.shared.applySettings()
    }

    @objc private func redeployFromMenu(_ sender: NSMenuItem) {
        SettingsLauncher.open(arguments: ["--page=advanced", "--redeploy"])
    }

    @objc private func openSettings(_ sender: NSMenuItem) {
        SettingsLauncher.open(arguments: [])
    }

    @objc private func openAbout(_ sender: NSMenuItem) {
        SettingsLauncher.open(arguments: ["--page=advanced"])
    }
}

/// 打開設定 app。
///
/// ⚠ 輸入法本體是 `LSBackgroundOnly`,**沒有辦法自己顯示視窗** ——
///   它連 NSAlert 都彈不出來。所以任何需要使用者看到東西的動作,
///   都必須經由設定 app。找不到設定 app 時寫 log 而不是靜靜地什麼都不做。
enum SettingsLauncher {
    static var settingsAppURL: URL? {
        guard let res = Bundle.main.resourceURL else { return nil }
        let url = res.appendingPathComponent("LuminaKeySettings.app")
        return FileManager.default.fileExists(atPath: url.path) ? url : nil
    }

    static func open(arguments: [String]) {
        guard let url = settingsAppURL else {
            NSLog("LuminaKey: 找不到 LuminaKeySettings.app —— 這份 .app 的內容不完整")
            return
        }
        let cfg = NSWorkspace.OpenConfiguration()
        cfg.activates = true
        cfg.arguments = arguments
        // 已經開著的話帶到前景,不要開第二份。
        cfg.createsNewApplicationInstance = false
        NSWorkspace.shared.openApplication(at: url, configuration: cfg) { _, error in
            if let error {
                NSLog("LuminaKey: 打不開設定視窗:\(error.localizedDescription)")
            }
        }
        // ⚠ 已經在跑的 app **不會**再收到一次 argv,所以指令另外送一則通知。
        //   少了這一行,第二次點「重新整理字詞…」就只是把視窗帶到前面,
        //   什麼都不會發生 —— 又一顆按了沒反應的選單項。
        let parsed = SettingsCommand.parse(arguments)
        var info: [String: String] = ["redeploy": parsed.redeploy ? "1" : "0"]
        if let page = parsed.page { info["page"] = page }
        // ⚠ `default` 在這個位置是**方法**不是屬性,少了 () 編不過。
        //   (AppDelegate 那邊的 `.default.addObserver` 之所以編得過,
        //    是因為那個呼叫形狀讓 Swift 選到了另一個多載。)
        DistributedNotificationCenter.default().postNotificationName(
            Notification.Name(SettingsCommand.notification), object: nil,
            userInfo: info, deliverImmediately: true)
    }
}
