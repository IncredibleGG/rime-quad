//
//  ArchiveGuard.swift — 方案套件的解壓守門
//
//  第三方方案是**別人寫的、從網路下載的、而且含有可執行的 Lua**。
//  這一層假設每一個壓縮檔都是敵意的,而且**任何一項不合格就整包拒絕** ——
//  不是「跳過那一個檔案繼續裝」,因為半套的方案部署起來會失敗,
//  而失敗訊息與被跳過的那個檔案之間沒有任何線索。
//
//  ── 兩層防線,缺一不可 ──────────────────────────────────────────────────
//  1. **宣告值的檢查**(inspect):central directory 說這個檔案解開有多大。
//  2. **實際位元組的檢查**(extract):一邊解一邊數。
//     宣告的大小是攻擊者控制的,只驗第一層等於沒驗。
//
//  路徑同理:字串比對(`pathProblem`)與正規化之後的範圍檢查(`isInside`)
//  是**互相獨立**的兩個機制,其中一個被繞過另一個還在。
//
//  規則與常數逐項照抄 Android 的 ArchiveGuard —— 同一個套件在兩端
//  必須得到同一個判定,否則「手機裝得起來、電腦裝不起來」會變成
//  一件沒有人解釋得了的事。
//

import Foundation

public struct ArchiveLimits: Equatable, Sendable {
    public var maxEntries = 2_000
    public var maxEntryBytes: Int64 = 64 * 1024 * 1024
    public var maxTotalBytes: Int64 = 256 * 1024 * 1024
    public var maxCompressionRatio: Int64 = 200
    /// 壓縮比只在壓縮後大小達到這個門檻時才判定 —— 一個 12 位元組的
    /// 檔案壓成 1 位元組是 12:1,那不是炸彈,那是小檔案。
    public var ratioFloorBytes: Int64 = 4 * 1024
    public var maxPathLength = 255
    public var maxDepth = 4

    /// 副檔名白名單(小寫,不含點)。
    ///
    /// ⚠ **`bin` 刻意不在裡面。** `.table.bin` / `.prism.bin` 是 librime
    /// 部署時自己產生的東西,套件裡帶它們沒有意義,而那正是藏二進位酬載
    /// 最好的地方。
    ///
    /// ⚠ **`lua` 刻意在裡面。** librime-lua 有連進來,而現代方案
    /// (雾凇拼音、萬象)沒有 `lua/` 目錄就只是個空殼。這等於明知故犯地
    /// 收下可執行碼 —— 理由是它跑在 librime 自己的 Lua 狀態裡,權限不比
    /// 同一個 zip 裡的 yaml 高,而且移植 librime-lua 時**必須**套用
    /// `patches/librime-lua@sandbox.patch`(見 coordination.md §3)。
    /// 路徑穿越、符號連結、解壓炸彈的防線都不因此改變。
    public var allowedExtensions: Set<String> = [
        "yaml", "yml", "txt", "ocd2", "gram", "json", "md", "lua",
    ]
    /// 沒有副檔名但允許的檔名。**區分大小寫**。
    public var allowedBareNames: Set<String> = [
        "LICENSE", "LICENCE", "COPYING", "NOTICE", "README", "AUTHORS", "CHANGELOG",
    ]

    public init() {}
}

public struct ArchiveRejection: Equatable, Sendable {
    public enum Kind: String, Sendable {
        case malformed, pathTraversal, symlink, zipBomb, extensionNotAllowed, empty
    }
    public let kind: Kind
    /// nil = 這個問題屬於整個壓縮檔,不是某一筆。
    public let entry: String?
    public let detail: String

