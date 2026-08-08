//
//  CandidateLayout.swift — 候選窗的排版計算（docs/theme-format.md §8.6.7.1）
//
//  ── 為什麼這一段是純函式 ────────────────────────────────────────────────
//  CI 上沒有登入的圖形工作階段，候選窗**畫不出來也就驗不了**。所以「窗要多大、
//  每一格在哪」這件事必須能在沒有 NSPanel 的情況下算出來並被斷言。
//  輸入是量測結果（寬、高）與主題設定，輸出是純數字 —— AppKit 只負責量字與畫。
//
//  本檔實作的演算法就是規範 §8.6.7.1 逐條寫下來的那一份。兩邊改動必須同步，
//  Windows 端會照那一份寫 C++，算出來的數字要一樣。
//

import Foundation

public struct LayoutSize: Equatable, Sendable {
    public var width: Double
    public var height: Double
    public init(width: Double, height: Double) { self.width = width; self.height = height }
}

public struct LayoutRect: Equatable, Sendable {
    public var x: Double, y: Double, width: Double, height: Double
    public init(x: Double, y: Double, width: Double, height: Double) {
        self.x = x; self.y = y; self.width = width; self.height = height
    }
}

/// 一個候選項的量測結果（已含 `item.padding_h/​padding_v`）。
public struct MeasuredItem: Sendable {
    public var width: Double
    public var height: Double
    public init(width: Double, height: Double) { self.width = width; self.height = height }
}

public struct PlacedItem: Equatable, Sendable {
    public var index: Int
    public var row: Int
    public var column: Int
    public var frame: LayoutRect
    /// 該格的可用寬度小於量測寬度 —— 渲染端**必須**在這一格截斷並加上 `…`。
    public var truncated: Bool
}

public struct CandidateLayoutResult: Sendable {
    public var rows: Int
    public var columns: Int
    /// 候選格陣的尺寸（不含 window.padding）。
    public var contentSize: LayoutSize
    public var items: [PlacedItem]
    /// 排完之後仍然超出 `max_width` / `max_height`。
    public var overflowed: Bool
}

public enum CandidateLayout {

    /// §8.6.7.1 的排版演算法。
    ///
    /// - Parameters:
    ///   - measured: 依候選順序的量測結果。
    ///   - window: 主題的 `candidates.window`。
    /// - Returns: 每一格的位置（原點在內容區左上角，y 向下）。
    public static func layout(measured: [MeasuredItem], window w: CandidateWindow)
        -> CandidateLayoutResult {

        let n = measured.count
        if n == 0 {
            return CandidateLayoutResult(rows: 0, columns: 0,
                                         contentSize: LayoutSize(width: 0, height: 0),
                                         items: [], overflowed: false)
        }

        let itemHeight = measured.map(\.height).max() ?? 0
        let horizontal = w.style.orientation == .horizontal
        let availW = w.maxWidth > 0 ? Swift.max(0, w.maxWidth - 2 * w.padding) : Double.infinity
        let availH = w.maxHeight > 0 ? Swift.max(0, w.maxHeight - 2 * w.padding) : Double.infinity

        // ── 決定行數 L ──────────────────────────────────────────
        // `lines: 0` = 自動。自動的方向由 orientation 決定：
        //   horizontal 時 L 是**列數**，L 變大 → 欄變少 → 窄，所以往 max_width 收斂；
        //   vertical   時 L 是**欄數**，L 變大 → 列變少 → 矮，所以往 max_height 收斂。
        // 兩者都從 1 開始往上找第一個塞得下的；找不到就用 n（K=1）。
        var L: Int
        if w.lines >= 1 {
            L = Swift.min(w.lines, n)
        } else {
            let limit = horizontal ? availW : availH
            if limit.isInfinite {
                L = 1
            } else {
                L = n
                for candidate in 1...n {
                    let g = geometry(n: n, L: candidate, horizontal: horizontal)
                    let sz = contentSize(measured: measured, g: g, itemHeight: itemHeight, w: w)
                    let value = horizontal ? sz.width : sz.height
                    if value <= limit { L = candidate; break }
                }
            }
        }

        let g = geometry(n: n, L: L, horizontal: horizontal)
        var colWidths = columnWidths(measured: measured, g: g, equal: w.equalColumns)

        // ── max_width 的處置（§8.6.7.1「超出時」） ────────────────
        var overflowed = false
        let gaps = Double(Swift.max(0, g.columns - 1)) * w.columnGap
        var totalCols = colWidths.reduce(0, +)
        if availW.isFinite && totalCols + gaps > availW {
            overflowed = true
            if w.overflow == .shrink {
                let room = Swift.max(0, availW - gaps)
                if totalCols > 0 && room > 0 {
                    let scale = room / totalCols
                    let floorW = w.style.item.minWidth
                    colWidths = colWidths.map { Swift.max(floorW, $0 * scale) }
                    totalCols = colWidths.reduce(0, +)
                    // 夾底之後仍然超出就接受超出：**不得**把某一欄縮成 0，
                    // 那會產生一個看得見卻讀不到的候選。
                }
            }
        }

        // ── 逐格定位 ────────────────────────────────────────────
        var xs: [Double] = []
        var acc = 0.0
        for c in 0..<g.columns {
            xs.append(acc)
            acc += colWidths[c] + w.columnGap
        }

        var placed: [PlacedItem] = []
        placed.reserveCapacity(n)
        for i in 0..<n {
            let (r, c) = cell(of: i, g: g)
            let cellW = colWidths[c]
            let itemW = Swift.min(measured[i].width, cellW)
            let dx: Double
            switch w.itemAlign {
            case .leading: dx = 0
            case .center: dx = (cellW - itemW) / 2
            case .trailing: dx = cellW - itemW
            }
            let y = Double(r) * (itemHeight + w.rowGap)
            placed.append(PlacedItem(
                index: i, row: r, column: c,
                frame: LayoutRect(x: xs[c] + dx, y: y, width: itemW, height: itemHeight),
                truncated: measured[i].width > cellW + 0.01))
        }

        let contentW = totalCols + gaps
        let contentH = Double(g.rows) * itemHeight + Double(Swift.max(0, g.rows - 1)) * w.rowGap
        if availH.isFinite && contentH > availH { overflowed = true }

        return CandidateLayoutResult(rows: g.rows, columns: g.columns,
                                     contentSize: LayoutSize(width: contentW, height: contentH),
                                     items: placed, overflowed: overflowed)
    }

