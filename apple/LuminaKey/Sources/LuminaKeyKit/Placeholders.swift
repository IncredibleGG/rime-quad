//
//  Placeholders.swift — `{…}` 樣板展開
//
//  用於 §8.6.1 的 `candidates.label.format` 與 §8.13 的
//  `accessibility.candidate_announcement`。
//
//  規範明文規定：**未知佔位符必須原樣保留**（不得丟棄，也不得報錯）。
//  理由是新增佔位符不算破壞性變更（§5.3），舊客戶端遇到新佔位符必須優雅退化 ——
//  而「原樣保留」是唯一一種使用者看得見、又不會讓版面塌掉的退化。
//

import Foundation

public enum Placeholders {

    /// 展開 `{name}`。`values` 查不到的名稱連同大括號原樣保留。
    public static func expand(_ format: String, _ values: [String: String]) -> String {
        var out = ""
        var i = format.startIndex
        while i < format.endIndex {
            let c = format[i]
            if c != "{" { out.append(c); i = format.index(after: i); continue }
            guard let close = format[i...].firstIndex(of: "}") else {
                // 沒有閉合的大括號：原樣輸出剩餘全部。
                out.append(contentsOf: format[i...])
                break
            }
            let name = String(format[format.index(after: i)..<close])
            if let v = values[name] {
                out.append(v)
            } else {
                out.append(contentsOf: format[i...close])
            }
            i = format.index(after: close)
        }
        return out
    }

    /// §8.6.1 的候選標籤。`{label}` / `{index}`（1 起算）/ `{index0}`。
    public static func candidateLabel(format: String, label: String, indexOnPage: Int) -> String {
        expand(format, [
            "label": label,
            "index": String(indexOnPage + 1),
            "index0": String(indexOnPage),
        ])
    }

    /// §8.13 的候選朗讀名。空欄位不留下多餘空白 —— VoiceOver 會把連續空白念成停頓。
    public static func announcement(format: String, label: String, text: String,
                                    comment: String) -> String {
        let raw = expand(format, ["label": label, "text": text, "comment": comment])
        return raw.split(separator: " ", omittingEmptySubsequences: true)
            .joined(separator: " ")
            .trimmingCharacters(in: .whitespaces)
    }
}
