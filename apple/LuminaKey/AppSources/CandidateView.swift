//
//  CandidateView.swift — 候選窗的內容視圖
//
//  排版由 LuminaKeyKit 的 CandidateLayout 算（有單元測試），這裡只負責
//  「量一段文字有多寬」與「照座標畫出來」。
//
//  ⚠ 無障礙：候選項不是 NSButton，VoiceOver 摸不到自繪的矩形。所以每一項
//    都要有一個 NSAccessibilityElement 代理，而且**朗讀名要說它是什麼**
//    （「第 1 個候選，你好」），不是念一塊方格。
//    Android 端已經因為「念得出名字、輕點兩下卻沒反應」吃過一次虧，
//    所以這裡的代理同時實作 accessibilityPerformPress()。
//

import AppKit

final class CandidateView: NSView {

    private var theme = Theme(id: "default-light")
    private var snapshot = RimeSnapshot()
    private var layout = CandidateLayoutResult(rows: 0, columns: 0,
                                               contentSize: LayoutSize(width: 0, height: 0),
                                               items: [], overflowed: false)
    private var preeditHeight: CGFloat = 0
    private var statusHeight: CGFloat = 0
    private var statusSegments: [[FaceSegment]] = []
    private var a11yElements: [NSAccessibilityElement] = []

    var onSelect: ((Int) -> Void)?
    var onChangePage: ((PageStep) -> Void)?
    private var pager = ScrollPager()

    override var isFlipped: Bool { true }   // y 向下，與 CandidateLayout 一致

    // ───────────────────────── 更新 ─────────────────────────

    /// 回傳窗應有的尺寸。
    func update(theme: Theme, snapshot: RimeSnapshot) -> LayoutSize {
        self.theme = theme
        self.snapshot = snapshot

        let w = theme.window
        let measured = snapshot.candidates.enumerated().map { i, c in
            measureItem(index: i, candidate: c, window: w)
        }
        layout = CandidateLayout.layout(measured: measured, window: w)

        preeditHeight = theme.preedit.show && !snapshot.preedit.isEmpty
            ? ceil(attributed(snapshot.preedit, size: theme.preedit.size,
                              color: theme.preedit.color, font: "preedit").size().height)
              + CGFloat(theme.preedit.paddingV) * 2
            : 0

        statusSegments = StatusFace.renderable(theme.statusBar.items).map {
            StatusFace.segments(for: $0, status: snapshot.status,
                                pageNo: snapshot.pageNo, isLastPage: snapshot.isLastPage)
        }.filter { !$0.isEmpty }
        statusHeight = (theme.statusBar.show && !statusSegments.isEmpty)
            ? max(CGFloat(theme.statusBar.height),
                  ceil(CGFloat(theme.statusBar.size) * 1.4) + CGFloat(theme.statusBar.paddingV) * 2)
            : 0

        var size = CandidateLayout.windowSize(content: layout.contentSize, window: w)
        size.height += Double(preeditHeight + statusHeight)
        rebuildAccessibility()
        return size
    }

    // ───────────────────────── 量測 ─────────────────────────

    private func font(_ name: String, size: Double) -> NSFont {
        let stack = theme.typography.fonts[name] ?? FontStack()
        let scaled = size * theme.typography.effectiveScale()
        for family in stack.family {
            let resolved = FontAliases.resolve(family)
            if let f = NSFont(name: resolved, size: CGFloat(scaled)) { return f }
        }
        return NSFont.systemFont(ofSize: CGFloat(scaled))
    }

    private func attributed(_ s: String, size: Double, color: RGBA, font name: String)
        -> NSAttributedString {
        NSAttributedString(string: s, attributes: [
            .font: font(name, size: size),
            .foregroundColor: NSColor(rgba: color),
        ])
    }