    public var message: T {
        switch kind {
        case .malformed:
            return T("壓縮檔損毀或格式不正確:\(detail)", "压缩档损坏或格式不正确:\(detail)",
                     "The archive is damaged or not a valid zip: \(detail)")
        case .pathTraversal:
            return T("壓縮檔含有會寫到目標目錄之外的路徑「\(entry ?? "?")」,已整包拒絕",
                     "压缩档含有会写到目标目录之外的路径「\(entry ?? "?")」,已整包拒绝",
                     "The archive contains a path that would escape the target directory (\(entry ?? "?")). Rejected.")
        case .symlink:
            return T("壓縮檔含有符號連結「\(entry ?? "?")」,已整包拒絕",
                     "压缩档含有符号链接「\(entry ?? "?")」,已整包拒绝",
                     "The archive contains a symlink (\(entry ?? "?")). Rejected.")
        case .zipBomb:
            return T("壓縮檔超出解壓上限:\(detail)", "压缩档超出解压上限:\(detail)",
                     "The archive exceeds the extraction limits: \(detail)")
        case .extensionNotAllowed:
            return T("壓縮檔含有不允許的檔案「\(entry ?? "?")」:\(detail)",
                     "压缩档含有不允许的文件「\(entry ?? "?")」:\(detail)",
                     "The archive contains a file that is not allowed (\(entry ?? "?")): \(detail)")
        case .empty:
            return T("壓縮檔裡沒有任何可用的檔案", "压缩档里没有任何可用的文件",
                     "The archive contains no usable files")
        }
    }
}

public struct ArchiveReport: Equatable, Sendable {
    public let entries: [ZipEntry]
    public let rejections: [ArchiveRejection]
    public var isSafe: Bool { rejections.isEmpty }
}

public enum ExtractResult: Equatable {
    case ok(files: [String], bytes: Int64)
    case rejected(ArchiveReport)
    case failed(String)
}

public enum ArchiveGuard {

    // MARK: - 檢查(不寫任何東西)

    /// **一項不合格就整包拒絕,但檢查會走完** —— 使用者自己準備的 zip
    /// 通常不只錯一個地方,一次講完比讓他改一次跑一次好。
    public static func inspect(_ url: URL, limits: ArchiveLimits = ArchiveLimits()) -> ArchiveReport {
        var rejections: [ArchiveRejection] = []
        var safe: [ZipEntry] = []

        let all: [ZipEntry]
        do {
            all = try ZipReader.readCentralDirectory(url)
        } catch let e as ZipError {
            return ArchiveReport(entries: [], rejections: [
                ArchiveRejection(kind: .malformed, entry: nil, detail: e.detail)])
        } catch {
            return ArchiveReport(entries: [], rejections: [
                ArchiveRejection(kind: .malformed, entry: nil, detail: "\(error)")])
        }

        if all.count > limits.maxEntries {
            rejections.append(ArchiveRejection(
                kind: .zipBomb, entry: nil,
                detail: "entry 數量 \(all.count) 超過上限 \(limits.maxEntries)"))
        }

        var total: Int64 = 0
        for e in all {
            if let problem = pathProblem(e.name, limits: limits) {
                rejections.append(ArchiveRejection(kind: .pathTraversal, entry: e.name,
                                                   detail: problem))
                continue
            }
            if e.isSymlink {
                rejections.append(ArchiveRejection(
                    kind: .symlink, entry: e.name,
                    detail: "external attributes 的 unix mode 為 0"
                            + String(e.unixMode, radix: 8) + "(S_IFLNK)"))
                continue
            }
            if e.isDirectory { continue }
            if let problem = extensionProblem(e.name, limits: limits) {
                rejections.append(ArchiveRejection(kind: .extensionNotAllowed, entry: e.name,
                                                   detail: problem))
                continue
            }
            if e.uncompressedSize > limits.maxEntryBytes {
                rejections.append(ArchiveRejection(
                    kind: .zipBomb, entry: e.name,
                    detail: "宣告的解壓後大小 \(e.uncompressedSize) 超過單檔上限 \(limits.maxEntryBytes)"))
                continue
            }
            if e.compressedSize >= limits.ratioFloorBytes, e.compressedSize > 0 {
                let ratio = e.uncompressedSize / e.compressedSize
                if ratio > limits.maxCompressionRatio {
                    rejections.append(ArchiveRejection(
                        kind: .zipBomb, entry: e.name,
                        detail: "壓縮比 \(ratio):1 超過上限 \(limits.maxCompressionRatio):1"))
                    continue
                }
            }
            total += e.uncompressedSize
            if total > limits.maxTotalBytes {
                rejections.append(ArchiveRejection(
                    kind: .zipBomb, entry: nil,
                    detail: "宣告的解壓後總大小已超過上限 \(limits.maxTotalBytes)"))
                break
            }
            safe.append(e)
        }

        if rejections.isEmpty, safe.isEmpty {
            rejections.append(ArchiveRejection(kind: .empty, entry: nil, detail: "沒有通過檢查的檔案"))
        }
        return ArchiveReport(entries: safe, rejections: rejections)
    }

