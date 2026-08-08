//
//  ZipReader.swift — 手寫的 ZIP central directory 讀取器 + raw DEFLATE
//
//  ── 為什麼手寫 ──────────────────────────────────────────────────────────
//  一個 ZIP 檔裡有**兩份**中繼資料:每個檔案前面的 local file header,
//  以及檔尾的 central directory。**兩者可以互相矛盾。** 如果驗證讀其中一份、
//  解壓讀另一份,驗證就是可以被繞過的 —— 這是 ZIP 的經典攻擊面。
//
//  所以本檔的紀律是:**驗證與解壓都以 central directory 為準**,
//  local header 只用來找資料從哪個位元組開始,而且**檔名必須一致**,
//  不一致就整包拒絕。
//
//  第二個理由:Foundation 沒有 ZIP 讀取器,而現成的 Swift ZIP 套件多半
//  跟著 local header 走(那是串流解壓最自然的做法),用了就等於把上面那條
//  紀律交出去。第三個理由:符號連結的位元藏在 external file attributes 裡,
//  不自己讀 central directory 根本拿不到。
//
//  ⚠ **ZIP64 一律拒絕。** 本專案的套件有 256 MiB 的上限,需要 ZIP64
//  代表要嘛打包出了問題,要嘛有人在試探。
//

import Foundation
import Compression

public struct ZipEntry: Equatable, Sendable {
    public let name: String
    public let compressedSize: Int64
    public let uncompressedSize: Int64
    /// 0 = stored,8 = deflate。其餘一律不支援。
    public let method: Int
    public let localHeaderOffset: Int64
    /// external file attributes 的高 16 位(Unix mode)。
    public let unixMode: Int
    /// version made by 的高位元組。3 = Unix。
    public let hostOS: Int

    public var isDirectory: Bool {
        name.hasSuffix("/") || (hostOS == 3 && (unixMode & 0xF000) == 0x4000)
    }
    /// 非 Unix 產生的 zip 沒有符號連結這個概念,不可能是連結。
    public var isSymlink: Bool { hostOS == 3 && (unixMode & 0xF000) == 0xA000 }
}

public struct ZipError: Error, Equatable {
    public let detail: String
    public init(_ d: String) { detail = d }
}

public enum ZipReader {

    static let eocdSig: UInt32 = 0x0605_4b50
    static let cdSig: UInt32 = 0x0201_4b50
    static let localSig: UInt32 = 0x0403_4b50
    static let zip64LocatorSig: UInt32 = 0x0706_4b50
    static let maxCentralDirectoryBytes = 16 * 1024 * 1024

    // MARK: - central directory

