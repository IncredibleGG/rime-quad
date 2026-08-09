//
//  tis_probe.swift — 問系統:「你認得這個輸入法嗎?」
//
//  ── 這支程式存在的理由 ──────────────────────────────────────────────────
//  這個專案(以及 Windows 端)都犯過同一個錯:驗了「bundle 結構對、
//  宣告齊全」,但那些**全都是在打包好的檔案上驗的** —— 沒有任何一關問過
//  「放到正確位置之後,系統認不認」。所以「裝在錯的地方」這件事,
//  自動化一句話都不會說。
//
//  Text Input Services 是 macOS 這一側的答案:`TISCreateInputSourceList`
//  列出系統目前認得的所有輸入來源。認得 = 使用者在「系統設定 › 鍵盤 ›
//  輸入來源」的 + 清單裡看得到。
//
//  ⚠ **這支程式在 CI 上很可能查不到我們的輸入法,而那不是失敗。**
//    TIS 掃描 `~/Library/Input Methods` 的時機綁在**有登入的圖形工作階段**,
//    而 GitHub runner 是背景工作階段。查不到時 verify_pkg.sh 會把原因印出來,
//    不會判失敗 —— 「試過了、原因是這個」本身就是結論。
//    真的要驗這一層,只有人在自己的 Mac 上做得到。
//
//  用法:
//      swiftc -O -o /tmp/tis_probe apple/scripts/tis_probe.swift
//      /tmp/tis_probe            # 印出所有輸入來源 id
//      /tmp/tis_probe org.luminakey.inputmethod.LuminaKey.Hant   # 只找這一個
//
import Carbon
import Foundation

func stringProperty(_ source: TISInputSource, _ key: CFString) -> String? {
    guard let ptr = TISGetInputSourceProperty(source, key) else { return nil }
    return Unmanaged<CFString>.fromOpaque(ptr).takeUnretainedValue() as String
}

let wanted = CommandLine.arguments.dropFirst().first

// includeAllInstalled = true:連「還沒被使用者加進輸入來源清單」的也算,
// 那正是我們要問的問題(系統看得到它嗎),而不是「使用者啟用了嗎」。
guard let raw = TISCreateInputSourceList(nil, true)?.takeRetainedValue() else {
    FileHandle.standardError.write(Data("TISCreateInputSourceList 回傳 nil\n".utf8))
    exit(2)
}
let sources = raw as! [TISInputSource]

var matched = false
var badName = false
var total = 0
for source in sources {
    guard let id = stringProperty(source, kTISPropertyInputSourceID) else { continue }
    total += 1
    let isLuminaKey = id.hasPrefix("org.luminakey.")
    if let wanted {
        if id == wanted { matched = true; print("FOUND \(id)") }
    } else if isLuminaKey {
        matched = true
        let name = stringProperty(source, kTISPropertyLocalizedName) ?? ""
        // ⚠ 這一行就是真機回報的那個缺陷:沒有 .lproj 時系統拿不到
        //   在地化名稱,清單裡顯示的就是這串 id 本身。
        if name.isEmpty || name == id {
            print("BAD-NAME \(id)  —  在地化名稱等於 id(清單裡會顯示 bundle id)")
            badName = true
        } else {
            print("FOUND \(id)  —  \(name)")
        }
    }
}

print("TIS 一共列出 \(total) 個輸入來源")
if !matched {
    print("清單裡沒有 org.luminakey.*")
}
// 結束碼:0 = 找到而且名字正常;3 = 找到但名字就是 id;4 = 完全找不到。
// ⚠ 之前這支程式一律回 0,因為我們**以為** runner 上查不到。
//   實測結果相反:TIS 在 GitHub runner 上看得到 ~/Library/Input Methods
//   裡的輸入法,連在地化名稱都解得出來。既然查得到,就該把它當關卡,
//   而不是當成一份「參考資訊」。
if badName { exit(3) }
exit(matched ? 0 : 4)
