import XCTest
@testable import RimeQuadKit

/// docs/theme-format.md §8.6.7.1 的排版演算法。
///
/// 這一組是「候選窗在 CI 上驗得到的那一半」。畫不出來，但**算得出來**：
/// 每一格的座標、窗的尺寸、什麼時候換行、超出時怎麼縮，全部是純算術。
final class CandidateLayoutTests: XCTestCase {

    private func window(_ mutate: (inout CandidateWindow) -> Void = { _ in })
        -> CandidateWindow {
        var w = CandidateWindow()
        w.padding = 6
        w.columnGap = 4
        w.rowGap = 4
        w.maxWidth = 640
        mutate(&w)
        return w
    }

    private func items(_ widths: [Double], height: Double = 20) -> [MeasuredItem] {
        widths.map { MeasuredItem(width: $0, height: height) }
    }

    func testEmptyPage() {
        let r = CandidateLayout.layout(measured: [], window: window())
        XCTAssertEqual(r.items.count, 0)
        XCTAssertEqual(r.contentSize, LayoutSize(width: 0, height: 0))
    }

    /// `lines: 1` + horizontal ＝ v1 的單行行為，必須逐位元不變。
    func testLinearHorizontalIsTheV1Behaviour() {
        let r = CandidateLayout.layout(measured: items([40, 40, 40, 40, 40]), window: window())
        XCTAssertEqual(r.rows, 1)
        XCTAssertEqual(r.columns, 5)
        XCTAssertEqual(r.contentSize, LayoutSize(width: 5 * 40 + 4 * 4, height: 20))
        XCTAssertEqual(r.items.map(\.frame.x), [0, 44, 88, 132, 176])
        XCTAssertFalse(r.overflowed)
        let ws = CandidateLayout.windowSize(content: r.contentSize, window: window())
        XCTAssertEqual(ws, LayoutSize(width: 228, height: 32))
    }

    func testLinearVerticalStacks() {
        var w = window()
        w.style.orientation = .vertical
        let r = CandidateLayout.layout(measured: items([40, 60, 30]), window: w)
        XCTAssertEqual(r.rows, 3)
        XCTAssertEqual(r.columns, 1)
        XCTAssertEqual(r.contentSize, LayoutSize(width: 60, height: 3 * 20 + 2 * 4))
        XCTAssertEqual(r.items.map(\.frame.y), [0, 24, 48])
    }

    /// horizontal + lines>1 → 逐列填滿（row-major）。
    func testGridHorizontalIsRowMajor() {
        var w = window()
        w.lines = 2
        let r = CandidateLayout.layout(measured: items([40, 40, 40, 40, 40]), window: w)
        XCTAssertEqual(r.rows, 2)
        XCTAssertEqual(r.columns, 3)
        XCTAssertEqual(r.items.map { [$0.row, $0.column] },
                       [[0, 0], [0, 1], [0, 2], [1, 0], [1, 1]])
        XCTAssertEqual(r.contentSize, LayoutSize(width: 3 * 40 + 2 * 4, height: 2 * 20 + 4))
    }

    /// vertical + lines>1 → 逐欄填滿（column-major）。中文輸入法的兩欄候選就是這個。
    func testGridVerticalIsColumnMajor() {
        var w = window()
        w.style.orientation = .vertical
        w.lines = 2
        let r = CandidateLayout.layout(measured: items([40, 40, 40, 40, 40]), window: w)
        XCTAssertEqual(r.rows, 3)
        XCTAssertEqual(r.columns, 2)
        XCTAssertEqual(r.items.map { [$0.row, $0.column] },
                       [[0, 0], [1, 0], [2, 0], [0, 1], [1, 1]])
        XCTAssertEqual(r.contentSize, LayoutSize(width: 2 * 40 + 4, height: 3 * 20 + 2 * 4))
    }

    func testEqualColumnsUsesTheWidestItemEverywhere() {
        var w = window()
        w.lines = 2
        w.equalColumns = true
        let r = CandidateLayout.layout(measured: items([10, 50, 20, 30]), window: w)
        XCTAssertEqual(r.columns, 2)
        XCTAssertEqual(r.contentSize.width, 2 * 50 + 4)
    }

    func testUnequalColumnsUseTheWidestPerColumn() {
        var w = window()
        w.lines = 2
        w.equalColumns = false
        let r = CandidateLayout.layout(measured: items([10, 50, 20, 30]), window: w)
        // (0,0)=10 (0,1)=50 (1,0)=20 (1,1)=30 → 欄寬 20 與 50
        XCTAssertEqual(r.contentSize.width, 20 + 50 + 4)
        XCTAssertEqual(r.items[1].frame.x, 24)
    }

