//
//  SwiftSourceScanner.swift — 「這條線真的接上了嗎」的判準
//
//  ── 為什麼需要它 ────────────────────────────────────────────────────────
//  上一輪(fix3-mac)四條真機缺陷的修法裡,有五處**接線**落在 `AppSources/`。
//  那個目錄需要 AppKit 與 InputMethodKit,`swift test` 根本不編譯它,
//  run_kit_tests.sh 的變異測試也只跑 Kit;`--self-check` 那時只問了
//  `recognizedEvents` 一件事。覆核者實測的結果是:**五處接線全部拆得掉
//  而 CI 不紅** —— 純函式都在、單元測試全綠、bundle 也還是那個 bundle,
//  只是沒有人再呼叫它們了。
//
//  ── 為什麼它不是 grep ───────────────────────────────────────────────────
//  這個專案已經有四次「守門被別處出現一次的名字餵飽」的紀錄
//  (最近的一次:守門問的是「字串在不在檔案裡」,不是「這份 APK 還是不是
//  輸入法」)。所以判準落在**呼叫位置與資料流**上,而不是字串出現與否:
//
//    1. 先把**註解與字串字面值抹掉**。名字寫在註解裡不算接上線。
//    2. 只在**指定的那一個函式主體**裡看(大括號配對切出來)。
//       同一個檔案別的地方叫過一次不算。
//    3. 看的是**引數標籤收到什麼**:`panel.show(theme:)` 拿到的到底是
//       `effectiveTheme` 還是 `themes.current` —— 兩者都「有那個字」。
//    4. 需要跨幾步的地方看**值的去向**(粗糙的 def-use):
//       `SchemaListReader.resolve()` 算出來的東西有沒有真的流到
//       `InputModeBinding.resolve(enabled:)`,還是算完就丟。
//
//  ⚠ **它證明得了的事情有邊界。** 它讀的是原始碼的結構,不是執行時的行為。
//    「真的滾一下會不會翻頁」仍然只有人在真 Mac 上驗得到
//    (apple/README.md §3 第 14 條)。這裡擋的是**靜靜還原**:
//    有人(包括未來的我們)把呼叫點拿掉,而 CI 什麼都沒說。
//
//  ⚠ **這一份自己也必須被證明會說「不」。** `SwiftSourceTests` 用固定字串
//    餵它九種拆法,外加「名字只出現在註解裡」與「名字只出現在別的函式裡」,
//    每一種都斷言它回 false。少了那一組,這裡就只是一個比較長的 grep,
//    而 run_kit_tests.sh 有一格變異(把 init 的抹除拿掉)在問這件事。
//

import Foundation

/// 一份 Swift 原始碼,**註解與字串字面值已經被抹成空白**。
struct SwiftSource {

    /// 抹過的文字。長度與原文相同(被抹掉的字元換成空白,換行原樣保留),
    /// 所以行號仍然對得回原檔。
    let text: String

    init(_ raw: String) { self.text = SwiftSource.blankOutNoise(raw) }

    init(contentsOf url: URL) throws {
        self.init(try String(contentsOf: url, encoding: .utf8))
    }

    // MARK: - 抹掉註解與字串

    private static func blank(_ a: inout [Character], _ k: Int) {
        if a[k] != "\n" { a[k] = " " }
    }

    /// `//`、`/* */`(可巢狀)、`"…"` 與 `"""…"""` 一律換成空白。
    ///
    /// 為什麼連字串也抹:`NSLog("…panel.onChangePage…")` 這種東西會讓
    /// 「有沒有接線」變成「有沒有人在訊息裡提過它」。
    static func blankOutNoise(_ raw: String) -> String {
        var a = Array(raw)
        let n = a.count
        var i = 0
        var comment = 0
        while i < n {
            if comment > 0 {
                if a[i] == "/" && i + 1 < n && a[i + 1] == "*" {
                    comment += 1
                    blank(&a, i); blank(&a, i + 1); i += 2; continue
                }
                if a[i] == "*" && i + 1 < n && a[i + 1] == "/" {
                    comment -= 1
                    blank(&a, i); blank(&a, i + 1); i += 2; continue
                }
                blank(&a, i); i += 1; continue
            }
            if a[i] == "/" && i + 1 < n && a[i + 1] == "/" {
                while i < n && a[i] != "\n" { blank(&a, i); i += 1 }
                continue
            }
            if a[i] == "/" && i + 1 < n && a[i + 1] == "*" {
                comment = 1
                blank(&a, i); blank(&a, i + 1); i += 2; continue
            }
            if a[i] == "\"" {
                if i + 2 < n && a[i + 1] == "\"" && a[i + 2] == "\"" {
                    blank(&a, i); blank(&a, i + 1); blank(&a, i + 2)
                    i += 3
                    while i < n {
                        if a[i] == "\\" && i + 1 < n {
                            blank(&a, i); blank(&a, i + 1); i += 2; continue
                        }
                        if a[i] == "\"" && i + 2 < n && a[i + 1] == "\"" && a[i + 2] == "\"" {
                            blank(&a, i); blank(&a, i + 1); blank(&a, i + 2); i += 3
                            break
                        }
                        blank(&a, i); i += 1
                    }
                    continue
                }
                blank(&a, i); i += 1
                while i < n {
                    // 沒有收尾的字串不可以把後面整份吃掉 —— 停在行尾。
                    if a[i] == "\n" { break }
                    if a[i] == "\\" && i + 1 < n {
                        blank(&a, i); blank(&a, i + 1); i += 2; continue
                    }
                    if a[i] == "\"" { blank(&a, i); i += 1; break }
                    blank(&a, i); i += 1
                }
                continue
            }
            i += 1
        }
        return String(a)
    }

