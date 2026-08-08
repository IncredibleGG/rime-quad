//
//  main.swift — IMKServer 的啟動點
//
//  ⚠ IMKServer **必須**被一個活著的參照持有。放進區域變數再返回，
//    輸入法會裝得起來、選得到、但一按鍵就沒有反應（連線已經沒了）。
//

import AppKit
import InputMethodKit

// CI 用的自我檢查模式：不啟動 NSApplication，跑完就結束。
if CommandLine.arguments.contains("--self-check") {
    exit(SelfCheck.run() ? 0 : 1)
}

guard let bundleId = Bundle.main.bundleIdentifier else {
    NSLog("RimeQuad: 沒有 bundle identifier —— 這個執行檔必須從 .app 裡跑")
    exit(1)
}
guard let connectionName =
        Bundle.main.infoDictionary?["InputMethodConnectionName"] as? String else {
    NSLog("RimeQuad: Info.plist 缺少 InputMethodConnectionName")
    exit(1)
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate

// 全域，活到行程結束。
let server: IMKServer? = IMKServer(name: connectionName, bundleIdentifier: bundleId)
guard let liveServer = server else {
    NSLog("RimeQuad: IMKServer 建立失敗（連線名 \(connectionName)）")
    exit(1)
}
ServerHolder.instance = liveServer

app.run()
