//
//  StatusFace.swift — 狀態列每一項「顯示成什麼」（docs/theme-format.md §8.12）
//
//  規範把 `label_from` 的中文字面訂成規範性的（四端一致）：
//  `input_mode` 是「中」／「英」，`shape` 是「全」／「半」，`variant` 是「简」／「繁」。
//  `input_mode_pair` 則同時畫出兩態並強調當前那一態。
//
//  ── 為什麼 pair 不是外觀偏好 ────────────────────────────────────────────
//  只寫一個「中」的切換鍵有兩種讀法 ——「現在是中文」與「按了會變中文」——
//  而它們指向相反的操作。Android 在真機上被使用者回報過。同時畫出兩態、
//  強調其中一態，這個歧義**在結構上就不存在**。
//

import Foundation

/// 給渲染端的一段文字。`emphasised` 用 `active_color`，其餘用 `color`。
public struct FaceSegment: Equatable, Sendable {
    public let text: String
    public let emphasised: Bool
    public init(_ text: String, emphasised: Bool) {
        self.text = text
        self.emphasised = emphasised
    }
}

/// rs_status 的純資料鏡像（讓這一層不必碰 C 結構）。
public struct EngineStatus: Sendable {
    public var schemaId = ""
    public var schemaName = ""
    public var isComposing = false
    public var isAsciiMode = false
    public var isFullShape = false
    public var isSimplified = false
    public var isAsciiPunct = false
    public var isDisabled = false

    public init() {}
}

public enum StatusFace {

    // 規範性字面（§9.6 的 label_from 表 + §8.12 的 status_bar）。
    public static let cjk = "中"
    /// 用 `En` 而不是 `英`：一邊漢字一邊拉丁，兩態一眼就分得開。
    public static let latin = "En"
    public static let separator = "/"

    /// 一項狀態顯示成哪幾段。空陣列 = 這一項在當前狀態下沒有東西可顯示，
    /// 渲染端**必須**整項略過（不得留一塊看不出用途的空白）。
    public static func segments(for item: StatusItem, status: EngineStatus,
                                pageNo: Int, isLastPage: Bool) -> [FaceSegment] {
        switch item.source {
        case .schemaName:
            return status.schemaName.isEmpty ? [] : [FaceSegment(status.schemaName, emphasised: false)]
        case .schemaId:
            return status.schemaId.isEmpty ? [] : [FaceSegment(status.schemaId, emphasised: false)]
        case .inputMode:
            return [FaceSegment(status.isAsciiMode ? latin : cjk, emphasised: true)]
        case .inputModePair:
            return [
                FaceSegment(cjk, emphasised: !status.isAsciiMode),
                FaceSegment(separator, emphasised: false),
                FaceSegment(latin, emphasised: status.isAsciiMode),
            ]
        case .shape:
            return [FaceSegment(status.isFullShape ? "全" : "半", emphasised: true)]
        case .variant:
            return [FaceSegment(status.isSimplified ? "简" : "繁", emphasised: true)]
        case .page:
            // 只有一頁時不顯示頁碼 —— 每一次組字都掛一個「1」是純粹的噪音。
            if pageNo == 0 && isLastPage { return [] }
            return [FaceSegment("\(pageNo + 1)\(isLastPage ? "" : "+")", emphasised: false)]
        case .text:
            return item.text.isEmpty ? [] : [FaceSegment(item.text, emphasised: false)]
        }
    }

    /// 純文字形態（量測與無障礙朗讀用）。
    public static func plainText(_ segs: [FaceSegment]) -> String {
        segs.map(\.text).joined()
    }

    /// §9.5.1：本端不支援的動詞不得畫出來。
    /// 狀態列是可以在執行期過濾的（項目沒有固定寬度，少一項不會讓別項移位到看不懂）。
    public static func renderable(_ items: [StatusItem]) -> [StatusItem] {
        items.filter { item in
            guard let tap = item.tap else { return true }
            return DesktopVerbSupport.isImplemented(tap.verb)
        }
    }
}
