//
//  SettingsWindow.swift — 側欄 + 內容,以及每一頁長什麼樣
//
//  ── 為什麼是側欄而不是一條長清單 ────────────────────────────────────────
//  使用者的原話有兩句:「你這個第一屏 99% 的人看不懂」,以及「UI 要比他好看」。
//  第一句的解法是每一項都有白話說明(那在 SettingsCatalog 裡已經強制了);
//  第二句的解法是**不要做成一條要一直捲的清單** —— 七頁,每頁短到一眼看完,
//  而且每一頁的標題底下都寫著它在回答哪一個問題。
//
//  ⚠ **每一顆按得下去的東西都必須真的做事。** 這個專案抓到過四個
//    「看得到但摸不到」的鍵,共同點是畫面完全正常、自動化全過。
//    所以這個檔案裡沒有任何佔位按鈕:做不到的功能就不畫出來。
//

import AppKit
import UniformTypeIdentifiers

final class SettingsWindowController: NSWindowController, NSTableViewDataSource,
                                      NSTableViewDelegate {

    private let model = SettingsModel()
    private let sidebar = NSTableView()
    private let contentBox = NSView()
    private let overlay = BusyOverlay(frame: .zero)
    private var pageIndex = 0

    private var lang: UiLanguage { model.lang }

    // MARK: - 建構

    init() {
        let window = NSWindow(
            contentRect: NSRect(origin: .zero, size: SettingsMetrics.windowSize),
            styleMask: [.titled, .closable, .miniaturizable, .resizable],
            backing: .buffered, defer: false)
        window.title = "LuminaKey"
        window.center()
        window.setFrameAutosaveName("LuminaKeySettings")
        window.minSize = NSSize(width: 720, height: 480)
        super.init(window: window)
        build()
        model.onChange = { [weak self] in self?.refresh() }
        model.onBusy = { [weak self] t, d, f in self?.overlay.show(title: t, detail: d, fraction: f) }
        model.onIdle = { [weak self] in self?.overlay.hide() }
        model.onResult = { [weak self] outcome in self?.present(outcome) }
        refresh()
    }
    required init?(coder: NSCoder) { fatalError() }

    private func build() {
        guard let root = window?.contentView else { return }

        sidebar.headerView = nil
        sidebar.rowHeight = 30
        sidebar.backgroundColor = .clear
        sidebar.selectionHighlightStyle = .sourceList
        let col = NSTableColumn(identifier: NSUserInterfaceItemIdentifier("page"))
        col.width = SettingsMetrics.sidebarWidth - 20
        sidebar.addTableColumn(col)
        sidebar.dataSource = self
        sidebar.delegate = self

        let sideScroll = NSScrollView()
        sideScroll.documentView = sidebar
        sideScroll.drawsBackground = false
        sideScroll.hasVerticalScroller = true

        let effect = NSVisualEffectView()
        effect.material = .sidebar
        effect.blendingMode = .behindWindow
        effect.translatesAutoresizingMaskIntoConstraints = false
        sideScroll.translatesAutoresizingMaskIntoConstraints = false
        effect.addSubview(sideScroll)

        contentBox.translatesAutoresizingMaskIntoConstraints = false
        overlay.translatesAutoresizingMaskIntoConstraints = false

        root.addSubview(effect)
        root.addSubview(contentBox)
        root.addSubview(overlay)

        NSLayoutConstraint.activate([
            effect.leadingAnchor.constraint(equalTo: root.leadingAnchor),
            effect.topAnchor.constraint(equalTo: root.topAnchor),
            effect.bottomAnchor.constraint(equalTo: root.bottomAnchor),
            effect.widthAnchor.constraint(equalToConstant: SettingsMetrics.sidebarWidth),
            sideScroll.topAnchor.constraint(equalTo: effect.topAnchor, constant: 10),
            sideScroll.leadingAnchor.constraint(equalTo: effect.leadingAnchor, constant: 8),
            sideScroll.trailingAnchor.constraint(equalTo: effect.trailingAnchor, constant: -8),
            sideScroll.bottomAnchor.constraint(equalTo: effect.bottomAnchor, constant: -10),

            contentBox.leadingAnchor.constraint(equalTo: effect.trailingAnchor),
            contentBox.topAnchor.constraint(equalTo: root.topAnchor),
            contentBox.trailingAnchor.constraint(equalTo: root.trailingAnchor),
            contentBox.bottomAnchor.constraint(equalTo: root.bottomAnchor),

            overlay.leadingAnchor.constraint(equalTo: root.leadingAnchor),
            overlay.topAnchor.constraint(equalTo: root.topAnchor),
            overlay.trailingAnchor.constraint(equalTo: root.trailingAnchor),
            overlay.bottomAnchor.constraint(equalTo: root.bottomAnchor),
        ])
        sidebar.selectRowIndexes(IndexSet(integer: 0), byExtendingSelection: false)
    }

    // MARK: - 側欄

    func numberOfRows(in tableView: NSTableView) -> Int { SettingsCatalog.pages.count }

    func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?,
                   row: Int) -> NSView? {
        let page = SettingsCatalog.pages[row]
        let cell = NSTableCellView()
        let text = UI.label(page.title[lang], size: 13, weight: .medium)
        text.translatesAutoresizingMaskIntoConstraints = false
        cell.addSubview(text)
        var leading: NSLayoutXAxisAnchor = cell.leadingAnchor
        var inset: CGFloat = 8
        if let image = NSImage(systemSymbolName: page.symbol, accessibilityDescription: nil) {
            let iv = NSImageView(image: image)
            iv.translatesAutoresizingMaskIntoConstraints = false
            iv.contentTintColor = .secondaryLabelColor
            cell.addSubview(iv)
            NSLayoutConstraint.activate([
                iv.leadingAnchor.constraint(equalTo: cell.leadingAnchor, constant: 8),
                iv.centerYAnchor.constraint(equalTo: cell.centerYAnchor),
                iv.widthAnchor.constraint(equalToConstant: 18),
            ])
            leading = iv.trailingAnchor
            inset = 8
        }
        NSLayoutConstraint.activate([
            text.leadingAnchor.constraint(equalTo: leading, constant: inset),
            text.centerYAnchor.constraint(equalTo: cell.centerYAnchor),
            text.trailingAnchor.constraint(lessThanOrEqualTo: cell.trailingAnchor, constant: -6),
        ])
        return cell
    }

    func tableViewSelectionDidChange(_ notification: Notification) {
        guard sidebar.selectedRow >= 0 else { return }
        pageIndex = sidebar.selectedRow
        refresh()
        if SettingsCatalog.pages[pageIndex].id == "store" { model.loadIndex() }
    }

    // MARK: - 重畫

    private func refresh() {
        contentBox.subviews.forEach { $0.removeFromSuperview() }
        let page = SettingsCatalog.pages[min(pageIndex, SettingsCatalog.pages.count - 1)]

        var blocks: [NSView] = [
            UI.label(page.title[lang], size: 22, weight: .semibold),
            UI.label(page.subtitle[lang], size: 12.5, colour: .secondaryLabelColor),
        ]
        blocks.append(contentsOf: body(for: page))

        let stack = NSStackView(views: blocks)
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = SettingsMetrics.rowSpacing
        stack.setCustomSpacing(4, after: blocks[0])

        let scroll = UI.scrollingPage(stack)
        scroll.translatesAutoresizingMaskIntoConstraints = false
        contentBox.addSubview(scroll)
        NSLayoutConstraint.activate([
            scroll.leadingAnchor.constraint(equalTo: contentBox.leadingAnchor),
            scroll.topAnchor.constraint(equalTo: contentBox.topAnchor),
            scroll.trailingAnchor.constraint(equalTo: contentBox.trailingAnchor),
            scroll.bottomAnchor.constraint(equalTo: contentBox.bottomAnchor),
        ])
        // ⚠ **不要**在這裡把 overlay 搬進 contentBox。它的 constraint 綁在
        //   root 上,換父視圖會讓那些 constraint 全部失效(執行期才會爆)。
        //   overlay 是 root 的子視圖而且加在 contentBox 之後,本來就在上層。
    }

    private func body(for page: SettingsPage) -> [NSView] {
        switch page.id {
        case "schemas": return schemasBody()
        case "remap": return remapBody()
        case "appearance": return appearanceBody()
        case "text": return textBody()
        case "dictionary": return dictionaryBody()
        case "store": return storeBody()
        case "network": return networkBody()
        case "advanced": return advancedBody()
        default: return []
        }
    }

    private func spec(_ id: String) -> SettingSpec { SettingsCatalog.item(id: id)! }

    // MARK: - 換鍵
    //
    // 這一頁的互動與狀態全部在 SettingsSources/RemapPage.swift ——
    // 它有自己的一整套互動(選兩顆鍵、確認、逐列還原),塞進這個
    // controller 只會讓兩邊都難改。

    private lazy var remapStore = KeyRemapStore(userDir: SettingsPaths.userDir)

    private func remapBody() -> [NSView] {
        // 每次進這一頁都重讀一次:輸入法本體與這裡是兩個行程,
        // 而使用者也可能在別的裝置上改過同一份檔案再同步回來。
        remapStore.reload()
        return [UI.label(spec("remap.keys").blurb[lang], size: 11.5,
                         colour: .secondaryLabelColor),
                RemapPage(lang: lang, store: remapStore)]
    }

    // MARK: - 輸入方案

    private func schemasBody() -> [NSView] {
        var out: [NSView] = []
        let rows = model.schemaRows

        if rows.isEmpty {
            out.append(UI.notice(
                title: T("還沒有任何方案", "还没有任何方案", "No schemas yet")[lang],
                body: T("這通常表示隨附的資料沒有裝好。到「方案市集」下載一個,或到「進階」按重新整理字詞。",
                        "这通常表示随附的数据没有装好。到「方案市集」下载一个,或到「进阶」按重新整理字词。",
                        "This usually means the bundled data did not install. Download one from the store, or rebuild from Advanced.")[lang],
                actionTitle: T("去方案市集", "去方案市集", "Open the store")[lang],
                action: { [weak self] in self?.go(to: "store") }))
            return out
        }

        var listViews: [NSView] = [UI.label(spec("schemas.list").blurb[lang], size: 11.5,
                                            colour: .secondaryLabelColor)]
        var working = model.pendingSchemaList ?? model.engine.enabledSchemas
        for row in rows {
            let scriptTag: String
            switch row.script {
            case .hant: scriptTag = T("繁", "繁", "Trad.")[lang]
            case .hans: scriptTag = T("簡", "简", "Simp.")[lang]
            case .unspecified: scriptTag = ""
            }
            var title = "\(row.name)  ·  \(row.id)"
            if !scriptTag.isEmpty { title += "  ·  \(scriptTag)" }
            if !row.installed {
                title += "  ·  " + T("找不到檔案", "找不到文件", "file missing")[lang]
            }
            let id = row.id
            let box = UI.checkbox(title, on: row.enabled) { [weak self] on in
                guard let self else { return }
                if on {
                    if !working.contains(id) { working.append(id) }
                } else {
                    working.removeAll { $0 == id }
                }
                self.model.pendingSchemaList = working
                self.refresh()
            }
            let up = UI.button("▲") { [weak self] in
                guard let self, let i = working.firstIndex(of: id), i > 0 else { return }
                working.swapAt(i, i - 1)
                self.model.pendingSchemaList = working
                self.refresh()
            }
            let down = UI.button("▼") { [weak self] in
                guard let self, let i = working.firstIndex(of: id),
                      i < working.count - 1 else { return }
                working.swapAt(i, i + 1)
                self.model.pendingSchemaList = working
                self.refresh()
            }
            up.isEnabled = row.enabled
            down.isEnabled = row.enabled
            let line = NSStackView(views: [box, up, down])
            line.orientation = .horizontal
            line.spacing = 6
            listViews.append(line)
        }
        out.append(UI.card(listViews))

        if model.hasPendingSchemaChanges {
            let apply = UI.button(T("套用變更", "套用变更", "Apply changes")[lang], key: "\r") {
                [weak self] in
                guard let self, let pending = self.model.pendingSchemaList else { return }
                self.model.run(T("套用方案設定", "套用方案设置", "Applying schema settings")) { p in
                    self.model.engine.reorder(pending, progress: p)
                }
            }
            let cancel = UI.button(T("取消", "取消", "Cancel")[lang]) { [weak self] in
                self?.model.pendingSchemaList = nil
                self?.refresh()
            }
            let bar = NSStackView(views: [
                UI.label(T("有還沒套用的變更。套用時會重新整理字詞,可能要等幾秒到幾十秒。",
                           "有还没套用的变更。套用时会重新整理字词,可能要等几秒到几十秒。",
                           "You have unapplied changes. Applying rebuilds the dictionaries and can take a while.")[lang],
                         size: 11.5, colour: .secondaryLabelColor),
                apply, cancel,
            ])
            bar.orientation = .vertical
            bar.alignment = .leading
            bar.spacing = 8
            out.append(UI.card([bar]))
        }

        // 跟著輸入來源
        let follow = spec("schemas.followInputMode")
        out.append(UI.titledRow(follow, lang: lang, control: UI.checkbox(
            T("開啟", "开启", "On")[lang], on: model.settings.followInputMode) { [weak self] on in
                self?.model.edit { $0.followInputMode = on }
                self?.refresh()
            }))

        let enabledRows = rows.filter { $0.enabled }
        let titles = [T("自動挑選", "自动挑选", "Choose automatically")[lang]]
            + enabledRows.map { "\($0.name)(\($0.id))" }
        let values = [""] + enabledRows.map(\.id)

        if model.settings.followInputMode {
            out.append(UI.titledRow(spec("schemas.pinnedHant"), lang: lang,
                control: ActionPopUp.make(titles: titles, values: values,
                                          selected: model.settings.pinnedSchemaHant ?? "") {
                    [weak self] v in
                    self?.model.edit { $0.pinnedSchemaHant = v.isEmpty ? nil : v }
                }))
            out.append(UI.titledRow(spec("schemas.pinnedHans"), lang: lang,
                control: ActionPopUp.make(titles: titles, values: values,
                                          selected: model.settings.pinnedSchemaHans ?? "") {
                    [weak self] v in
                    self?.model.edit { $0.pinnedSchemaHans = v.isEmpty ? nil : v }
                }))
        } else {
            out.append(UI.titledRow(spec("schemas.pinnedGlobal"), lang: lang,
                control: ActionPopUp.make(titles: titles, values: values,
                                          selected: model.settings.pinnedSchemaId ?? "") {
                    [weak self] v in
                    self?.model.edit { $0.pinnedSchemaId = v.isEmpty ? nil : v }
                }))
        }

        out.append(UI.titledRow(spec("schemas.openStore"), lang: lang,
            control: UI.button(T("打開方案市集", "打开方案市集", "Open the store")[lang]) {
                [weak self] in self?.go(to: "store")
            }))
        return out
    }

    /// 輸入法本體要求開在某一頁、順便重新整理。
    func apply(command: (page: String?, redeploy: Bool)) {
        if let page = command.page { go(to: page) }
        if command.redeploy { redeploy() }
    }

    private func go(to pageId: String) {
        guard let i = SettingsCatalog.pages.firstIndex(where: { $0.id == pageId }) else { return }
        sidebar.selectRowIndexes(IndexSet(integer: i), byExtendingSelection: false)
    }

    // MARK: - 外觀

    private func appearanceBody() -> [NSView] {
        var out: [NSView] = []
        let s = model.settings

        // 配色:掃 themes 目錄,取家族名。
        let families = themeFamilies()
        let titles = [T("預設", "默认", "Default")[lang]] + families.map(\.1)
        let values = [""] + families.map(\.0)
        out.append(UI.titledRow(spec("appearance.themeFamily"), lang: lang,
            control: ActionPopUp.make(titles: titles, values: values,
                                      selected: s.themeFamily ?? "") { [weak self] v in
                self?.model.edit { $0.themeFamily = v.isEmpty ? nil : v }
            }))

        out.append(choiceRow("appearance.appearance", selected: s.appearance.rawValue) {
            [weak self] v in
            self?.model.edit { $0.appearance = AppearanceMode(rawValue: v) ?? .followSystem }
        })
        out.append(choiceRow("appearance.candidateScale", selected: s.candidateScale.rawValue) {
            [weak self] v in
            self?.model.edit { $0.candidateScale = CandidateScale(rawValue: v) ?? .medium }
        })

        // 候選數:這一項會動 default.custom.yaml,所以要重新整理字詞。
        let current = RimeConfigPatch.readPageSize(userDir: SettingsPaths.userDir) ?? 5
        out.append(choiceRow("appearance.candidateCount", selected: String(current)) {
            [weak self] v in
            guard let self, let n = Int(v) else { return }
            self.model.run(T("套用候選字數", "套用候选字数", "Applying candidates per page")) { p in
                self.model.engine.setPageSize(n, progress: p)
            }
        })

        out.append(choiceRow("appearance.orientation", selected: s.candidateOrientation.rawValue) {
            [weak self] v in
            self?.model.edit { $0.candidateOrientation = OrientationPref(rawValue: v) ?? .followTheme }
        })
        out.append(choiceRow("appearance.showLabels", selected: s.showCandidateLabels.rawValue) {
            [weak self] v in
            self?.model.edit { $0.showCandidateLabels = ThemeBoolPref(rawValue: v) ?? .followTheme }
        })
        out.append(choiceRow("appearance.showStatusBar", selected: s.showStatusBar.rawValue) {
            [weak self] v in
            self?.model.edit { $0.showStatusBar = ThemeBoolPref(rawValue: v) ?? .followTheme }
        })
        return out
    }

    /// 主題家族。深淺是另一個控制項,所以清單裡不出現「淺色 / 深色」兩份。
    private func themeFamilies() -> [(String, String)] {
        let dirs = [SettingsPaths.userDir.appendingPathComponent("themes"),
                    SettingsPaths.themesDir]
        var seen: [String: String] = [:]
        var order: [String] = []
        for dir in dirs {
            for name in ((try? FileManager.default.contentsOfDirectory(atPath: dir.path)) ?? []).sorted() {
                guard name.hasSuffix(".yaml") else { continue }
                let id = String(name.dropLast(5))
                let family = id.hasSuffix("-dark") ? String(id.dropLast(5))
                    : (id.hasSuffix("-light") ? String(id.dropLast(6)) : id)
                if seen[family] == nil {
                    seen[family] = family
                    order.append(family)
                }
            }
        }
        return order.map { ($0, $0) }
    }

    private func choiceRow(_ id: String, selected: String,
                           action: @escaping (String) -> Void) -> NSView {
        let s = spec(id)
        guard case .choice(let choices) = s.kind else { return NSView() }
        return UI.titledRow(s, lang: lang,
                            control: UI.segmented(choices, lang: lang, selected: selected,
                                                  action: action))
    }

    // MARK: - 文字

    private func textBody() -> [NSView] {
        let s = model.settings
        return [
            choiceRow("text.variant", selected: s.variant.rawValue) { [weak self] v in
                self?.model.edit { $0.variant = VariantPref(rawValue: v) ?? .followInputMode }
            },
            choiceRow("text.punctuation", selected: s.punctuation.rawValue) { [weak self] v in
                self?.model.edit { $0.punctuation = PunctuationPref(rawValue: v) ?? .followSchema }
            },
            choiceRow("text.shape", selected: s.shape.rawValue) { [weak self] v in
                self?.model.edit { $0.shape = ShapePref(rawValue: v) ?? .followSchema }
            },
            UI.notice(title: T("改了立刻生效", "改了立刻生效", "Takes effect immediately")[lang],
                      body: T("這三項不需要重新整理字詞。切到別的 app 再切回來就會套用。",
                              "这三项不需要重新整理字词。切到别的 app 再切回来就会套用。",
                              "These three do not need a rebuild. Switch away and back and they apply.")[lang]),
        ]
    }

    // MARK: - 自己加的詞

    private var newWordField: NSTextField?
    private var newCodeField: NSTextField?

    private func dictionaryBody() -> [NSView] {
        var out: [NSView] = []
        out.append(UI.label(spec("dictionary.list").blurb[lang], size: 11.5,
                            colour: .secondaryLabelColor))

        let word = NSTextField(string: "")
        word.placeholderString = T("詞,例如:黃小明", "词,例如:黄小明", "Word, e.g. Anthropic")[lang]
        let code = NSTextField(string: "")
        // ⚠ 這個範例本來寫的是「huang xiao ming」—— **帶空格的範例是錯的**,
        //   照著填的每一個詞都永遠打不出來。這一頁被拿下來一輪就是為了這件事。
        code.placeholderString = T("怎麼打,例如:huangxiaoming",
                                   "怎么打,例如:huangxiaoming",
                                   "How you type it, e.g. anthropic")[lang]
        word.setContentHuggingPriority(.defaultLow, for: .horizontal)
        code.setContentHuggingPriority(.defaultLow, for: .horizontal)
        newWordField = word
        newCodeField = code

        let add = UI.button(T("加這個詞", "加这个词", "Add")[lang]) { [weak self] in
            self?.addPhrase()
        }
        let entry = NSStackView(views: [word, code, add])
        entry.orientation = .horizontal
        entry.spacing = 8
        entry.distribution = .fillProportionally
        out.append(UI.card([entry]))

        if model.phrases.isEmpty {
            out.append(UI.notice(
                title: T("還沒有加過詞", "还没有加过词", "No words yet")[lang],
                body: T("上面加一個試試。加完之後要按「重新整理字詞」才會生效 —— 或是等下一次重新整理。",
                        "上面加一个试试。加完之后要按「重新整理字词」才会生效 —— 或是等下一次重新整理。",
                        "Add one above. New words take effect after a rebuild.")[lang]))
        } else {
            var rows: [NSView] = []
            for p in model.phrases.prefix(200) {
                let text = UI.label("\(p.text)    —    \(p.code)", size: 13)
                let del = UI.button(T("刪除", "删除", "Delete")[lang]) { [weak self] in
                    guard let self else { return }
                    self.model.phrases = UserPhrases.removing(identity: p.identity,
                                                              from: self.model.phrases)
                    try? UserPhrases.write(self.model.phrases, userDir: SettingsPaths.userDir)
                    self.refresh()
                }
                let line = NSStackView(views: [text, del])
                line.orientation = .horizontal
                line.spacing = 10
                rows.append(line)
            }
            if model.phrases.count > 200 {
                rows.append(UI.label(T("(只顯示前 200 筆,共 \(model.phrases.count) 筆)",
                                       "(只显示前 200 笔,共 \(model.phrases.count) 笔)",
                                       "(showing the first 200 of \(model.phrases.count))")[lang],
                                     size: 11.5, colour: .secondaryLabelColor))
            }
            out.append(UI.card(rows))
        }

        out.append(UI.titledRow(spec("dictionary.export"), lang: lang,
            control: UI.button(T("匯出…", "导出…", "Export…")[lang]) { [weak self] in
                self?.exportPhrases()
            }))
        out.append(UI.titledRow(spec("dictionary.import"), lang: lang,
            control: UI.button(T("匯入…", "导入…", "Import…")[lang]) { [weak self] in
                self?.importPhrases()
            }))

        let rebuild = UI.button(T("現在重新整理字詞", "现在重新整理字词", "Rebuild now")[lang]) {
            [weak self] in self?.redeploy()
        }
        out.append(UI.card([
            UI.label(T("加詞、刪詞之後要重新整理才會生效。",
                       "加词、删词之后要重新整理才会生效。",
                       "Added or removed words take effect after a rebuild.")[lang],
                     size: 12),
            rebuild,
        ]))
        return out
    }

    /// 找方案檔的順序必須與 librime 一致:先使用者目錄,再隨附目錄。
    private var schemaSearchDirs: [URL] { [SettingsPaths.userDir, SettingsPaths.sharedDir] }

    /// 讓每一個已啟用的方案都讀得到使用者加的詞。
    ///
    /// ⚠ 加詞與匯入**兩條路都要走這裡**。只有加詞走的話,一個從別台機器
    /// 匯入了一整份詞、自己一個都沒加過的使用者,會拿到一頁滿滿的詞
    /// 和一個什麼都打不出來的鍵盤。
    private func mountPhrasesToEnabledSchemas() {
        for id in model.engine.enabledSchemas {
            _ = UserPhrases.mount(schemaId: id, userDir: SettingsPaths.userDir,
                                  searchDirs: schemaSearchDirs)
        }
    }

    private func addPhrase() {
        guard let w = newWordField?.stringValue.trimmingCharacters(in: .whitespaces),
              let c = newCodeField?.stringValue.trimmingCharacters(in: .whitespaces),
              !w.isEmpty, !c.isEmpty else {
            alert(T("兩欄都要填", "两栏都要填", "Both fields are required")[lang],
                  T("左邊是你要打出來的字,右邊是你會怎麼打它。",
                    "左边是你要打出来的字,右边是你会怎么打它。",
                    "The left box is the word; the right box is how you type it.")[lang])
            return
        }
        // ⚠ **擋在這裡,不要等到使用者回去打字才發現。**
        // 最常見的是兩欄填反(把「黃小明」填進右邊)。那一筆會安靜地存進去、
        // 出現在下面的清單裡、然後永遠不會被打出來 —— 而使用者只會覺得
        // 是自己哪裡做錯了。
        if let problem = UserPhrases.codeProblem(c) {
            alert(T("右邊那一欄填不對", "右边那一栏填不对",
                    "That won't work as a spelling")[lang], problem[lang])
            return
        }
        let (list, _) = UserPhrases.adding(UserPhrase(text: w, code: c), to: model.phrases)
        model.phrases = list
        try? UserPhrases.write(list, userDir: SettingsPaths.userDir)
        mountPhrasesToEnabledSchemas()
        newWordField?.stringValue = ""
        newCodeField?.stringValue = ""
        refresh()
    }

    private func exportPhrases() {
        let panel = NSSavePanel()
        panel.nameFieldStringValue = UserPhrases.fileName
        panel.allowedContentTypes = [.plainText]
        guard panel.runModal() == .OK, let url = panel.url else { return }
        do {
            try UserPhrases.serialise(model.phrases).write(to: url, atomically: true,
                                                           encoding: .utf8)
        } catch {
            alert(T("匯出失敗", "导出失败", "Export failed")[lang], "\(error)")
        }
    }

    private func importPhrases() {
        let panel = NSOpenPanel()
        panel.allowsMultipleSelection = false
        panel.allowedContentTypes = [.plainText, .text]
        guard panel.runModal() == .OK, let url = panel.url,
              let text = try? String(contentsOf: url, encoding: .utf8) else { return }
        let parsed = UserPhrases.parse(text)
        let merged = UserPhrases.merging(parsed.phrases, into: model.phrases)
        model.phrases = merged.list
        try? UserPhrases.write(merged.list, userDir: SettingsPaths.userDir)
        mountPhrasesToEnabledSchemas()
        refresh()
        alert(T("匯入完成", "导入完成", "Import finished")[lang],
              T("新增 \(merged.added) 筆,更新 \(merged.updated) 筆,略過 \(merged.skipped) 筆。"
                + (parsed.problems.isEmpty ? "" : "有 \(parsed.problems.count) 行讀不懂,已跳過。"),
                "新增 \(merged.added) 笔,更新 \(merged.updated) 笔,略过 \(merged.skipped) 笔。"
                + (parsed.problems.isEmpty ? "" : "有 \(parsed.problems.count) 行读不懂,已跳过。"),
                "\(merged.added) added, \(merged.updated) updated, \(merged.skipped) skipped."
                + (parsed.problems.isEmpty ? "" : " \(parsed.problems.count) line(s) could not be read."))[lang])
    }

    // MARK: - 方案市集

    private func storeBody() -> [NSView] {
        var out: [NSView] = []

        guard model.settings.networkEnabled else {
            out.append(UI.notice(
                title: T("要下載方案需要先開啟連網", "要下载方案需要先开启联网",
                         "Downloading needs networking turned on")[lang],
                body: T("這個輸入法預設完全不連網。開啟之後,每一次連線都會記在「連網」那一頁,你可以自己看。",
                        "这个输入法默认完全不联网。开启之后,每一次连线都会记在「联网」那一页,你可以自己看。",
                        "This input method is fully offline by default. Once on, every connection is listed on the Networking page.")[lang],
                actionTitle: T("開啟連網", "开启联网", "Turn networking on")[lang],
                action: { [weak self] in
                    self?.model.edit { $0.networkEnabled = true }
                    self?.model.loadIndex()
                    self?.refresh()
                }))
            return out
        }

        if model.loadingIndex {
            out.append(UI.notice(title: T("正在取得清單…", "正在获取清单…", "Fetching the list…")[lang],
                                 body: model.indexURL))
            return out
        }
        if let err = model.indexError {
            out.append(UI.notice(
                title: T("拿不到方案清單", "拿不到方案清单", "Could not fetch the list")[lang],
                body: err,
                actionTitle: T("再試一次", "再试一次", "Try again")[lang],
                action: { [weak self] in self?.model.loadIndex() }))
            return out
        }
        guard let index = model.index else {
            out.append(UI.notice(title: T("還沒有取得清單", "还没有获取清单",
                                          "The list has not been fetched")[lang],
                                 body: model.indexURL,
                                 actionTitle: T("取得清單", "获取清单", "Fetch the list")[lang],
                                 action: { [weak self] in self?.model.loadIndex() }))
            return out
        }

        let installed = model.engine.registry.ids
        let enabled = Set(model.engine.enabledSchemas)
        for category in index.visibleCategories() {
            let packages = index.packages(in: category.id).filter { !$0.isComponentOnly }
            guard !packages.isEmpty else { continue }
            out.append(UI.label(category.name, size: 15, weight: .semibold))
            var rows: [NSView] = []
            for pkg in packages {
                let isInstalled = installed.contains(pkg.id)
                let isEnabled = pkg.schemaIds.contains { enabled.contains($0) }
                var subtitle = pkg.description
                if subtitle.isEmpty { subtitle = pkg.schemaIds.joined(separator: ", ") }
                if pkg.size > 0 { subtitle += "  ·  \(formatBytes(pkg.size))" }
                if let note = pkg.layoutNote { subtitle += "  ·  \(note)" }

                let text = NSStackView(views: [
                    UI.label(pkg.name, size: 13, weight: .medium),
                    UI.label(subtitle, size: 11.5, colour: .secondaryLabelColor),
                ])
                text.orientation = .vertical
                text.alignment = .leading
                text.spacing = 2

                let actionTitle: String
                if isEnabled {
                    actionTitle = T("已啟用", "已启用", "Enabled")[lang]
                } else if isInstalled {
                    actionTitle = T("啟用", "启用", "Enable")[lang]
                } else {
                    actionTitle = T("下載並啟用", "下载并启用", "Download and enable")[lang]
                }
                let btn = UI.button(actionTitle) { [weak self] in
                    self?.install(pkg: pkg, index: index, alreadyInstalled: isInstalled)
                }
                btn.isEnabled = !isEnabled

                let line = NSStackView(views: [text, btn])
                line.orientation = .horizontal
                line.spacing = 12
                rows.append(line)
            }
            out.append(UI.card(rows))
        }

        let field = NSTextField(string: model.indexURL)
        field.placeholderString = SettingsModel.defaultIndexURL
        let save = UI.button(T("換成這個位址", "换成这个地址", "Use this address")[lang]) {
            [weak self] in
            let v = field.stringValue.trimmingCharacters(in: .whitespaces)
            self?.model.edit { $0.storeIndexUrl = v.isEmpty ? nil : v }
            self?.model.index = nil
            self?.model.loadIndex()
        }
        let reset = UI.button(T("恢復預設", "恢复默认", "Reset")[lang]) { [weak self] in
            self?.model.edit { $0.storeIndexUrl = nil }
            self?.model.index = nil
            self?.model.loadIndex()
        }
        let urlRow = NSStackView(views: [field, save, reset])
        urlRow.orientation = .horizontal
        urlRow.spacing = 8
        out.append(UI.titledRow(spec("store.indexUrl"), lang: lang, control: urlRow))
        return out
    }

    private func install(pkg: StorePackage, index: SchemaIndex, alreadyInstalled: Bool) {
        let installed = model.engine.registry.ids
        switch DependencyResolver.resolve(index: index, selected: [pkg.id], installed: installed) {
        case .unknownPackage(let id):
            alert(T("清單裡沒有這個套件", "清单里没有这个套件", "Not in the list")[lang], id)
        case .missingDependency(let missing, let requiredBy):
            alert(T("缺少相依套件", "缺少依赖套件", "Missing dependency")[lang],
                  "\(requiredBy) → \(missing)")
        case .ok(let plan):
            let ids = pkg.schemaIds
            model.run(T("安裝「\(pkg.name)」", "安装「\(pkg.name)」", "Installing \(pkg.name)")) {
                [weak self] progress in
                guard let self else {
                    return StoreOutcome(ok: false, message: T("已取消", "已取消", "Cancelled"))
                }
                if plan.count > 0 {
                    let r = self.model.engine.install(indexURL: self.model.indexURL,
                                                      index: index, plan: plan,
                                                      progress: progress)
                    guard r.ok else { return r }
                }
                guard !ids.isEmpty else {
                    return StoreOutcome(ok: true,
                                        message: T("已安裝(這是基礎元件,沒有可啟用的方案)",
                                                   "已安装(这是基础组件,没有可启用的方案)",
                                                   "Installed (a component package with no schema)"))
                }
                return self.model.engine.setEnabled(ids, enabled: true, progress: progress)
            }
        }
    }

    // MARK: - 連網

    private func networkBody() -> [NSView] {
        var out: [NSView] = []
        out.append(UI.titledRow(spec("network.enabled"), lang: lang,
            control: UI.checkbox(T("允許連網", "允许联网", "Allow networking")[lang],
                                 on: model.settings.networkEnabled) { [weak self] on in
                self?.model.edit { $0.networkEnabled = on }
                self?.refresh()
            }))

        model.logFile.trim()
        let entries = model.logFile.read().reversed()
        out.append(UI.label(spec("network.log").blurb[lang], size: 11.5,
                            colour: .secondaryLabelColor))
        if entries.isEmpty {
            out.append(UI.card([
                UI.label(T("空的 —— 這個輸入法從來沒有連過網路。",
                           "空的 —— 这个输入法从来没有连过网络。",
                           "Empty — this input method has never connected to anything.")[lang],
                         size: 13, weight: .medium),
            ]))
        } else {
            let fmt = DateFormatter()
            fmt.dateStyle = .short
            fmt.timeStyle = .medium
            var rows: [NSView] = []
            for e in entries.prefix(200) {
                var line = "\(fmt.string(from: e.at))   \(e.host)   \(e.purpose.label[lang])"
                if !e.label.isEmpty { line += "(\(e.label))" }
                switch e.outcome {
                case .ok: line += "   ✓ \(formatBytes(e.bytes))"
                case .redirected: line += "   → \(e.detail)"
                case .failed: line += "   ✗ \(e.detail)"
                }
                rows.append(UI.label(line, size: 11.5))
            }
            out.append(UI.card(rows))
            out.append(UI.button(T("清除紀錄", "清除记录", "Clear the log")[lang]) { [weak self] in
                self?.model.logFile.clear()
                self?.refresh()
            })
        }
        return out
    }

    // MARK: - 進階

    private func advancedBody() -> [NSView] {
        var out: [NSView] = []

        out.append(UI.titledRow(spec("advanced.redeploy"), lang: lang,
            control: UI.button(T("重新整理字詞", "重新整理字词", "Rebuild now")[lang]) {
                [weak self] in self?.redeploy()
            }))

        out.append(UI.titledRow(spec("advanced.import"), lang: lang,
            control: UI.button(T("選一個檔案…", "选一个文件…", "Choose a file…")[lang]) {
                [weak self] in self?.importArchive()
            }))

        out.append(choiceRow("advanced.language", selected: model.settings.uiLanguage.rawValue) {
            [weak self] v in
            self?.model.edit { $0.uiLanguage = UiLanguage(rawValue: v) ?? .system }
            self?.refresh()
        })

        let resetSpec = spec("advanced.reset")
        let resetButton = UI.button(T("全部回復預設", "全部恢复默认", "Reset everything")[lang]) {
            [weak self] in
            guard let self else { return }
            let a = NSAlert()
            a.messageText = resetSpec.title[self.lang]
            a.informativeText = resetSpec.blurb[self.lang]
            a.addButton(withTitle: T("回復預設", "恢复默认", "Reset")[self.lang])
            a.addButton(withTitle: T("取消", "取消", "Cancel")[self.lang])
            guard a.runModal() == .alertFirstButtonReturn else { return }
            self.model.store.resetKeepingFacts()
            self.model.edit { _ in }
            self.refresh()
        }
        // 沒有東西可以回復時按鈕是灰的 —— 一顆按了什麼都不會發生的按鈕
        // 比沒有這顆按鈕更糟。
        resetButton.isEnabled = !model.store.isPristine
        out.append(UI.titledRow(resetSpec, lang: lang, control: resetButton))

        let report = diagnostics()
        let text = NSTextView()
        text.string = report
        text.isEditable = false
        text.font = .monospacedSystemFont(ofSize: 11, weight: .regular)
        text.drawsBackground = false
        let scroll = NSScrollView()
        scroll.documentView = text
        scroll.hasVerticalScroller = true
        scroll.drawsBackground = false
        scroll.translatesAutoresizingMaskIntoConstraints = false
        scroll.heightAnchor.constraint(equalToConstant: 190).isActive = true

        out.append(UI.titledRow(spec("advanced.diagnostics"), lang: lang, control: scroll))
        out.append(UI.button(T("複製", "复制", "Copy")[lang]) {
            NSPasteboard.general.clearContents()
            NSPasteboard.general.setString(report, forType: .string)
        })
        return out
    }

    /// **永遠是英文**,與 Android 端同一個決定 —— 這不是介面文字,
    /// 是一份要被貼進 issue 的回報載荷。
    private func diagnostics() -> String {
        let s = model.settings
        var out: [String] = []
        out.append("settings app: \(Bundle.main.bundleIdentifier ?? "?") "
                   + "\(Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "?")")
        out.append("host app: \(SettingsPaths.hostAppURL?.path ?? "NOT FOUND")")
        out.append("user dir: \(SettingsPaths.userDir.path)")
        out.append("shared dir: \(SettingsPaths.sharedDir.path) "
                   + "(exists: \(FileManager.default.fileExists(atPath: SettingsPaths.sharedDir.path)))")
        out.append("ui language: \(s.uiLanguage.rawValue) → \(lang.rawValue)")
        out.append("network: \(s.networkEnabled ? "on" : "off")")
        out.append("follow input mode: \(s.followInputMode)")
        out.append("pinned: global=\(s.pinnedSchemaId ?? "-") "
                   + "hant=\(s.pinnedSchemaHant ?? "-") hans=\(s.pinnedSchemaHans ?? "-")")
        out.append("page_size: \(RimeConfigPatch.readPageSize(userDir: SettingsPaths.userDir).map(String.init) ?? "default")")
        out.append("")
        let installed = model.installedSchemas
        out.append("installed schemas (\(installed.count))")
        for i in installed {
            out.append("  \(i.id)  —  \(i.name)  [\(i.isBuiltin ? "builtin" : "user")] "
                       + "\(i.languageTag ?? "no-lang") \(SchemaScript.of(id: i.id, languageTag: i.languageTag).rawValue)")
        }
        out.append("")
        let enabled = model.engine.enabledSchemas
        out.append("schema_list (\(enabled.count))")
        for e in enabled { out.append("  \(e)") }
        out.append("")
        out.append("store packages (\(model.engine.registry.all.count))")
        for p in model.engine.registry.all { out.append("  \(p.id)  \(p.source)  \(p.files.count) files") }
        out.append("")
        out.append("user phrases: \(model.phrases.count)")
        out.append("network log: \(model.logFile.url.path)")
        return out.joined(separator: "\n")
    }

    private func redeploy() {
        model.run(T("重新整理字詞", "重新整理字词", "Rebuilding dictionaries")) { [weak self] p in
            guard let self else {
                return StoreOutcome(ok: false, message: T("已取消", "已取消", "Cancelled"))
            }
            return self.model.engine.redeploy(progress: p)
        }
    }

    private func importArchive() {
        let panel = NSOpenPanel()
        panel.allowsMultipleSelection = false
        if let zip = UTType(filenameExtension: "zip") {
            panel.allowedContentTypes = [zip, .yaml, .plainText]
        }
        guard panel.runModal() == .OK, let url = panel.url else { return }

        if url.pathExtension.lowercased() == "zip" {
            model.run(T("匯入", "导入", "Importing")) { [weak self] progress in
                guard let self else {
                    return StoreOutcome(ok: false, message: T("已取消", "已取消", "Cancelled"))
                }
                progress(.extracting(name: url.lastPathComponent))
                switch ArchiveGuard.extract(url, to: SettingsPaths.userDir,
                                            stagingParent: SettingsPaths.workDir) {
                case .rejected(let report):
                    return StoreOutcome(
                        ok: false,
                        message: T("這個檔案沒有通過安全檢查,已整包拒絕",
                                   "这个文件没有通过安全检查,已整包拒绝",
                                   "This file failed the safety checks and was rejected"),
                        details: report.rejections.map { $0.message.hant })
                case .failed(let m):
                    return StoreOutcome(ok: false,
                                        message: T("匯入失敗:\(m)", "导入失败:\(m)",
                                                   "Import failed: \(m)"))
                case .ok(let files, _):
                    let ids = files.filter { $0.hasSuffix(SchemaCatalog.suffix) }
                        .map { String($0.dropLast(SchemaCatalog.suffix.count)) }
                        .map { $0.components(separatedBy: "/").last ?? $0 }
                    guard !ids.isEmpty else {
                        return StoreOutcome(ok: true,
                                            message: T("已匯入 \(files.count) 個檔案(裡面沒有方案)",
                                                       "已导入 \(files.count) 个文件(里面没有方案)",
                                                       "Imported \(files.count) file(s), no schema inside"))
                    }
                    return self.model.engine.setEnabled(ids, enabled: true, progress: progress)
                }
            }
        } else {
            // 單一 YAML:直接複製進使用者目錄,檔名先過同一套檢查。
            let name = url.lastPathComponent
            if let problem = ArchiveGuard.pathProblem(name)
                ?? ArchiveGuard.extensionProblem(name) {
                alert(T("這個檔名不能匯入", "这个文件名不能导入", "That filename is not allowed")[lang],
                      problem)
                return
            }
            let dest = SettingsPaths.userDir.appendingPathComponent(name)
            do {
                if FileManager.default.fileExists(atPath: dest.path) {
                    try FileManager.default.removeItem(at: dest)
                }
                try FileManager.default.copyItem(at: url, to: dest)
                redeploy()
            } catch {
                alert(T("匯入失敗", "导入失败", "Import failed")[lang], "\(error)")
            }
        }
    }

    // MARK: - 結果回饋

    /// 成功 → 短暫提示;失敗 → 停在對話框上。
    ///
    /// 兩者刻意走不同的通道:使用者按「重新整理」的目的是**完成它**,
    /// 不是讀一份報告;成功之後再彈一個要按「知道了」的對話框,
    /// 等於在他已經達成目的之後多收一次過路費。失敗則相反 ——
    /// 訊息裡有他需要採取行動的指示,那種東西不能三秒後自己消失。
    private func present(_ outcome: StoreOutcome) {
        if outcome.ok {
            toast(outcome.message[lang])
        } else {
            alert(outcome.message[lang], outcome.details.joined(separator: "\n"))
        }
    }

    private var toastField: NSTextField?

    private func toast(_ text: String) {
        toastField?.removeFromSuperview()
        guard let root = window?.contentView else { return }
        let f = UI.label(text, size: 12.5, weight: .medium)
        f.wantsLayer = true
        f.drawsBackground = true
        f.backgroundColor = .controlAccentColor.withAlphaComponent(0.16)
        f.translatesAutoresizingMaskIntoConstraints = false
        root.addSubview(f, positioned: .above, relativeTo: nil)
        NSLayoutConstraint.activate([
            f.centerXAnchor.constraint(equalTo: root.centerXAnchor),
            f.bottomAnchor.constraint(equalTo: root.bottomAnchor, constant: -20),
            f.widthAnchor.constraint(lessThanOrEqualTo: root.widthAnchor, multiplier: 0.8),
        ])
        toastField = f
        DispatchQueue.main.asyncAfter(deadline: .now() + 5) { [weak f] in f?.removeFromSuperview() }
    }

    private func alert(_ title: String, _ body: String) {
        let a = NSAlert()
        a.messageText = title
        a.informativeText = body
        a.addButton(withTitle: T("知道了", "知道了", "OK")[lang])
        a.runModal()
    }
}
