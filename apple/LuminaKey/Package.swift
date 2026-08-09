// swift-tools-version:5.9
//
// LuminaKeyKit — macOS 端的**純邏輯**層。
//
// ⚠ 這個 package 刻意**不含** AppKit / InputMethodKit / librime。
//   理由：CI 上沒有登入的圖形工作階段，UI 驗不了；能純邏輯化的東西
//   （主題解析、keysym 映射、候選窗排版、上屏狀態機）就必須離開 UI，
//   否則它們也一起變成「只有人在真 Mac 上跑才驗得到」。
//
//   App 的原始碼在 ../LuminaKey/AppSources/，由 apple/scripts/build_app.sh
//   用 swiftc 直接編（它要連 librime 靜態庫與 IMKit，SwiftPM 幫不上忙）。
//   兩邊編的是**同一份** LuminaKeyKit 原始碼，不是複製品。
//
import PackageDescription

let package = Package(
    name: "LuminaKeyKit",
    platforms: [.macOS(.v11)],
    products: [
        .library(name: "LuminaKeyKit", targets: ["LuminaKeyKit"]),
    ],
    targets: [
        .target(name: "LuminaKeyKit", path: "Sources/LuminaKeyKit"),
        .testTarget(name: "LuminaKeyKitTests", dependencies: ["LuminaKeyKit"],
                    path: "Tests/LuminaKeyKitTests"),
    ]
)
