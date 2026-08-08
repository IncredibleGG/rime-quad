//
//  CandidatePanel.swift — 懸浮候選窗（NSPanel）
//
//  ⚠ CI 驗不到這個檔案裡的任何一行。macos runner 沒有登入的圖形工作階段，
//    NSPanel 建得起來也不會出現在畫面上。所以這裡的紀律是：
//    **凡是算得出來的都不要留在這裡**，全部推到 RimeQuadKit（純函式、有測試）。
//    這個檔案只剩下三件事：量字、把 Kit 算出來的座標畫出來、把窗擺對位置。
//
//  為什麼不用 IMKCandidates：它只認得自己那一套外觀，`candidates.window`
//  的顏色、字級、多欄排版一項都套不上去 —— 而「一套主題四端共用」正是這個
//  專案的主張。自繪的代價是要自己處理位置與 VoiceOver，收益是主題真的有用。
//

import AppKit
import InputMethodKit

final class CandidatePanel {

    private let panel: NSPanel
    private let view: CandidateView

    init() {
        view = CandidateView(frame: NSRect(x: 0, y: 0, width: 200, height: 40))
        panel = NSPanel(contentRect: view.frame,
                        styleMask: [.borderless, .nonactivatingPanel],
                        backing: .buffered, defer: false)
        panel.isFloatingPanel = true
        panel.level = .popUpMenu
        panel.hasShadow = false          // 陰影由主題畫，見 CandidateView
        panel.isOpaque = false
        panel.backgroundColor = .clear
        panel.hidesOnDeactivate = false
        panel.worksWhenModal = true
        // 輸入法的窗不得搶焦點，也不得跟著 Space 切換而消失。
        panel.collectionBehavior = [.canJoinAllSpaces, .stationary, .fullScreenAuxiliary,
                                    .ignoresCycle]
        panel.ignoresMouseEvents = false
        panel.contentView = view
        panel.setAccessibilityRole(.window)
        panel.setAccessibilitySubrole(.floatingWindow)
    }

    var onSelect: ((Int) -> Void)? {
        get { view.onSelect }
        set { view.onSelect = newValue }
    }

    func hide() {
        panel.orderOut(nil)
    }

    /// 更新內容並擺位。`caretRect` 是宿主 app 回報的插入點（螢幕座標）。
    func show(theme: Theme, snapshot: RimeSnapshot, caretRect: NSRect) {
        let size = view.update(theme: theme, snapshot: snapshot)
        let frame = PanelPlacement.frame(for: size, caret: caretRect,
                                         window: theme.window,
                                         screens: NSScreen.screens.map(\.visibleFrame),
                                         fallback: NSScreen.main?.visibleFrame ?? .zero)
        panel.setFrame(frame, display: true)
        panel.alphaValue = CGFloat(theme.window.opacity)
        if !panel.isVisible { panel.orderFrontRegardless() }
        view.needsDisplay = true
    }
}

/// 擺位是純幾何，抽出來讓它至少能被人腦驗算（AppKit 型別讓它進不了單元測試 target）。
enum PanelPlacement {

    static func frame(for size: LayoutSize, caret: NSRect, window w: CandidateWindow,
                      screens: [NSRect], fallback: NSRect) -> NSRect {
        let width = CGFloat(size.width)
        let height = CGFloat(size.height)
        let screen = screens.first { $0.intersects(caret) } ?? fallback

        var x = caret.minX + CGFloat(w.offsetX)
        // AppKit 的 y 向上：`below` 是插入點下方，所以要往**小**的 y 走。
        var y: CGFloat
        let below = caret.minY - CGFloat(w.offsetY) - height
        let above = caret.maxY + CGFloat(w.offsetY)

        switch w.placement {
        case .below: y = below
        case .above: y = above
        case .auto:  y = below < screen.minY ? above : below
        }

        // 撞到螢幕邊緣就往回夾。翻面之後仍然出界時，貼邊優先於維持方向 ——
        // 一個看不見的候選窗比一個位置不理想的候選窗糟得多。
        if x + width > screen.maxX { x = screen.maxX - width }
        if x < screen.minX { x = screen.minX }
        if y < screen.minY { y = screen.minY }
        if y + height > screen.maxY { y = screen.maxY - height }
        return NSRect(x: x, y: y, width: width, height: height)
    }
}
