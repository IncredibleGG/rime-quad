//
//  main.swift — 兩個入口,一份執行檔
//
//  ⚠ IMKServer **必須**被一個活著的參照持有。放進區域變數再返回,
//    輸入法會裝得起來、選得到、但一按鍵就沒有反應(連線已經沒了)。
//
//  ── 為什麼一份執行檔會有兩種行為 ────────────────────────────────────────
//  設定介面必須是**另一個 .app**:輸入法本體的 Info.plist 是
//  `LSBackgroundOnly`,那種行程照定義不能被帶到前景,設定視窗裡的文字框
//  拿不到鍵盤焦點(而「詞庫」頁整頁都是文字框)。
//
//  但沒有必要編兩份二進位:同一份執行檔複製到
//  `RimeQuad.app/Contents/Resources/RimeQuadSettings.app/Contents/MacOS/`,
//  靠 bundle id 分岔。這樣兩邊永遠是同一版程式碼,不會有「設定介面
//  用的是上一版的解析器」這種只在使用者機器上出現的落差。
//

import AppKit
import InputMethodKit

// CI 用的自我檢查模式:不啟動 NSApplication,跑完就結束。
if CommandLine.arguments.contains("--self-check") {
    exit(SelfCheck.run() ? 0 : 1)
}

guard let bundleId = Bundle.main.bundleIdentifier else {
    NSLog("RimeQuad: 沒有 bundle identifier —— 這個執行檔必須從 .app 裡跑")
    exit(1)
}

// ⚠ 這個後綴與 SettingsSources 的 Info.plist、build_app.sh、
//   verify_app_bundle.sh 的斷言必須一致。
if bundleId.hasSuffix(".Settings") {
    SettingsAppMain.run()
}

guard let connectionName =
        Bundle.main.infoDictionary?["InputMethodConnectionName"] as? String else {
    NSLog("RimeQuad: Info.plist 缺少 InputMethodConnectionName")
    exit(1)
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate

// 全域,活到行程結束。
let server: IMKServer? = IMKServer(name: connectionName, bundleIdentifier: bundleId)
guard let liveServer = server else {
    NSLog("RimeQuad: IMKServer 建立失敗(連線名 \(connectionName))")
    exit(1)
}
ServerHolder.instance = liveServer

app.run()
