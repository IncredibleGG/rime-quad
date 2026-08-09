//
//  RemapPage.swift — 「換鍵」那一頁
//
//  ── 為什麼是一個自給自足的 NSView,而不是 SettingsWindow 裡的一個方法 ────
//  這一頁有自己的一整套互動(選兩顆鍵、確認、逐列還原),`docs/ui-design.md`
//  §4.5 也把它與市集並列為「兩層深度」的唯二例外。做成獨立的 view 有兩個
//  好處:它的狀態(現在選了哪兩顆)不必擠進 SettingsWindowController,
//  而且整頁重畫的時候不會把使用者選到一半的東西吃掉。
//
//  ⚠ 這正是 Android 端踩過的那個坑的鏡像:`prefs/KeyRemapSection.kt` 是
//    LazyColumn 裡的一個 item,捲出畫面就被銷毀,使用者選好兩顆鍵、捲下去
//    找按鈕,按鈕卻是灰的 —— 而他完全看不出自己的選擇何時被吃掉。
//
//  ── 畫面上的每一個字都不准出現實作的詞 ─────────────────────────────────
//  `docs/ui-design.md` §6.2:remap → 「換鍵」,動作說成「把兩顆鍵對調」。
//  layout / layer / keysym 一個都不准出現。驗證失敗時**先講白話結論**,
//  技術細節收起來(§7.7)。這些字串全部走 `RemapCopy`,由
//  `KeyRemapTests.testEveryNoticeCodeHasPlainLanguage` 守著。
//

import AppKit

final class RemapPage: NSView {

    /// 實體鍵盤上的三排字母。**與使用者眼前那塊鍵盤同一個順序** ——
    /// 這一頁唯一的作用就是讓他認得出自己要點的是哪一顆。
    private static let rows = ["qwertyuiop", "asdfghjkl", "zxcvbnm"]

    private let lang: UiLanguage
    private let store: KeyRemapStore
    private var picked: [Character] = []
    private let stack = NSStackView()

