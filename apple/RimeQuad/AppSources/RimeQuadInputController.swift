//
//  RimeQuadInputController.swift — IMKInputController
//
//  ⚠ 類別名稱 `RimeQuadInputController` 同時出現在三個地方，改一個就要改三個：
//     · 這裡的 @objc(...) —— Swift 的類別名會被 mangle 成
//       `_TtC8RimeQuad24RimeQuadInputController`，IMKit 用 NSClassFromString
//       找它，**不加 @objc 就一定找不到**（症狀是輸入法裝得起來、選得到、
//       但一個字都打不出來，而且完全沒有錯誤訊息）。
//     · Info.plist 的 InputMethodServerControllerClass。
//     · apple/scripts/verify_app_bundle.sh 的斷言。
//

import AppKit
import InputMethodKit

@objc(RimeQuadInputController)
final class RimeQuadInputController: IMKInputController {

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
            // commit 之後必須 acquire 才拿得到文字（見 rime_shell.h）。
            if let snap = engine.snapshot() { deliver(snap, to: sender) }
        }
        AppContext.shared.panel.hide()
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
        // Command 組合鍵一律讓宿主處理。⌘C / ⌘V 被輸入法吃掉是最惹人厭的 bug 之一，
        // 而 librime 的預設 keybinding 裡沒有任何 Command 組合。
        if event.modifierFlags.contains(.command) { return false }

        guard let stroke = mapper.stroke(for: macEvent(event, isKeyUp: isKeyUp)) else {
            return false
        }
        let consumed = engine.process(keysym: stroke.keysym, modifiers: stroke.modifiers)

        // ⚠ **每個輸入事件只 acquire 一次。** commit 在 acquire 當下就被消費，
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
        // 修飾鍵本身**永遠不消費**：回傳 true 會讓宿主收不到 Shift，
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
            // 轉換完成待確認（注音類方案選字後會停在這裡）。
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
        // preedit 交給候選窗畫（主題的 `preedit` 區塊）；同時放一段空的 marked text，
        // 好讓宿主 app 知道「正在組字」而不要送出自己的補完。
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
            // 宿主不回報插入點（部分 Electron / Java app）：退到滑鼠位置，
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

    override func menu() -> NSMenu! {
        let menu = NSMenu(title: "RimeQuad")
        for (id, name) in engine.schemaList() {
            let item = NSMenuItem(title: name.isEmpty ? id : name,
                                  action: #selector(selectSchemaFromMenu(_:)), keyEquivalent: "")
            item.representedObject = id
            item.target = self
            menu.addItem(item)
        }
        if menu.numberOfItems > 0 { menu.addItem(.separator()) }
        let redeploy = NSMenuItem(title: NSLocalizedString("Redeploy", comment: ""),
                                  action: #selector(redeployFromMenu(_:)), keyEquivalent: "")
        redeploy.target = self
        menu.addItem(redeploy)
        return menu
    }

    @objc private func selectSchemaFromMenu(_ sender: NSMenuItem) {
        guard let id = sender.representedObject as? String else { return }
        engine.selectSchema(id)
    }

    @objc private func redeployFromMenu(_ sender: NSMenuItem) {
        engine.deploy()
    }
}