    public static func readCentralDirectory(_ url: URL) throws -> [ZipEntry] {
        guard let handle = try? FileHandle(forReadingFrom: url) else {
            throw ZipError("檔案打不開:\(url.lastPathComponent)")
        }
        defer { try? handle.close() }
        let length = Int64((try? FileManager.default.attributesOfItem(atPath: url.path)[.size]
                            as? Int) ?? 0)
        guard length >= 22 else { throw ZipError("檔案太小,不可能是 zip(\(length) bytes)") }

        let tailLen = Int(min(length, Int64(0xFFFF + 22)))
        try handle.seek(toOffset: UInt64(length - Int64(tailLen)))
        let tail = [UInt8]((try? handle.read(upToCount: tailLen)) ?? Data())
        guard tail.count == tailLen else { throw ZipError("讀不到檔尾") }

        // 從後往前找最後一個 EOCD。往前找是必須的:zip 的註解裡可以有
        // 一段看起來像 EOCD 的位元組,往前找才會取到真的那一個。
        var eocd = -1
        var k = tail.count - 22
        while k >= 0 {
            if u32(tail, k) == eocdSig { eocd = k; break }
            k -= 1
        }
        guard eocd >= 0 else { throw ZipError("找不到 End Of Central Directory,不是 zip 檔") }

        if eocd >= 20, u32(tail, eocd - 20) == zip64LocatorSig {
            throw ZipError("ZIP64 格式不受支援(本專案的套件不應該大到需要它)")
        }
        let count = Int(u16(tail, eocd + 10))
        let cdSize = Int64(u32(tail, eocd + 12))
        let cdOffset = Int64(u32(tail, eocd + 16))
        guard count != 0xFFFF, cdSize != 0xFFFF_FFFF, cdOffset != 0xFFFF_FFFF else {
            throw ZipError("central directory 使用 ZIP64 欄位,不受支援")
        }
        guard cdOffset + cdSize <= length else {
            throw ZipError("central directory 位置超出檔案範圍")
        }
        guard cdSize <= Int64(maxCentralDirectoryBytes) else {
            throw ZipError("central directory 過大")
        }

        try handle.seek(toOffset: UInt64(cdOffset))
        let cd = [UInt8]((try? handle.read(upToCount: Int(cdSize))) ?? Data())
        guard cd.count == Int(cdSize) else { throw ZipError("central directory 被截斷") }

        var out: [ZipEntry] = []
        var p = 0
        while p + 46 <= cd.count {
            guard u32(cd, p) == cdSig else { break }
            let versionMadeBy = Int(u16(cd, p + 4))
            let method = Int(u16(cd, p + 10))
            let compSize = Int64(u32(cd, p + 20))
            let uncompSize = Int64(u32(cd, p + 24))
            let nameLen = Int(u16(cd, p + 28))
            let extraLen = Int(u16(cd, p + 30))
            let commentLen = Int(u16(cd, p + 32))
            let externalAttrs = u32(cd, p + 38)
            let localOffset = Int64(u32(cd, p + 42))
            guard p + 46 + nameLen <= cd.count else { throw ZipError("central directory 被截斷") }
            let nameBytes = Array(cd[(p + 46)..<(p + 46 + nameLen)])
            let name = String(decoding: nameBytes, as: UTF8.self)
            out.append(ZipEntry(name: name,
                                compressedSize: compSize,
                                uncompressedSize: uncompSize,
                                method: method,
                                localHeaderOffset: localOffset,
                                unixMode: Int((externalAttrs >> 16) & 0xFFFF),
                                hostOS: (versionMadeBy >> 8) & 0xFF))
            p += 46 + nameLen + extraLen + commentLen
        }
        guard out.count == count else {
            throw ZipError("central directory 宣告 \(count) 項,實際讀到 \(out.count) 項")
        }
        return out
    }

    // MARK: - 取出一筆資料

    /// 把 `entry` 的內容解出來,逐塊交給 `sink`。
    ///
    /// `sink` 回 false = 呼叫端喊停(超過上限),本函式立刻拋出 `.aborted`。
    ///
    /// ⚠ **檔名比對**:local header 的檔名必須與 central directory 的一致。
    /// 兩份中繼資料矛盾就是攻擊訊號,不是格式寬容度的問題。
    public static func extract(entry: ZipEntry, from url: URL,
                               sink: (Data) -> Bool) throws {
        guard let handle = try? FileHandle(forReadingFrom: url) else {
            throw ZipError("檔案打不開")
        }
        defer { try? handle.close() }
        try handle.seek(toOffset: UInt64(entry.localHeaderOffset))
        let head = [UInt8]((try? handle.read(upToCount: 30)) ?? Data())
        guard head.count == 30, u32(head, 0) == localSig else {
            throw ZipError("local file header 不正確:\(entry.name)")
        }
        let nameLen = Int(u16(head, 26))
        let extraLen = Int(u16(head, 28))
        let nameBytes = [UInt8]((try? handle.read(upToCount: nameLen)) ?? Data())
        let localName = String(decoding: nameBytes, as: UTF8.self)
        guard localName == entry.name else {
            throw ZipError("local header 的檔名(\(localName))與 central directory"
                           + "(\(entry.name))不一致")
        }
        try handle.seek(toOffset: UInt64(entry.localHeaderOffset) + 30
                        + UInt64(nameLen) + UInt64(extraLen))

        switch entry.method {
        case 0:
            var remaining = entry.compressedSize
            while remaining > 0 {
                let n = Int(min(remaining, 64 * 1024))
                guard let chunk = try handle.read(upToCount: n), !chunk.isEmpty else { break }
                remaining -= Int64(chunk.count)
                guard sink(chunk) else { throw ZipError("已中止") }
            }
        case 8:
            try Inflate.raw(handle: handle, compressedSize: entry.compressedSize, sink: sink)
        default:
            throw ZipError("不支援的壓縮方式 \(entry.method)(只支援 stored 與 deflate)")
        }
    }

