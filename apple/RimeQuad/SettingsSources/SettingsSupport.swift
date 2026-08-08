//
//  SettingsSupport.swift — 設定視窗共用的小零件與狀態
//
//  ⚠ 這個檔案(以及同目錄的其他檔案)是 **AppKit 那一側**,CI 驗不到。
//    紀律照 apple/README.md §1:**凡是算得出來的都不要留在這裡。**
//    這裡剩下的只有「把 RimeQuadKit 算好的東西畫出來」與「把點擊轉成呼叫」。
//

import AppKit

// MARK: - 版面常數

/// ⚠ 名字不能只叫 `Metrics`:RimeQuadKit 已經有一個 `Metrics`
///   (主題規範 §8.5 的那一塊),而 AppSources 與 Kit 編在同一個
///   module 裡。撞名的錯誤訊息會變成「type 'Metrics' has no member
///   'pagePadding'」與莫名其妙的 Float16 轉型失敗 —— 看起來完全
///   不像是撞名。
enum SettingsMetrics {
    static let sidebarWidth: CGFloat = 208
    static let pagePadding: CGFloat = 24
    static let rowSpacing: CGFloat = 18
    static let windowSize = NSSize(width: 860, height: 600)
}

// MARK: - 小零件

enum UI {

    static func label(_ text: String, size: CGFloat = 13,
                      weight: NSFont.Weight = .regular,
                      colour: NSColor = .labelColor) -> NSTextField {
        let f = NSTextField(labelWithString: text)
        f.font = .systemFont(ofSize: size, weight: weight)
        f.textColor = colour
        f.lineBreakMode = .byWordWrapping
        f.maximumNumberOfLines = 0
        f.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        return f
    }

    /// 標題 + 一句白話。**白話是必填的** —— 目錄那一層已經用型別擋住了,
    /// 這裡只是把它畫出來。
    static func titledRow(_ spec: SettingSpec, lang: UiLanguage, control: NSView) -> NSView {
        let text = NSStackView(views: [
            label(spec.title[lang], size: 13, weight: .medium),
            label(spec.blurb[lang], size: 11.5, colour: .secondaryLabelColor),
        ])
        text.orientation = .vertical
        text.alignment = .leading
        text.spacing = 3

        let row = NSStackView(views: [text, control])
        row.orientation = .vertical
        row.alignment = .leading
        row.spacing = 8
        return row
    }

    /// 幾選一。用 NSSegmentedControl 而不是下拉選單:**選項全部看得見**,
    /// 使用者不必先點開才知道有什麼可以選。
    static func segmented(_ choices: [SettingChoice], lang: UiLanguage,
                          selected: String,
                          action: @escaping (String) -> Void) -> NSView {
        let seg = ActionSegmented(labels: choices.map { $0.label[lang] },
                                  trackingMode: .selectOne)
        seg.values = choices.map(\.value)
        seg.onChange = action
        if let i = choices.firstIndex(where: { $0.value == selected }) {
            seg.selectedSegment = i
        }
        seg.segmentDistribution = .fillEqually
        return seg
    }

    static func button(_ title: String, key: String = "",
                       action: @escaping () -> Void) -> NSButton {
        let b = ActionButton(title: title, target: nil, action: nil)
        b.bezelStyle = .rounded
        b.keyEquivalent = key
        b.onClick = action
        return b
    }

    static func checkbox(_ title: String, on: Bool,
                         action: @escaping (Bool) -> Void) -> NSButton {
        let b = ActionButton(checkboxWithTitle: title, target: nil, action: nil)
        b.state = on ? .on : .off
        b.onClick = { [weak b] in action(b?.state == .on) }
        return b
    }

    static func card(_ views: [NSView]) -> NSView {
        let box = NSBox()
        box.boxType = .custom
        box.fillColor = .controlBackgroundColor
        box.borderColor = .separatorColor
        box.borderWidth = 1
        box.cornerRadius = 8
        box.contentViewMargins = NSSize(width: 14, height: 12)
        let stack = NSStackView(views: views)
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 10
        box.contentView = stack
        return box
    }

    static func scrollingPage(_ content: NSView) -> NSView {
        let scroll = NSScrollView()
        scroll.hasVerticalScroller = true
        scroll.drawsBackground = false
        scroll.autohidesScrollers = true
        let doc = NSView()
        doc.translatesAutoresizingMaskIntoConstraints = false
        content.translatesAutoresizingMaskIntoConstraints = false
        doc.addSubview(content)
        NSLayoutConstraint.activate([
            content.topAnchor.constraint(equalTo: doc.topAnchor, constant: SettingsMetrics.pagePadding),
            content.leadingAnchor.constraint(equalTo: doc.leadingAnchor,
                                             constant: SettingsMetrics.pagePadding),
            content.trailingAnchor.constraint(equalTo: doc.trailingAnchor,
                                              constant: -SettingsMetrics.pagePadding),
            content.bottomAnchor.constraint(equalTo: doc.bottomAnchor,
                                            constant: -SettingsMetrics.pagePadding),
        ])
        scroll.documentView = doc
        // 讓內容寬度跟著視窗走,不然文字不會換行。
        doc.widthAnchor.constraint(equalTo: scroll.widthAnchor).isActive = true
        return scroll
    }