    // MARK: - 切出一個宣告的主體

    /// `header` 之後那一對大括號中間的東西。
    ///
    /// `header` 是宣告的**開頭字串**,例如 `"func updatePanel("` 或
    /// `"var effectiveTheme:"`。它必須出現在抹掉註解之後的文字裡,
    /// 所以「只在檔頭註解裡提過」不算數。
    func body(of header: String) -> String? {
        guard let r = text.range(of: header) else { return nil }
        return SwiftSource.bracedBlock(in: text, from: r.upperBound)
    }

    static func bracedBlock(in s: String, from: String.Index) -> String? {
        guard let open = s[from...].firstIndex(of: "{") else { return nil }
        var depth = 0
        var i = open
        while i < s.endIndex {
            let c = s[i]
            if c == "{" { depth += 1 }
            if c == "}" {
                depth -= 1
                if depth == 0 { return String(s[s.index(after: open)..<i]) }
            }
            i = s.index(after: i)
        }
        return nil
    }

    // MARK: - 名字

    /// `needle` 在 `hay` 裡是不是一個**獨立的**名字。
    ///
    /// `.` 算分隔符,所以 `effectiveTheme` 在 `AppContext.shared.effectiveTheme`
    /// 裡算命中;但 `Theme` 在 `effectiveTheme` 裡**不算**。
    static func mentions(_ needle: String, in hay: String) -> Bool {
        guard !needle.isEmpty else { return false }
        var search = hay.startIndex
        while search < hay.endIndex,
              let r = hay.range(of: needle, range: search..<hay.endIndex) {
            search = r.upperBound
            if !isBoundaryBefore(r.lowerBound, in: hay) { continue }
            if r.upperBound < hay.endIndex {
                let c = hay[r.upperBound]
                if c.isLetter || c.isNumber || c == "_" { continue }
            }
            return true
        }
        return false
    }

    private static func isBoundaryBefore(_ i: String.Index, in s: String) -> Bool {
        guard i > s.startIndex else { return true }
        let c = s[s.index(before: i)]
        return !(c.isLetter || c.isNumber || c == "_")
    }

    // MARK: - 呼叫

    /// 對 `callee` 的第一個呼叫的引數清單(不含最外層括號)。
    ///
    /// `callee` 後面允許空白與 `?` / `!`(`onChangePage?(step)` 也算呼叫),
    /// 但必須接著 `(`。
    static func argumentList(ofCall callee: String, in body: String) -> String? {
        var search = body.startIndex
        while search < body.endIndex,
              let r = body.range(of: callee, range: search..<body.endIndex) {
            search = r.upperBound
            if !isBoundaryBefore(r.lowerBound, in: body) { continue }
            var i = r.upperBound
            while i < body.endIndex, isSkippable(body[i]) { i = body.index(after: i) }
            guard i < body.endIndex, body[i] == "(" else { continue }
            var depth = 0
            var j = i
            while j < body.endIndex {
                let c = body[j]
                if c == "(" { depth += 1 }
                if c == ")" {
                    depth -= 1
                    if depth == 0 { return String(body[body.index(after: i)..<j]) }
                }
                j = body.index(after: j)
            }
            return nil
        }
        return nil
    }

    private static func isSkippable(_ c: Character) -> Bool {
        c == " " || c == "\n" || c == "\t" || c == "?" || c == "!"
    }

    static func calls(_ callee: String, in body: String) -> Bool {
        argumentList(ofCall: callee, in: body) != nil
    }

    /// 引數清單 → 「標籤 → 運算式」。沒有標籤的用位置(`_0`、`_1`…)。
    static func arguments(ofCall callee: String, in body: String) -> [String: String] {
        guard let list = argumentList(ofCall: callee, in: body) else { return [:] }
        var parts: [String] = []
        var depth = 0
        var current = ""
        for c in list {
            if c == "(" || c == "[" || c == "{" { depth += 1 }
            if c == ")" || c == "]" || c == "}" { depth -= 1 }
            if c == "," && depth == 0 {
                parts.append(current)
                current = ""
                continue
            }
            current.append(c)
        }
        parts.append(current)

        var out: [String: String] = [:]
        var positional = 0
        for raw in parts {
            let p = raw.trimmingCharacters(in: .whitespacesAndNewlines)
            if p.isEmpty { continue }
            if let colon = labelColon(p) {
                let label = String(p[p.startIndex..<colon])
                let value = String(p[p.index(after: colon)...])
                    .trimmingCharacters(in: .whitespacesAndNewlines)
                out[label] = value
            } else {
                out["_\(positional)"] = p
                positional += 1
            }
        }
        return out
    }