    // MARK: - 小端序讀取

    static func u16(_ b: [UInt8], _ i: Int) -> UInt16 {
        guard i >= 0, i + 1 < b.count else { return 0 }
        return UInt16(b[i]) | UInt16(b[i + 1]) << 8
    }
    static func u32(_ b: [UInt8], _ i: Int) -> UInt32 {
        guard i >= 0, i + 3 < b.count else { return 0 }
        return UInt32(b[i]) | UInt32(b[i + 1]) << 8 | UInt32(b[i + 2]) << 16 | UInt32(b[i + 3]) << 24
    }
}

// MARK: - raw DEFLATE

/// Apple 的 Compression framework,`COMPRESSION_ZLIB` 是**裸 DEFLATE**
/// (沒有 zlib 的兩位元組表頭),正好就是 ZIP 裡存的東西。
enum Inflate {

    static let bufferSize = 64 * 1024

    static func raw(handle: FileHandle, compressedSize: Int64, sink: (Data) -> Bool) throws {
        var stream = compression_stream(dst_ptr: UnsafeMutablePointer<UInt8>(bitPattern: 1)!,
                                        dst_size: 0,
                                        src_ptr: UnsafePointer<UInt8>(bitPattern: 1)!,
                                        src_size: 0, state: nil)
        guard compression_stream_init(&stream, COMPRESSION_STREAM_DECODE, COMPRESSION_ZLIB)
                == COMPRESSION_STATUS_OK else {
            throw ZipError("解壓縮器初始化失敗")
        }
        defer { compression_stream_destroy(&stream) }

        let dst = UnsafeMutablePointer<UInt8>.allocate(capacity: bufferSize)
        defer { dst.deallocate() }

        var remaining = compressedSize
        var input = Data()
        var srcIndex = 0
        var finished = false

        while !finished {
            if stream.src_size == 0 {
                if remaining > 0 {
                    let n = Int(min(remaining, Int64(bufferSize)))
                    input = (try handle.read(upToCount: n)) ?? Data()
                    if input.isEmpty { remaining = 0 } else { remaining -= Int64(input.count) }
                    srcIndex = 0
                } else {
                    input = Data()
                }
            }

            let status: compression_status = input.isEmpty && stream.src_size == 0
                ? inflateStep(&stream, dst: dst, src: nil, srcCount: 0, finalise: true)
                : input.withUnsafeBytes { (raw: UnsafeRawBufferPointer) -> compression_status in
                    let base = raw.bindMemory(to: UInt8.self).baseAddress!
                    return inflateStep(&stream, dst: dst,
                                       src: base + srcIndex,
                                       srcCount: input.count - srcIndex,
                                       finalise: remaining == 0)
                }

            let produced = bufferSize - stream.dst_size
            if produced > 0 {
                guard sink(Data(bytes: dst, count: produced)) else { throw ZipError("已中止") }
            }
            srcIndex = input.count - stream.src_size

            switch status {
            case COMPRESSION_STATUS_END: finished = true
            case COMPRESSION_STATUS_OK:
                // 沒有輸入、也沒有產出 → 資料被截斷了,再轉下去會空轉。
                if produced == 0, stream.src_size == 0, remaining == 0 {
                    throw ZipError("壓縮資料被截斷")
                }
            default: throw ZipError("解壓縮失敗")
            }
        }
    }

    private static func inflateStep(_ stream: inout compression_stream,
                                    dst: UnsafeMutablePointer<UInt8>,
                                    src: UnsafePointer<UInt8>?, srcCount: Int,
                                    finalise: Bool) -> compression_status {
        stream.dst_ptr = dst
        stream.dst_size = bufferSize
        if let src {
            stream.src_ptr = src
            stream.src_size = srcCount
        }
        let flags = finalise ? Int32(COMPRESSION_STREAM_FINALIZE.rawValue) : 0
        return compression_stream_process(&stream, flags)
    }
}