    private func itemStrings(index: Int, candidate: RimeCandidate, highlighted: Bool)
        -> (label: NSAttributedString?, text: NSAttributedString, comment: NSAttributedString?) {
        let st = theme.window.style
        let label: NSAttributedString? = st.label.show
            ? attributed(Placeholders.candidateLabel(format: st.label.format,
                                                     label: candidate.label,
                                                     indexOnPage: index),
                         size: st.label.size,
                         color: highlighted ? st.label.highlightColor : st.label.color,
                         font: "label")
            : nil
        let text = attributed(candidate.text, size: st.text.size,
                              color: highlighted ? st.text.highlightColor : st.text.color,
                              font: "candidate")
        let comment: NSAttributedString? =
            (st.comment.show && st.comment.position != .hidden && !candidate.comment.isEmpty)
            ? attributed(candidate.comment, size: st.comment.size,
                         color: highlighted ? st.comment.highlightColor : st.comment.color,
                         font: "comment")
            : nil
        return (label, text, comment)
    }

    private func measureItem(index: Int, candidate: RimeCandidate,
                             window w: CandidateWindow) -> MeasuredItem {
        let (label, text, comment) = itemStrings(index: index, candidate: candidate,
                                                 highlighted: false)
        let st = w.style
        var width = text.size().width
        var height = text.size().height
        if let label { width += label.size().width + 2 }
        if let comment {
            if st.comment.position == .below {
                width = max(width, comment.size().width)
                height += comment.size().height
            } else {
                width += comment.size().width + 4
            }
        }
        return MeasuredItem(width: Double(ceil(width)) + st.item.paddingH * 2,
                            height: Double(ceil(height)) + st.item.paddingV * 2)
    }

    // ───────────────────────── 繪製 ─────────────────────────

    override func draw(_ dirtyRect: NSRect) {
        let w = theme.window
        let r = bounds
        let radius = CGFloat(w.cornerRadius)
        let path = NSBezierPath(roundedRect: r.insetBy(dx: CGFloat(w.borderWidth) / 2,
                                                       dy: CGFloat(w.borderWidth) / 2),
                                xRadius: radius, yRadius: radius)
        NSColor(rgba: w.background).setFill()
        path.fill()
        if w.borderWidth > 0 {
            NSColor(rgba: w.borderColor).setStroke()
            path.lineWidth = CGFloat(w.borderWidth)
            path.stroke()
        }

        var top = CGFloat(w.padding)
        if theme.statusBar.show && theme.statusBar.position == .top && statusHeight > 0 {
            drawStatusBar(y: 0)
            top += statusHeight
        }
        if preeditHeight > 0 {
            drawPreedit(y: top + CGFloat(theme.preedit.paddingV))
            top += preeditHeight
        }
        drawCandidates(originY: top)
        if theme.statusBar.show && theme.statusBar.position == .bottom && statusHeight > 0 {
            drawStatusBar(y: r.height - statusHeight)
        }
    }

    private func drawPreedit(y: CGFloat) {
        let p = theme.preedit
        let s = attributed(snapshot.preedit, size: p.size, color: p.color, font: "preedit")
        s.draw(at: NSPoint(x: CGFloat(theme.window.padding) + CGFloat(p.paddingH), y: y))
    }