    private static func labelColon(_ p: String) -> String.Index? {
        guard let colon = p.firstIndex(of: ":") else { return nil }
        let head = p[p.startIndex..<colon]
        if head.isEmpty { return nil }
        for ch in head {
            if !(ch.isLetter || ch.isNumber || ch == "_") { return nil }
        }
        return colon
    }

    // MARK: - 賦值

    /// `lhs = …` 的右手邊。右手邊是 `{` 時取整個大括號區塊,否則取到行尾。
    ///
    /// `==` / `!=` / `>=` 不算賦值 —— 那是比較,不是接線。
    static func assignedExpression(to lhs: String, in body: String) -> String? {
        var search = body.startIndex
        while search < body.endIndex,
              let r = body.range(of: lhs, range: search..<body.endIndex) {
            search = r.upperBound
            if !isBoundaryBefore(r.lowerBound, in: body) { continue }
            var i = r.upperBound
            while i < body.endIndex, body[i] == " " || body[i] == "\t" {
                i = body.index(after: i)
            }
            guard i < body.endIndex, body[i] == "=" else { continue }
            let after = body.index(after: i)
            if after < body.endIndex, body[after] == "=" { continue }
            var j = after
            while j < body.endIndex, body[j] == " " || body[j] == "\t" {
                j = body.index(after: j)
            }
            if j < body.endIndex, body[j] == "{" {
                return bracedBlock(in: body, from: j)
            }
            if let nl = body[j...].firstIndex(of: "\n") { return String(body[j..<nl]) }
            return String(body[j...])
        }
        return nil
    }

    // MARK: - 值的去向(粗糙的 def-use)

    /// 從 `callee` 的回傳值出發,沿著 `let x = …` / `var x = …` 往下傳,
    /// 回傳所有「值來自那個呼叫」的名字。
    ///
    /// ⚠ 它很粗:不追分支、不追欄位、只認一行裡的繫結。但它問得出**這一個**
    ///   問題 —— 那個呼叫算出來的東西,有沒有真的流到終點,還是算完就丟。
    ///   「算完就丟」正是這一輪要擋的那種拆法。
    static func valuesDerived(fromCall callee: String, in body: String) -> Set<String> {
        let lines = body.components(separatedBy: "\n")
        var reached = Set<String>()
        var changed = true
        while changed {
            changed = false
            for line in lines {
                guard let b = binding(in: line) else { continue }
                if reached.contains(b.name) { continue }
                var hit = mentions(callee, in: b.rhs)
                if !hit {
                    for n in reached where mentions(n, in: b.rhs) {
                        hit = true
                        break
                    }
                }
                if hit {
                    reached.insert(b.name)
                    changed = true
                }
            }
        }
        return reached
    }

    /// 一行裡的 `let x = …` / `var x = …`。`if let` / `guard let` 不算。
    static func binding(in line: String) -> (name: String, rhs: String)? {
        let t = line.trimmingCharacters(in: .whitespaces)
        let rest: Substring
        if t.hasPrefix("let ") {
            rest = t.dropFirst(4)
        } else if t.hasPrefix("var ") {
            rest = t.dropFirst(4)
        } else {
            return nil
        }
        guard let eq = rest.firstIndex(of: "=") else { return nil }
        var name = String(rest[rest.startIndex..<eq])
        if let colon = name.firstIndex(of: ":") { name = String(name[name.startIndex..<colon]) }
        name = name.trimmingCharacters(in: .whitespaces)
        guard !name.isEmpty else { return nil }
        for ch in name {
            if !(ch.isLetter || ch.isNumber || ch == "_") { return nil }
        }
        return (name, String(rest[rest.index(after: eq)...]))
    }

    /// `derived` 裡有沒有任何一個名字出現在 `expression` 中。
    static func anyOf(_ derived: Set<String>, reaches expression: String) -> Bool {
        for n in derived where mentions(n, in: expression) { return true }
        return false
    }
}

// MARK: - 讀 AppSources 的檔案

extension Repo {
    static var appSourcesDir: URL {
        root.appendingPathComponent("apple/LuminaKey/AppSources")
    }

    /// ⚠ 讀的是**原始碼**,不是編出來的東西 —— 這個 target 編不動 AppKit。
    ///   為什麼還是值得讀,見本檔開頭。
    static func appSource(_ name: String) throws -> SwiftSource {
        try SwiftSource(contentsOf: appSourcesDir.appendingPathComponent(name))
    }
}
