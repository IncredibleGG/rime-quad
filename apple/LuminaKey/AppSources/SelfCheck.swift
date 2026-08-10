//
//  SelfCheck.swift — `LuminaKey --self-check`
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
import AppKit
import InputMethodKit

enum SelfCheck {

    static func run() -> Bool {
        var ok = true
        func check(_ label: String, _ passed: Bool, _ detail: String = "") {
            print("\(passed ? "  ✓" : "  ✗") \(label)\(detail.isEmpty ? "" : " — \(detail)")")
            if !passed { ok = false }
        }

        print("=== LuminaKey self-check ===")

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

        // 2b. **IMKit 找不找得到 controller 類別。**
        //
        // 這一問原本是拿 `nm` grep `_OBJC_CLASS_$_...` 來回答的，那是在猜。
        // IMKit 實際做的事是 `NSClassFromString(Info.plist 裡那個字串)`，
        // 所以就照做一次 —— 這同時驗了三件事：@objc(...) 有生效、
        // plist 裡的名字沒打錯、而且那個類別真的是 IMKInputController。
        //
        // 少了 @objc(...) 時，Swift 會把類別名 mangle 成
        // `_TtC9LuminaKey24LuminaKeyInputController`，NSClassFromString 回傳 nil，
        // 而症狀是「輸入法裝得起來、選單裡看得到、一個字都打不出來，
        // 且完全沒有錯誤訊息」。
        let declared = Bundle.main.infoDictionary?["InputMethodServerControllerClass"] as? String
        check("Info.plist 宣告了 InputMethodServerControllerClass", declared != nil, declared ?? "缺")
        if let declared {
            let cls = NSClassFromString(declared)
            check("NSClassFromString(\"\(declared)\") 找得到類別（@objc(...) 有生效）", cls != nil)
            if let cls {
                check("\(declared) 是 IMKInputController 的子類別", cls is IMKInputController.Type)
            }
        }
        let conn = Bundle.main.infoDictionary?["InputMethodConnectionName"] as? String
        check("Info.plist 宣告了 InputMethodConnectionName", !(conn ?? "").isEmpty, conn ?? "缺")

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

        // 6. **中／英切換的事件真的收得到嗎。**
        //
        // 這一組驗的是 `LuminaKeyInputController.recognizedEvents(_:)`。
        // 少了那個 override,IMKit 的預設是「只有 keyDown」,而修飾鍵在 macOS
        // 不產生 keyDown —— 於是輕點 Shift 切中英**整條路徑不存在**,
        // 而畫面完全正常、單元測試也全綠(它們測的是純邏輯,不是 IMKit)。
        //
        // ⚠ 不是 grep 原始碼:那種守門被「名字在別處出現一次」餵飽過四次。
        //   這裡是**問這個二進位裡的類別實際回傳什麼**。
        let appkitMask = NSEvent.EventTypeMask.keyDown.rawValue
            | NSEvent.EventTypeMask.flagsChanged.rawValue
        check("事件遮罩常數與 AppKit 對得上", IMKRecognizedEvents.mask == appkitMask,
              "Kit \(IMKRecognizedEvents.mask) / AppKit \(appkitMask)")

        // ⚠ 變數名不要叫 declared —— 這個函式上面已經有一個
        //   (Info.plist 宣告的類別名),撞名是編譯錯誤。
        let probe = LuminaKeyInputController()
        let eventMask = UInt64(bitPattern: Int64(probe.recognizedEvents(nil)))
        check("controller 宣告要收 flagsChanged（輕點 Shift 才會有反應）",
              IMKRecognizedEvents.includesFlagsChanged(eventMask),
              String(format: "回傳 0x%llX", eventMask))
        check("controller 仍然要收 keyDown（少了它一個字都打不出來）",
              eventMask & IMKRecognizedEvents.keyDown != 0,
              String(format: "回傳 0x%llX", eventMask))

        // 7. **使用者初始配置的範本有沒有隨 .app 附上。**
        //
        // 沒有它的話,librime 會照上游 rime-prelude 的 default.yaml 部署:
        // 那份清單裡沒有我們實際打包的 luna_pinyin_tw / bopomofo_tw / t9_pinyin,
        // 卻列了兩個我們沒有打包的(cangjie5 / quick5)。
        // 後果是「引擎有方案、設定畫面一列都沒勾」,而且繁簡綁定挑不出方案。
        let resources = Bundle.main.resourceURL ?? URL(fileURLWithPath: ".")
        let templateDir = resources.appendingPathComponent(UserDataSeed.templateDirectoryName)
        let customURL = templateDir.appendingPathComponent(RimeConfigPatch.fileName)
        let templateText = (try? String(contentsOf: customURL, encoding: .utf8)) ?? ""
        let templateIds = RimeConfigPatch.readSchemaList(text: templateText)
        check("隨附了 \(UserDataSeed.templateDirectoryName)/\(RimeConfigPatch.fileName)",
              !templateIds.isEmpty, templateIds.joined(separator: ", "))

        // 範本裡列的每一個方案都要真的在 SharedSupport 裡,否則部署會報錯 ——
        // 那正是這一份範本存在的理由,寫錯了等於沒有它。
        let sharedDir = resources.appendingPathComponent("SharedSupport")
        let missing = templateIds.filter {
            !FileManager.default.fileExists(
                atPath: sharedDir.appendingPathComponent("\($0)\(SchemaCatalog.suffix)").path)
        }
        check("範本列的方案都有對應的 .schema.yaml", missing.isEmpty,
              missing.joined(separator: ", "))

        print(ok ? "=== self-check 通過 ===" : "=== self-check 失敗 ===")
        return ok
    }
}
