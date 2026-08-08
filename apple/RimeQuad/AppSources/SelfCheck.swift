//
//  SelfCheck.swift — `RimeQuad --self-check`
//
//  ── 為什麼要在**這個**二進位裡跑 ────────────────────────────────────────
//  CI 上驗不了候選窗，但驗得了「這一份 .app 的二進位真的連上了 librime，
//  而且它依賴的每一個 keysym 名稱在 librime 裡查得到」。
//
//  這件事非得由 app 自己做不可：`PhysicalKeys.table` 裡放的是**名稱**，
//  值要向 `rs_keysym_by_name()` 要。名稱抄錯了（`Page_up` vs `Page_Up`）
//  查出來是 0，而 0 的表現是「那顆鍵什麼都不會發生」—— 畫面完全正常，
//  單元測試也全綠，因為測試用的是固定表而不是 librime。
//
//  ⚠ 這幾項是純查表，**不需要 rs_init()**，所以在沒有資料目錄的 CI 上也跑得動。
//

import Foundation

enum SelfCheck {

    static func run() -> Bool {
        var ok = true
        func check(_ label: String, _ passed: Bool, _ detail: String = "") {
            print("\(passed ? "  ✓" : "  ✗") \(label)\(detail.isEmpty ? "" : " — \(detail)")")
            if !passed { ok = false }
        }

        print("=== RimeQuad self-check ===")

        // 1. ABI 協商。實作端與呼叫端不同步時，這是唯一擋得住的關卡。
        let impl = rs_abi_version()
        check("ABI 版本相符", impl == Int32(RIME_SHELL_ABI_VERSION),
              "實作端 \(impl) / 呼叫端 \(RIME_SHELL_ABI_VERSION)")

        // 2. keysym 正反查（門面層把查不到正規化成 0，不是 XK_VoidSymbol）。
        let back = rs_keysym_by_name("BackSpace")
        check("rs_keysym_by_name(\"BackSpace\") == 0xFF08", back == 0xFF08, String(format: "0x%X", back))
        let bogus = rs_keysym_by_name("NoSuchKeyAtAll")
        check("未知鍵名回傳 0（不是 XK_VoidSymbol 0xFFFFFF）", bogus == 0, "\(bogus)")
        if let name = rs_keysym_name(0xFF08) {
            check("rs_keysym_name(0xFF08) == \"BackSpace\"", String(cString: name) == "BackSpace")
        } else {
            check("rs_keysym_name(0xFF08)", false, "回傳 NULL")
        }

        // 3. PhysicalKeys 的每一個名稱都要查得到。這是本檔存在的主要理由。
        var bad: [(UInt16, String)] = []
        for (code, name) in PhysicalKeys.table.sorted(by: { $0.key < $1.key }) {
            let v = name.withCString { rs_keysym_by_name($0) }
            if v == 0 { bad.append((code, name)) }
        }
        check("PhysicalKeys.table 的 \(PhysicalKeys.table.count) 個名稱 librime 全部查得到",
              bad.isEmpty, bad.map { "keyCode \($0.0)=\($0.1)" }.joined(separator: ", "))

        // 4. 每一個修飾鍵 keyCode 都要有名字（少一個＝那顆修飾鍵送不進引擎）。
        let orphan = PhysicalKeys.modifierKeyFlag.keys.filter { PhysicalKeys.table[$0] == nil }
        check("修飾鍵 keyCode 都有 keysym 名稱", orphan.isEmpty, "\(orphan)")

        // 5. 隨附主題解析得起來（用 .app 裡真正會被讀到的那一份）。
        let themesDir = (Bundle.main.resourceURL ?? URL(fileURLWithPath: "."))
            .appendingPathComponent("themes")
        let source = DirectoryDocumentSource([themesDir])
        var themeIds = (try? FileManager.default.contentsOfDirectory(atPath: themesDir.path))?
            .filter { $0.hasSuffix(".yaml") }.map { String($0.dropLast(5)) }.sorted() ?? []
        themeIds = themeIds.filter { !$0.isEmpty }
        check("隨附了主題檔", !themeIds.isEmpty, "\(themesDir.path)")
        for id in themeIds {
            let r = ThemeLoader.load(id: id, source: source, platform: .macos)
            check("主題 \(id) 解析乾淨",
                  r.isSuccess && r.errors.isEmpty && r.warnings.isEmpty,
                  r.diagnostics.map(\.developerMessage).joined(separator: "; "))
        }

        print(ok ? "=== self-check 通過 ===" : "=== self-check 失敗 ===")
        return ok
    }
}