    init(lang: UiLanguage, store: KeyRemapStore) {
        self.lang = lang
        self.store = store
        super.init(frame: .zero)
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = SettingsMetrics.rowSpacing
        stack.translatesAutoresizingMaskIntoConstraints = false
        addSubview(stack)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: leadingAnchor),
            stack.trailingAnchor.constraint(lessThanOrEqualTo: trailingAnchor),
            stack.topAnchor.constraint(equalTo: topAnchor),
            stack.bottomAnchor.constraint(equalTo: bottomAnchor),
        ])
        rebuild()
    }

    required init?(coder: NSCoder) { fatalError() }

    // MARK: - 版面

    private func rebuild() {
        stack.arrangedSubviews.forEach { $0.removeFromSuperview() }
        let compiled = store.compiled

        stack.addArrangedSubview(UI.label(
            T("點兩顆鍵,下面就會問你要不要對調。對調之後,按下左邊那顆鍵打出來的就是右邊那顆的字。",
              "点两颗键,下面就会问你要不要对调。对调之后,按下左边那颗键打出来的就是右边那颗的字。",
              "Tap two keys and you will be asked whether to swap them. After swapping, pressing the first key types what the second one used to type.")[lang],
            size: 12, colour: .secondaryLabelColor))

        for notice in compiled.notices {
            stack.addArrangedSubview(UI.notice(title: notice.title[lang],
                                               body: notice.bodyText(lang)))
        }

        stack.addArrangedSubview(keyboardPicture(compiled))

        if picked.count == 2 { stack.addArrangedSubview(confirmCard(compiled)) }

        if !compiled.cycles.isEmpty {
            stack.addArrangedSubview(UI.label(
                T("你換過的", "你换过的", "What you changed")[lang], size: 13, weight: .medium))
            stack.addArrangedSubview(UI.card(compiled.cycles.map(row(for:))))
        }

        if store.hasAnythingToRestore { stack.addArrangedSubview(restoreEverything(compiled)) }
    }

    /// 鍵盤縮圖。**每一顆鍵顯示的是它現在會送出什麼**,不是鍵帽上印什麼 ——
    /// 桌面端沒有軟鍵盤可以跟著變,這是使用者唯一看得到「換過了」的地方。
    /// 換過的鍵底下再用小字寫出鍵帽上印的字母,他才對得上要按哪一顆。
    private func keyboardPicture(_ compiled: RemapCompilation) -> NSView {
        let rows: [NSView] = RemapPage.rows.map { line in
            let keys: [NSView] = line.map { cap in keyButton(cap, compiled: compiled) }
            let row = NSStackView(views: keys)
            row.orientation = .horizontal
            row.spacing = 6
            return row
        }
        let grid = NSStackView(views: rows)
        grid.orientation = .vertical
        grid.alignment = .centerX
        grid.spacing = 6
        return UI.card([grid])
    }

    private func keyButton(_ cap: Character, compiled: RemapCompilation) -> NSView {
        let now = compiled.table.output(forLetter: cap)
        let isPicked = picked.contains(cap)

        let b = ActionButton(title: String(now).uppercased(), target: nil, action: nil)
        b.bezelStyle = .rounded
        b.font = .systemFont(ofSize: 15, weight: isPicked ? .bold : .regular)
        if isPicked { b.bezelColor = .controlAccentColor }
        b.widthAnchor.constraint(equalToConstant: 40).isActive = true
        b.onClick = { [weak self] in self?.pick(cap) }
        // 朗讀名要說出**這顆鍵現在做什麼**,而不是唸一個字母。
        b.setAccessibilityLabel(
            now == cap
            ? T("{0} 鍵", "{0} 键", "{0} key").format(lang, String(cap).uppercased())
            : T("{0} 鍵,現在打出 {1}", "{0} 键,现在打出 {1}", "{0} key, now types {1}")
                .format(lang, String(cap).uppercased(), String(now).uppercased()))

        guard now != cap else { return b }
        let hint = UI.label(String(cap).uppercased(), size: 9.5, colour: .tertiaryLabelColor)
        hint.alignment = .center
        let cell = NSStackView(views: [b, hint])
        cell.orientation = .vertical
        cell.alignment = .centerX
        cell.spacing = 1
        return cell
    }

    private func confirmCard(_ compiled: RemapCompilation) -> NSView {
        let a = compiled.table.output(forLetter: picked[0])
        let b = compiled.table.output(forLetter: picked[1])
        let headline = UI.label("\(String(a).uppercased())  ⇄  \(String(b).uppercased())",
                                size: 20, weight: .semibold)
        let go = UI.button(T("對調", "对调", "Swap")[lang]) { [weak self] in self?.applySwap() }
        go.keyEquivalent = "\r"
        let cancel = UI.button(T("取消", "取消", "Cancel")[lang]) { [weak self] in
            self?.picked = []
            self?.rebuild()
        }
        let buttons = NSStackView(views: [go, cancel])
        buttons.orientation = .horizontal
        buttons.spacing = 8
        return UI.card([headline, buttons])
    }

    private func row(for cycle: RemapCycle) -> NSView {
        let arrow = cycle.letters.count == 2 ? "  ⇄  " : "  →  "
        var faces = cycle.letters.map { String($0).uppercased() }
        if cycle.letters.count > 2 { faces.append(faces[0]) }
        let text = UI.label(faces.joined(separator: arrow), size: 13, weight: .medium)

        var views: [NSView] = [text]
        // 手機端一次只寫一層,所以「只有沒按 Shift 的時候會換」是常態,
        // 不是壞掉。照實說出來,不要假裝兩邊都換了。
        if !(cycle.appliesUnshifted && cycle.appliesShifted) {
            views.append(UI.label(
                cycle.appliesUnshifted
                ? T("只有沒按 Shift 的時候會換", "只有没按 Shift 的时候会换",
                    "Only swaps when Shift is not held")[lang]
                : T("只有按著 Shift 的時候會換", "只有按着 Shift 的时候会换",
                    "Only swaps while Shift is held")[lang],
                size: 11, colour: .secondaryLabelColor))
        }
        let label = NSStackView(views: views)
        label.orientation = .vertical
        label.alignment = .leading
        label.spacing = 2

        let restore = UI.button(T("還原", "还原", "Restore")[lang]) { [weak self] in
            guard let self else { return }
            self.write(DesktopRemap.restoring(cycle, in: self.store.document))
        }
        let line = NSStackView(views: [label, restore])
        line.orientation = .horizontal
        line.spacing = 12
        return line
    }

    /// 「全部還原」。**會消失的與不會消失的都要說出來** —— 使用者按這顆按鈕
    /// 之前唯一想知道的就是「我會不會失去別的東西」(`docs/ui-design.md` §7.7)。
    private func restoreEverything(_ compiled: RemapCompilation) -> NSView {
        let hairline = NSBox()
        hairline.boxType = .separator
        hairline.translatesAutoresizingMaskIntoConstraints = false
        hairline.widthAnchor.constraint(equalToConstant: 520).isActive = true

        let button = UI.button(
            T("全部還原成原本的樣子", "全部还原成原本的样子", "Put every key back")[lang]) {
            [weak self] in
            guard let self else { return }
            self.write(DesktopRemap.clearing(in: self.store.document))
        }
        button.contentTintColor = .systemRed

        let count = compiled.cycles.count
        let what = count > 0
            ? T("你換過的 {0} 處會消失。你打出來的字、你自己加的詞都不受影響。",
                "你换过的 {0} 处会消失。你打出来的字、你自己加的词都不受影响。",
                "The {0} change(s) above will be undone. What you type and the words you added are not affected.")
                .format(lang, String(count))
            : T("這台電腦上的換鍵會全部清掉。你打出來的字、你自己加的詞都不受影響。",
                "这台电脑上的换键会全部清掉。你打出来的字、你自己加的词都不受影响。",
                "Every key change on this computer will be cleared. What you type and the words you added are not affected.")[lang]

        let box = NSStackView(views: [hairline, button, UI.label(what, size: 11.5,
                                                                colour: .secondaryLabelColor)])
        box.orientation = .vertical
        box.alignment = .leading
        box.spacing = 8
        return box
    }

    // MARK: - 動作

    private func pick(_ cap: Character) {
        if let i = picked.firstIndex(of: cap) {
            picked.remove(at: i)
        } else if picked.count < 2 {
            picked.append(cap)
        } else {
            // 已經選了兩顆還點第三顆:換掉第一顆。比「什麼都不發生」好懂 ——
            // 一顆點下去毫無反應的鍵,使用者會以為畫面壞了。
            picked = [picked[1], cap]
        }
        rebuild()
    }

    private func applySwap() {
        guard picked.count == 2 else { return }
        switch DesktopRemap.swapping(picked[0], picked[1], in: store.document) {
        case .success(let next):
            picked = []
            write(next)
        case .failure(let notice):
            // 白話結論在最前面,原因在下面 —— 不是把驗證器的訊息倒出來。
            let a = NSAlert()
            a.messageText = notice.title[lang]
            a.informativeText = notice.bodyText(lang)
            a.addButton(withTitle: T("知道了", "知道了", "OK")[lang])
            a.runModal()
        }
    }

    /// 寫檔 + 通知輸入法本體 + 重畫。
    ///
    /// 三件事綁在一起是刻意的:少了通知,使用者要重開輸入法才會生效,
    /// 而畫面上明明已經顯示換好了 —— 那是這個專案最常見的一種「看得到
    /// 但摸不到」。
    private func write(_ next: RemapDocument) {
        guard store.save(next) else {
            let a = NSAlert()
            a.messageText = T("存不起來", "存不起来", "Could not save")[lang]
            a.informativeText = T("換鍵沒有存成功,鍵盤還是原本的樣子。硬碟可能滿了,或這個資料夾沒有寫入權限。",
                                  "换键没有存成功,键盘还是原本的样子。硬盘可能满了,或这个文件夹没有写入权限。",
                                  "Nothing was changed and your keyboard still works as before. The disk may be full, or this folder may not be writable.")[lang]
            a.addButton(withTitle: T("知道了", "知道了", "OK")[lang])
            a.runModal()
            return
        }
        DistributedNotificationCenter.default().postNotificationName(
            Notification.Name(KeyRemapStore.changedNotification), object: nil,
            userInfo: nil, deliverImmediately: true)
        rebuild()
    }
}