    /// 一塊說明卡:離線、沒有方案、找不到輸入法……
    /// **每一張都要附一顆按鈕**,否則使用者讀完之後不知道要做什麼。
    static func notice(title: String, body: String,
                       actionTitle: String? = nil,
                       action: (() -> Void)? = nil) -> NSView {
        var views: [NSView] = [
            label(title, size: 13, weight: .semibold),
            label(body, size: 11.5, colour: .secondaryLabelColor),
        ]
        if let actionTitle, let action {
            views.append(button(actionTitle, action: action))
        }
        return card(views)
    }
}

// MARK: - target/action 的包裝

/// AppKit 的 target/action 要一個 ObjC selector。每個按鈕各寫一個
/// `@objc func` 會讓這個檔案變成兩百個一行函式,所以包一層。
final class ActionButton: NSButton {
    var onClick: (() -> Void)?
    override init(frame: NSRect) {
        super.init(frame: frame)
        target = self
        action = #selector(fire)
    }
    required init?(coder: NSCoder) { fatalError() }
    convenience init(title: String, target: AnyObject?, action: Selector?) {
        self.init(frame: .zero)
        self.title = title
        self.bezelStyle = .rounded
        self.target = self
        self.action = #selector(fire)
    }
    convenience init(checkboxWithTitle title: String, target: AnyObject?, action: Selector?) {
        self.init(frame: .zero)
        self.title = title
        self.setButtonType(.switch)
        self.target = self
        self.action = #selector(fire)
    }
    @objc private func fire() { onClick?() }
}

final class ActionSegmented: NSSegmentedControl {
    var values: [String] = []
    var onChange: ((String) -> Void)?

    convenience init(labels: [String], trackingMode: NSSegmentedControl.SwitchTracking) {
        self.init(labels: labels, trackingMode: trackingMode, target: nil, action: nil)
        self.target = self
        self.action = #selector(fire)
    }
    @objc private func fire() {
        let i = selectedSegment
        guard i >= 0, i < values.count else { return }
        onChange?(values[i])
    }
}

final class ActionPopUp: NSPopUpButton {
    var values: [String] = []
    var onChange: ((String) -> Void)?

    static func make(titles: [String], values: [String], selected: String,
                     onChange: @escaping (String) -> Void) -> ActionPopUp {
        let p = ActionPopUp(frame: .zero, pullsDown: false)
        p.addItems(withTitles: titles)
        p.values = values
        p.onChange = onChange
        p.target = p
        p.action = #selector(fire)
        if let i = values.firstIndex(of: selected) { p.selectItem(at: i) }
        return p
    }
    @objc private func fire() {
        let i = indexOfSelectedItem
        guard i >= 0, i < values.count else { return }
        onChange?(values[i])
    }
}

// MARK: - 忙碌覆蓋層

/// 部署、下載、安裝時蓋在上面的那一層。
///
/// ⚠ **存在的理由**:Android 端真機回報過「按了重新部署,什麼都沒發生」——
/// `rs_deploy()` 是非同步的,而畫面上沒有任何東西告訴使用者它在跑。
/// 所以只要有長時間作業,就一定要有這一層,而且要顯示**經過的時間**
/// (librime 不給百分比,唯一誠實的進度就是秒數)。
final class BusyOverlay: NSView {

    private let titleField = UI.label("", size: 15, weight: .semibold)
    private let detailField = UI.label("", size: 12, colour: .secondaryLabelColor)
    private let bar = NSProgressIndicator()

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        layer?.backgroundColor = NSColor.windowBackgroundColor.withAlphaComponent(0.92).cgColor
        bar.style = .bar
        bar.isIndeterminate = true
        bar.minValue = 0
        bar.maxValue = 1
        let stack = NSStackView(views: [titleField, bar, detailField])
        stack.orientation = .vertical
        stack.alignment = .centerX
        stack.spacing = 12
        stack.translatesAutoresizingMaskIntoConstraints = false
        addSubview(stack)
        NSLayoutConstraint.activate([
            stack.centerXAnchor.constraint(equalTo: centerXAnchor),
            stack.centerYAnchor.constraint(equalTo: centerYAnchor),
            bar.widthAnchor.constraint(equalToConstant: 320),
            stack.widthAnchor.constraint(lessThanOrEqualToConstant: 460),
        ])
        isHidden = true
    }
    required init?(coder: NSCoder) { fatalError() }

    func show(title: String, detail: String, fraction: Double) {
        titleField.stringValue = title
        detailField.stringValue = detail
        if fraction < 0 {
            bar.isIndeterminate = true
            bar.startAnimation(nil)
        } else {
            bar.isIndeterminate = false
            bar.doubleValue = fraction
        }
        isHidden = false
    }

    func hide() {
        bar.stopAnimation(nil)
        isHidden = true
    }
}