    /// `lines: 0` = 自動：horizontal 往 `max_width` 收斂。
    func testAutoLinesHorizontalWrapsToFitMaxWidth() {
        var w = window()
        w.lines = 0
        w.maxWidth = 300
        let r = CandidateLayout.layout(measured: items(Array(repeating: 100, count: 10)), window: w)
        XCTAssertEqual(r.columns, 2)
        XCTAssertEqual(r.rows, 5)
        XCTAssertFalse(r.overflowed)
    }

    /// vertical 的自動方向是 `max_height`，不是 max_width ——
    /// 對垂直清單來說「太長」才是問題，「太寬」不是。
    func testAutoLinesVerticalWrapsToFitMaxHeight() {
        var w = window()
        w.style.orientation = .vertical
        w.lines = 0
        w.maxHeight = 100          // 內容區高 88 → 每欄最多 (88+4)/24 = 3 列
        let r = CandidateLayout.layout(measured: items(Array(repeating: 40, count: 9)), window: w)
        XCTAssertEqual(r.rows, 3)
        XCTAssertEqual(r.columns, 3)
    }

    func testAutoLinesWithoutLimitStaysOnOneLine() {
        var w = window()
        w.lines = 0
        w.maxWidth = 0             // 0 = 不限
        let r = CandidateLayout.layout(measured: items([100, 100, 100]), window: w)
        XCTAssertEqual(r.rows, 1)
    }

    /// 超出 `max_width`：`shrink` 等比縮欄，並把每一格標記為需要截斷。
    func testOverflowShrinkScalesColumnsProportionally() {
        var w = window()
        w.maxWidth = 300
        w.overflow = .shrink
        let r = CandidateLayout.layout(measured: items([200, 200, 200]), window: w)
        XCTAssertTrue(r.overflowed)
        XCTAssertEqual(r.contentSize.width, 288, accuracy: 0.001)
        XCTAssertTrue(r.items.allSatisfy(\.truncated), "縮過的格子必須被標記，渲染端才會加上 …")
        let ws = CandidateLayout.windowSize(content: r.contentSize, window: w)
        XCTAssertEqual(ws.width, 300, accuracy: 0.001)
    }

    /// `clip` 不動欄寬，窗被 `max_width` 夾住。
    func testOverflowClipKeepsColumnWidths() {
        var w = window()
        w.maxWidth = 300
        w.overflow = .clip
        let r = CandidateLayout.layout(measured: items([200, 200, 200]), window: w)
        XCTAssertTrue(r.overflowed)
        XCTAssertEqual(r.contentSize.width, 3 * 200 + 2 * 4)
        XCTAssertEqual(CandidateLayout.windowSize(content: r.contentSize, window: w).width, 300)
    }

    /// 縮到 `item.min_width` 就停 —— **不得**把某一欄縮成 0，
    /// 那會產生一個看得見卻讀不到的候選。
    func testShrinkRespectsMinWidthFloor() {
        var w = window()
        w.maxWidth = 40
        w.overflow = .shrink
        w.style.item.minWidth = 30
        let r = CandidateLayout.layout(measured: items([200, 200]), window: w)
        XCTAssertTrue(r.items.allSatisfy { $0.frame.width >= 30 })
    }

    func testItemAlignment() {
        var w = window()
        w.equalColumns = true
        w.itemAlign = .center
        let r = CandidateLayout.layout(measured: items([20, 60]), window: w)
        XCTAssertEqual(r.items[0].frame.x, 20, "60 寬的欄裡置中 20 寬的項 → 偏移 20")
        w.itemAlign = .trailing
        let r2 = CandidateLayout.layout(measured: items([20, 60]), window: w)
        XCTAssertEqual(r2.items[0].frame.x, 40)
    }

    /// 列高是**該頁最高的那一項**，不是第一項也不是最矮的那一項。
    /// （`position: below` 的註解會讓某些項高一倍。）
    func testRowHeightIsTheTallestItem() {
        let m = [MeasuredItem(width: 40, height: 20), MeasuredItem(width: 40, height: 36)]
        let r = CandidateLayout.layout(measured: m, window: window())
        XCTAssertEqual(r.contentSize.height, 36)
        XCTAssertTrue(r.items.allSatisfy { $0.frame.height == 36 })
    }

    func testWindowMinWidthWins() {
        var w = window()
        w.minWidth = 400
        let r = CandidateLayout.layout(measured: items([40]), window: w)
        XCTAssertEqual(CandidateLayout.windowSize(content: r.contentSize, window: w).width, 400)
    }

    /// `lines` 大於候選數時不得排出空行。
    func testLinesLargerThanItemCount() {
        var w = window()
        w.lines = 8
        let r = CandidateLayout.layout(measured: items([40, 40, 40]), window: w)
        XCTAssertEqual(r.rows, 3)
        XCTAssertEqual(r.columns, 1)
    }
}