    // MARK: - 路徑

    /// 回傳問題的描述,`nil` = 這個路徑沒問題。
    public static func pathProblem(_ raw: String, limits: ArchiveLimits = ArchiveLimits()) -> String? {
        if raw.isEmpty { return "entry 名稱為空" }
        if raw.count > limits.maxPathLength {
            return "路徑長度 \(raw.count) 超過上限 \(limits.maxPathLength)"
        }
        for u in raw.unicodeScalars where u.value < 0x20 || u.value == 0x7F {
            return "路徑含控制字元"
        }
        // ZIP 規定分隔符是 `/`。反斜線本身就可疑,而且在 macOS 上它是
        // 合法的檔名字元 —— 一個叫 `..\..\evil` 的「檔名」躲得過段落檢查。
        if raw.contains("\\") { return "路徑含反斜線" }
        if raw.hasPrefix("/") { return "路徑以 / 開頭(絕對路徑)" }
        let chars = Array(raw)
        if chars.count >= 2, chars[1] == ":", chars[0].isLetter {
            return "路徑含磁碟機代號(絕對路徑)"
        }
        let segments = raw.components(separatedBy: "/")
        for (i, seg) in segments.enumerated() {
            if seg == ".." { return "路徑含 .. 區段" }
            if seg == "." { return "路徑含 . 區段" }
            if seg.isEmpty, i != segments.count - 1 { return "路徑含連續斜線" }
            if seg.hasSuffix(" ") || seg.hasSuffix(".") {
                return "路徑區段「\(seg)」以空白或點結尾"
            }
        }
        let depth = segments.filter { !$0.isEmpty }.count
        if depth > limits.maxDepth { return "目錄深度 \(depth) 超過上限 \(limits.maxDepth)" }
        return nil
    }

    public static func extensionProblem(_ name: String,
                                        limits: ArchiveLimits = ArchiveLimits()) -> String? {
        let base = name.components(separatedBy: "/").last ?? ""
        if base.isEmpty { return "檔名為空" }
        // 擋掉 dotfile,順便擋掉「整個檔名就只是一個副檔名」的 `.yaml`。
        if base.hasPrefix(".") { return "不接受以點開頭的隱藏檔" }
        guard let dot = base.lastIndex(of: ".") else {
            return limits.allowedBareNames.contains(base)
                ? nil
                : "沒有副檔名且不在允許的檔名清單(\(limits.allowedBareNames.sorted().joined(separator: ", ")))"
        }
        let ext = String(base[base.index(after: dot)...]).lowercased()
        guard limits.allowedExtensions.contains(ext) else {
            return "副檔名 .\(ext) 不在白名單(允許:"
                   + limits.allowedExtensions.sorted().joined(separator: ", ") + ")"
        }
        return nil
    }

    /// 正規化之後 `candidate` 是不是真的在 `root` 底下。
    ///
    /// ⚠ **兩邊都要先正規化**(解掉符號連結、去掉 `.`/`..`)。
    /// 而且 APFS 預設**不分大小寫** —— 只做字串前綴比對的話,
    /// 一個大小寫不同的路徑可以繞過去。所以比對前先把兩邊都
    /// 標準化,並且用大小寫不敏感的比對。
    public static func isInside(_ candidate: URL, root: URL) -> Bool {
        let c = candidate.resolvingSymlinksInPath().standardizedFileURL.path
        var r = root.resolvingSymlinksInPath().standardizedFileURL.path
        if c.compare(r, options: .caseInsensitive) == .orderedSame { return true }
        if !r.hasSuffix("/") { r += "/" }
        return c.count > r.count
            && c.prefix(r.count).compare(r, options: .caseInsensitive) == .orderedSame
    }