    private func drawCandidates(originY: CGFloat) {
        let w = theme.window
        let st = w.style
        let ox = CGFloat(w.padding)
        for placed in layout.items {
            let c = snapshot.candidates[placed.index]
            let highlighted = placed.index == snapshot.highlighted
            let rect = NSRect(x: ox + CGFloat(placed.frame.x),
                              y: originY + CGFloat(placed.frame.y),
                              width: CGFloat(placed.frame.width),
                              height: CGFloat(placed.frame.height))

            let bg = highlighted ? st.item.highlightBackground : st.item.background
            if bg.a > 0 {
                let p = NSBezierPath(roundedRect: rect,
                                     xRadius: CGFloat(st.item.cornerRadius),
                                     yRadius: CGFloat(st.item.cornerRadius))
                NSColor(rgba: bg).setFill()
                p.fill()
            }
            let borderW = highlighted ? st.item.highlightBorderWidth : st.item.borderWidth
            if borderW > 0 {
                let p = NSBezierPath(roundedRect: rect,
                                     xRadius: CGFloat(st.item.cornerRadius),
                                     yRadius: CGFloat(st.item.cornerRadius))
                NSColor(rgba: highlighted ? st.item.highlightBorderColor : st.item.borderColor)
                    .setStroke()
                p.lineWidth = CGFloat(borderW)
                p.stroke()
            }

            let (label, text, comment) = itemStrings(index: placed.index, candidate: c,
                                                     highlighted: highlighted)
            var x = rect.minX + CGFloat(st.item.paddingH)
            let y = rect.minY + CGFloat(st.item.paddingV)
            if let label {
                label.draw(at: NSPoint(x: x, y: y + (text.size().height - label.size().height)))
                x += label.size().width + 2
            }
            text.draw(at: NSPoint(x: x, y: y))
            if let comment {
                if st.comment.position == .below {
                    comment.draw(at: NSPoint(x: x, y: y + text.size().height))
                } else {
                    comment.draw(at: NSPoint(x: x + text.size().width + 4,
                                             y: y + (text.size().height - comment.size().height)))
                }
            }
        }
    }

    private func drawStatusBar(y: CGFloat) {
        let sb = theme.statusBar
        let rect = NSRect(x: 0, y: y, width: bounds.width, height: statusHeight)
        if sb.background.a > 0 {
            NSColor(rgba: sb.background).setFill()
            rect.fill()
        }
        if sb.separator.show && sb.separator.width > 0 {
            NSColor(rgba: sb.separator.color).setFill()
            let lineY = sb.position == .top ? rect.maxY - CGFloat(sb.separator.width) : rect.minY
            NSRect(x: 0, y: lineY, width: bounds.width,
                   height: CGFloat(sb.separator.width)).fill()
        }

        let pieces: [NSAttributedString] = statusSegments.map { segs in
            let out = NSMutableAttributedString()
            for seg in segs {
                out.append(attributed(seg.text, size: sb.size,
                                      color: seg.emphasised ? sb.activeColor : sb.color,
                                      font: "ui"))
            }
            return out
        }
        let widths = pieces.map { $0.size().width }
        let total = widths.reduce(0, +) + CGFloat(sb.spacing) * CGFloat(max(0, pieces.count - 1))
        let inset = CGFloat(sb.paddingH)
        var x: CGFloat
        var gap = CGFloat(sb.spacing)
        switch sb.arrangement {
        case .leading: x = inset
        case .center: x = (bounds.width - total) / 2
        case .trailing: x = bounds.width - inset - total
        case .spaceBetween:
            x = inset
            if pieces.count > 1 {
                gap = max(CGFloat(sb.spacing),
                          (bounds.width - inset * 2 - widths.reduce(0, +)) / CGFloat(pieces.count - 1))
            }
        }
        let textY = y + (statusHeight - (pieces.first?.size().height ?? 0)) / 2
        for (i, piece) in pieces.enumerated() {
            piece.draw(at: NSPoint(x: x, y: textY))
            x += widths[i] + gap
        }
    }

    // ───────────────────────── 點選 ─────────────────────────

    override func mouseDown(with event: NSEvent) {
        let p = convert(event.locationInWindow, from: nil)
        if let idx = hitTestCandidate(p) { onSelect?(idx) }
    }

    /// 滾輪／觸控板翻頁。**「滾多少算一頁」在 LuminaKeyKit/CandidatePaging.swift**
    /// （純函式、有測試）—— NSEvent 在 CI 上造不出來,這裡只負責餵數字。
    ///
    /// 橫排的候選窗上,直向與橫向滾動都該翻頁:使用者在觸控板上會兩種都試。
    override func scrollWheel(with event: NSEvent) {
        let dy = Double(event.scrollingDeltaY)
        let delta = dy != 0 ? dy : Double(event.scrollingDeltaX)
        let step = pager.feed(delta: delta, isPrecise: event.hasPreciseScrollingDeltas)
        if step != .none { onChangePage?(step) }
    }