    /// 窗的外框尺寸：內容加上 padding，再套 `min_width` / `max_width`。
    public static func windowSize(content: LayoutSize, window w: CandidateWindow) -> LayoutSize {
        var width = content.width + 2 * w.padding
        width = Swift.max(width, w.minWidth)
        if w.maxWidth > 0 { width = Swift.min(width, w.maxWidth) }
        var height = content.height + 2 * w.padding
        if w.maxHeight > 0 { height = Swift.min(height, w.maxHeight) }
        return LayoutSize(width: width, height: height)
    }

    // ───────────────────────── 內部 ─────────────────────────

    struct Geometry { let rows: Int; let columns: Int; let perLine: Int; let horizontal: Bool }

    /// L = 次要軸上的行數；K = ceil(n / L) = 主要軸上每行放幾個。
    static func geometry(n: Int, L: Int, horizontal: Bool) -> Geometry {
        let lines = Swift.max(1, Swift.min(L, n))
        let perLine = (n + lines - 1) / lines
        // 行數可能因為整除而用不完（n=5, L=4 → K=2 → 只需要 3 行）。
        let usedLines = (n + perLine - 1) / perLine
        return horizontal
            ? Geometry(rows: usedLines, columns: perLine, perLine: perLine, horizontal: true)
            : Geometry(rows: perLine, columns: usedLines, perLine: perLine, horizontal: false)
    }

    static func cell(of i: Int, g: Geometry) -> (row: Int, column: Int) {
        g.horizontal
            ? (row: i / g.perLine, column: i % g.perLine)
            : (row: i % g.perLine, column: i / g.perLine)
    }

    static func columnWidths(measured: [MeasuredItem], g: Geometry, equal: Bool) -> [Double] {
        if equal {
            let m = measured.map(\.width).max() ?? 0
            return [Double](repeating: m, count: g.columns)
        }
        var out = [Double](repeating: 0, count: g.columns)
        for (i, item) in measured.enumerated() {
            let (_, c) = cell(of: i, g: g)
            out[c] = Swift.max(out[c], item.width)
        }
        return out
    }

    static func contentSize(measured: [MeasuredItem], g: Geometry,
                            itemHeight: Double, w: CandidateWindow) -> LayoutSize {
        let cols = columnWidths(measured: measured, g: g, equal: w.equalColumns)
        let width = cols.reduce(0, +) + Double(Swift.max(0, g.columns - 1)) * w.columnGap
        let height = Double(g.rows) * itemHeight + Double(Swift.max(0, g.rows - 1)) * w.rowGap
        return LayoutSize(width: width, height: height)
    }
}