    // MARK: - 解壓

    /// 解壓到 `targetDir`。
    ///
    /// **先全部解到暫存目錄,全部成功之後才搬過去。** 半寫入的檔案不是無害的:
    /// 一本被截斷的 `.dict.yaml` 會讓之後**每一次**部署都失敗,
    /// 而使用者完全看不出是哪一次安裝造成的。
    public static func extract(_ zip: URL, to targetDir: URL, stagingParent: URL,
                               limits: ArchiveLimits = ArchiveLimits()) -> ExtractResult {
        let report = inspect(zip, limits: limits)
        guard report.isSafe else { return .rejected(report) }

        let fm = FileManager.default
        let staging = stagingParent.appendingPathComponent("unzip-\(UUID().uuidString)")
        guard (try? fm.createDirectory(at: staging, withIntermediateDirectories: true)) != nil else {
            return .failed("無法建立暫存目錄 \(staging.path)")
        }
        defer { try? fm.removeItem(at: staging) }

        var written: Int64 = 0
        var names: [String] = []

        for entry in report.entries {
            let dest = staging.appendingPathComponent(entry.name)
            // 第二道、與字串比對**互相獨立**的防線。
            guard isInside(dest, root: staging) || isInside(dest.deletingLastPathComponent(),
                                                            root: staging) else {
                return .rejected(ArchiveReport(entries: [], rejections: [
                    ArchiveRejection(kind: .pathTraversal, entry: entry.name,
                                     detail: "正規化後跳出了目標目錄")]))
            }
            try? fm.createDirectory(at: dest.deletingLastPathComponent(),
                                    withIntermediateDirectories: true)
            fm.createFile(atPath: dest.path, contents: nil)
            guard let handle = try? FileHandle(forWritingTo: dest) else {
                return .failed("無法建立檔案 \(entry.name)")
            }

            var entryBytes: Int64 = 0
            var overflow: String?
            do {
                try ZipReader.extract(entry: entry, from: zip) { chunk in
                    entryBytes += Int64(chunk.count)
                    // ⚠ 數的是**真的讀到的位元組**,不是 central directory 宣告的。
                    if entryBytes > limits.maxEntryBytes {
                        overflow = "\(entry.name) 實際解壓超過單檔上限 "
                                 + "\(limits.maxEntryBytes)(宣告的大小說謊了)"
                        return false
                    }
                    if written + entryBytes > limits.maxTotalBytes {
                        overflow = "解壓總量超過上限(宣告的大小說謊了)"
                        return false
                    }
                    try? handle.write(contentsOf: chunk)
                    return true
                }
                try? handle.close()
            } catch {
                try? handle.close()
                try? fm.removeItem(at: dest)
                if let overflow {
                    return .rejected(ArchiveReport(entries: [], rejections: [
                        ArchiveRejection(kind: .zipBomb, entry: entry.name, detail: overflow)]))
                }
                let d = (error as? ZipError)?.detail ?? "\(error)"
                return .failed("解壓 \(entry.name) 失敗:\(d)")
            }
            written += entryBytes
            names.append(entry.name)
        }

        // 全部成功了才搬。
        for name in names {
            let from = staging.appendingPathComponent(name)
            let to = targetDir.appendingPathComponent(name)
            guard isInside(to, root: targetDir) || isInside(to.deletingLastPathComponent(),
                                                            root: targetDir) else {
                return .failed("目標路徑跳出了使用者目錄:\(name)")
            }
            do {
                try fm.createDirectory(at: to.deletingLastPathComponent(),
                                       withIntermediateDirectories: true)
                if fm.fileExists(atPath: to.path) { try fm.removeItem(at: to) }
                try fm.moveItem(at: from, to: to)
            } catch {
                // 跨檔案系統時 move 會失敗,退回複製。
                if (try? fm.copyItem(at: from, to: to)) == nil {
                    return .failed("搬進 \(targetDir.lastPathComponent) 失敗:\(name)")
                }
            }
        }
        return .ok(files: names, bytes: written)
    }
}