    func resetPaging() { pager.reset() }

    private func hitTestCandidate(_ p: NSPoint) -> Int? {
        let ox = CGFloat(theme.window.padding)
        let oy = CGFloat(theme.window.padding) + preeditHeight
            + (theme.statusBar.position == .top ? statusHeight : 0)
        for placed in layout.items {
            let rect = NSRect(x: ox + CGFloat(placed.frame.x), y: oy + CGFloat(placed.frame.y),
                              width: CGFloat(placed.frame.width),
                              height: CGFloat(placed.frame.height))
            if rect.contains(p) { return placed.index }
        }
        return nil
    }

    // ───────────────────────── 無障礙 ─────────────────────────

    private func rebuildAccessibility() {
        guard theme.accessibility.announceCandidates != .none else {
            a11yElements = []
            return
        }
        let ox = CGFloat(theme.window.padding)
        let oy = CGFloat(theme.window.padding) + preeditHeight
            + (theme.statusBar.position == .top ? statusHeight : 0)
        a11yElements = layout.items.map { placed in
            let c = snapshot.candidates[placed.index]
            let e = CandidateAccessibilityElement()
            e.index = placed.index
            e.owner = self
            e.setAccessibilityRole(.button)
            e.setAccessibilityLabel(
                theme.accessibility.announceCandidates == .textOnly
                    ? c.text
                    : Placeholders.announcement(format: theme.accessibility.candidateAnnouncement,
                                                label: c.label.isEmpty
                                                    ? String(placed.index + 1) : c.label,
                                                text: c.text, comment: c.comment))
            e.setAccessibilityParent(self)
            e.setAccessibilityFrameInParentSpace(
                NSRect(x: ox + CGFloat(placed.frame.x), y: oy + CGFloat(placed.frame.y),
                       width: CGFloat(placed.frame.width), height: CGFloat(placed.frame.height)))
            return e
        }
    }

    override func accessibilityChildren() -> [Any]? { a11yElements }
    override func accessibilityRole() -> NSAccessibility.Role? { .list }
    override func isAccessibilityElement() -> Bool { true }

    fileprivate func selectFromAccessibility(_ index: Int) { onSelect?(index) }
}

/// ⚠ `accessibilityPerformPress` 不是裝飾。VoiceOver 的「輕點兩下」送的是
///   無障礙的按下動作，**不會**變成 mouseDown —— 少了這個方法，候選項會是
///   「念得出名字、聚焦得到、按下去什麼都不會發生」的那一類鍵。
private final class CandidateAccessibilityElement: NSAccessibilityElement {
    var index: Int = 0
    weak var owner: CandidateView?

    override func accessibilityPerformPress() -> Bool {
        owner?.selectFromAccessibility(index)
        return true
    }
}

extension NSColor {
    convenience init(rgba: RGBA) {
        self.init(srgbRed: CGFloat(rgba.r) / 255, green: CGFloat(rgba.g) / 255,
                  blue: CGFloat(rgba.b) / 255, alpha: CGFloat(rgba.a) / 255)
    }
}

/// §8.4.1 的通用字體代號 → macOS 字體名。
enum FontAliases {
    static let map: [String: String] = [
        "$system": ".AppleSystemUIFont",
        "$serif": "New York",
        "$mono": "SF Mono",
        "$rounded": "SF Pro Rounded",
        "$emoji": "Apple Color Emoji",
        "$system-hant": "PingFang TC",
        "$system-hans": "PingFang SC",
        "$system-jpan": "Hiragino Sans",
        "$system-kore": "Apple SD Gothic Neo",
    ]

    /// §8.4.1：**未知代號必須被當作一般字體名處理**（而非報錯），以利未來新增。
    static func resolve(_ family: String) -> String { map[family] ?? family }
}
