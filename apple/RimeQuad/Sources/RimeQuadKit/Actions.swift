//
//  Actions.swift — §9.5 的 action 字串
//
//  桌面端沒有軟鍵盤，但**狀態列**與（規範上的）工具列項目都用同一套動詞，
//  所以動詞表本身是共用的，不是行動端專屬。
//

import Foundation

public enum ActionVerb: String, Sendable, CaseIterable {
    case noop
    case layer, layerOnce, layerLock
    case switchLayout
    case toggleOption, setOption
    case schemaNext, schemaPrev, schemaPicker, schemaSelect
    case candidateSelect, candidateDelete
    case candidateNextPage, candidatePrevPage, candidateNext, candidatePrev
    case cursorLeft, cursorRight, cursorHome, cursorEnd
    case clear, hideKeyboard, settings, emoji
    /// §9.5：「切中英」的完整語義 —— 切模式**並且**切到佈局宣告的 `alpha_layer`。
    case inputModeToggle
}

public struct KeyAction: Sendable, Equatable {
    public let verb: ActionVerb
    public let args: [String]
    public let raw: String

    public init(_ verb: ActionVerb, _ args: [String] = [], raw: String) {
        self.verb = verb
        self.args = args
        self.raw = raw
    }

    public var arg: String? { args.first }
}

public enum Actions {

    /// 解析 `<verb>` 或 `<verb>:<arg>[:<arg>]`。
    /// 未知動詞 / 參數不合法 → nil + WARNING（呼叫端據 §6.3 退化為 noop 或丟棄該項）。
    public static func parse(_ raw: String, path: String, diag: Diagnostics, line: Int?) -> KeyAction? {
        let t = raw.trimmingCharacters(in: .whitespaces)
        if t.isEmpty {
            diag.add(.unknownAction, [raw], path: path, line: line)
            return nil
        }
        let parts = t.split(separator: ":", omittingEmptySubsequences: false).map(String.init)
        let head = parts[0]
        let rest = Array(parts.dropFirst())

        func needArg(_ verb: ActionVerb) -> KeyAction? {
            guard let a = rest.first, !a.isEmpty else {
                diag.add(.badActionArgument, [t], path: path, line: line)
                return nil
            }
            return KeyAction(verb, [a], raw: t)
        }

        switch head {
        case "noop": return KeyAction(.noop, raw: t)
        case "layer": return needArg(.layer)
        case "layer_once": return needArg(.layerOnce)
        case "layer_lock": return needArg(.layerLock)
        case "switch_layout": return needArg(.switchLayout)
        case "toggle": return needArg(.toggleOption)
        case "set":
            guard rest.count >= 2, !rest[0].isEmpty,
                  rest[1] == "on" || rest[1] == "off" else {
                diag.add(.badActionArgument, [t], path: path, line: line)
                return nil
            }
            return KeyAction(.setOption, [rest[0], rest[1]], raw: t)
        case "input_mode":
            guard rest.first == "toggle" else {
                diag.add(.badActionArgument, [t], path: path, line: line)
                return nil
            }
            return KeyAction(.inputModeToggle, raw: t)
        case "schema":
            guard let a = rest.first, !a.isEmpty else {
                diag.add(.badActionArgument, [t], path: path, line: line)
                return nil
            }
            switch a {
            case "next": return KeyAction(.schemaNext, raw: t)
            case "prev": return KeyAction(.schemaPrev, raw: t)
            case "picker": return KeyAction(.schemaPicker, raw: t)
            default: return KeyAction(.schemaSelect, [a], raw: t)
            }
        case "candidate":
            guard let a = rest.first, !a.isEmpty else {
                diag.add(.badActionArgument, [t], path: path, line: line)
                return nil
            }
            switch a {
            case "select", "delete":
                guard rest.count >= 2, let n = Int(rest[1]), n >= 0 else {
                    diag.add(.badActionArgument, [t], path: path, line: line)
                    return nil
                }
                return KeyAction(a == "select" ? .candidateSelect : .candidateDelete,
                                 [String(n)], raw: t)
            case "next_page": return KeyAction(.candidateNextPage, raw: t)
            case "prev_page": return KeyAction(.candidatePrevPage, raw: t)
            case "next": return KeyAction(.candidateNext, raw: t)
            case "prev": return KeyAction(.candidatePrev, raw: t)
            default:
                diag.add(.badActionArgument, [t], path: path, line: line)
                return nil
            }
        case "cursor":
            switch rest.first {
            case "left": return KeyAction(.cursorLeft, raw: t)
            case "right": return KeyAction(.cursorRight, raw: t)
            case "home": return KeyAction(.cursorHome, raw: t)
            case "end": return KeyAction(.cursorEnd, raw: t)
            default:
                diag.add(.badActionArgument, [t], path: path, line: line)
                return nil
            }
        case "clear": return KeyAction(.clear, raw: t)
        case "hide_keyboard": return KeyAction(.hideKeyboard, raw: t)
        case "settings": return KeyAction(.settings, raw: t)
        case "emoji": return KeyAction(.emoji, raw: t)
        default:
            diag.add(.unknownAction, [t], path: path, line: line)
            return nil
        }
    }
}

/// §9.5.1：**本端還沒有實作的動詞。**
///
/// 規範要求渲染端把「還沒實作」變成一個查得到的事實，而不是分派表裡
/// 一行安靜的 log —— 那種鍵畫面完全正常、自動化全綠，只有真的去按才知道。
/// Android 端已經因為 emoji 抓到三個入口（工具列、佈局按鍵、長按蓋掉彈出盤）。
///
/// 每一項都必須寫清楚**為什麼**還沒有；沒有理由的項目下一個人不知道能不能刪。
public enum DesktopVerbSupport {

    public static let unimplemented: Set<ActionVerb> = [
        // 表情面板還沒做。§8.6.6.1 的規範性預設工具列含 emoji，
        // 桌面端的狀態列預設清單刻意不含它。
        .emoji,

        // 桌面沒有軟鍵盤，「收起鍵盤」在這一端**沒有對應的東西**。
        // 這不是「還沒做」而是「形態上不存在」，但對渲染端的效果一樣：
        // 不得畫出來、不得靜默 noop。
        .hideKeyboard,

        // IMKit 沒有給輸入法移動宿主 app 游標的 API。
        // 偽造方向鍵給宿主是可行的，但那會在組字中被 librime 吃掉，
        // 行為與規範描述的「移動輸入框游標」不同，所以先宣告不支援。
        .cursorLeft, .cursorRight, .cursorHome, .cursorEnd,

        // 佈局是行動端的東西，桌面端不消費 core/layouts/，所以「換層／換佈局」
        // 在這一端沒有對應物。
        //
        // ⚠ `input_mode:toggle` **不**在這份清單裡。它的語義是「切模式，並且
        //   切到本佈局的 alpha_layer」，而規範明文規定沒宣告字母層的佈局只切模式 ——
        //   桌面端連佈局都沒有，所以它退化成純粹的模式切換，是**做得到**的。
        //   把它列為不支援會讓狀態列的「中/En」消失，那才是真的壞掉。
        .layer, .layerOnce, .layerLock, .switchLayout,
    ]

    public static func isImplemented(_ v: ActionVerb) -> Bool { !unimplemented.contains(v) }
}
