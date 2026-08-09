//
//  StoreTests.swift — 市集索引、相依展開、壓縮檔守門
//

import XCTest
@testable import LuminaKeyKit

final class MiniJsonTests: XCTestCase {

    func testAcceptsComments() {
        // 索引是人在維護的,每筆套件旁邊寫一句註解是常態。
        let j = try? MiniJson.parse("""
        {
          // 行註解
          "a": 1, /* 區塊註解 */ "b": [true, null, "x"]
        }
        """)
        XCTAssertEqual(j?["a"]?.intValue, 1)
        XCTAssertEqual(j?["b"]?.arrayValue.count, 3)
    }

    /// 數字保留原始字串:`size` 是 64 位元,經過 Double 會失真。
    func testLargeIntegerKeepsPrecision() {
        let j = try? MiniJson.parse("{\"size\": 9007199254740993}")
        XCTAssertEqual(j?["size"]?.int64Value, 9_007_199_254_740_993)
    }

    /// 這些**不**放寬:它們是「檔案壞了」的訊號,吞掉等於把
    /// 「索引被截斷」變成「索引裡少了幾個方案」,而後者沒有人查得出來。
    func testStrictWhereItMatters() {
        for bad in ["{\"a\":1,}", "['x']", "{a:1}", "{\"a\":1} trailing",
                    "[1,2,]", "{\"a\": 01}", "\"unterminated"] {
            XCTAssertThrowsError(try MiniJson.parse(bad), "應該拒絕:\(bad)")
        }
    }

    func testUnicodeEscapesIncludingSurrogatePairs() {
        let j = try? MiniJson.parse("{\"s\": \"\\u4f60\\u597d\\ud83d\\ude00\"}")
        XCTAssertEqual(j?["s"]?.stringValue, "你好😀")
    }
}

final class StoreIndexTests: XCTestCase {

    private func index(_ text: String) -> (SchemaIndex?, [String], String?) {
        switch IndexParser.parse(text) {
        case .ok(let i, let w): return (i, w, nil)
        case .err(let m): return (nil, [], m)
        }
    }

    private let sha = String(repeating: "a", count: 64)

    func testMinimalIndex() {
        let (i, w, e) = index("""
        {"format_version": 1, "packages": [
          {"id": "luna", "name": "朙月拼音", "file": "luna.zip", "sha256": "\(sha)",
           "size": 400000, "schemas": [{"id": "luna_pinyin", "language": "zh-Hant"}]}
        ]}
        """)
        XCTAssertNil(e)
        XCTAssertEqual(i?.packages.count, 1)
        XCTAssertEqual(i?.packages[0].schemaIds, ["luna_pinyin"])
        XCTAssertEqual(i?.languageTags()["luna_pinyin"], "zh-Hant")
        XCTAssertTrue(w.isEmpty)
    }

    /// 只有四種情況整份拒絕。索引是伺服器產生的,使用者改不了 ——
    /// 因為其中一筆壞掉就整份不給用,會讓完全正常的三十幾個方案一起消失。
    func testWholeIndexRejectedOnlyForFourReasons() {
        XCTAssertNotNil(index("not json").2)
        XCTAssertNotNil(index("[1,2]").2)
        XCTAssertNotNil(index("{\"packages\": []}").2)                  // 缺 format_version
        XCTAssertNotNil(index("{\"format_version\": 2, \"packages\": []}").2)
    }

    func testBadPackagesAreDroppedWithWarnings() {
        let (i, w, e) = index("""
        {"format_version": 1, "packages": [
          {"name": "沒有 id", "file": "a.zip", "sha256": "\(sha)"},
          {"id": "no-file", "sha256": "\(sha)"},
          {"id": "no-sha", "file": "b.zip"},
          {"id": "bad-sha", "file": "c.zip", "sha256": "XYZ"},
          {"id": "good", "file": "d.zip", "sha256": "\(sha)"},
          {"id": "good", "file": "dup.zip", "sha256": "\(sha)"}
        ]}
        """)
        XCTAssertNil(e)
        XCTAssertEqual(i?.packages.map(\.id), ["good"])
        XCTAssertEqual(w.count, 5)
    }

    /// sha256 一律轉小寫再比;大寫的索引不該讓每一個套件都驗不過。
    func testSha256IsLowercased() {
        let upper = String(repeating: "A", count: 64)
        let (i, _, _) = index("""
        {"format_version": 1, "packages":
          [{"id": "x", "file": "x.zip", "sha256": "\(upper)"}]}
        """)
        XCTAssertEqual(i?.packages.first?.sha256, String(repeating: "a", count: 64))
    }

    /// requires 指到不存在的套件:**警告但保留**。索引可能引用
    /// 一個下一版才會出現的東西,那不是這一筆壞掉。
    func testUnknownRequiresWarnsButKeeps() {
        let (i, w, _) = index("""
        {"format_version": 1, "packages":
          [{"id": "a", "file": "a.zip", "sha256": "\(sha)", "requires": ["ghost"]}]}
        """)
        XCTAssertEqual(i?.packages.count, 1)
        XCTAssertEqual(w.count, 1)
    }

    func testFormatBytes() {
        XCTAssertEqual(formatBytes(512), "512 B")
        XCTAssertEqual(formatBytes(2048), "2 KB")
        XCTAssertEqual(formatBytes(5 * 1024 * 1024), "5.0 MB")
    }
}

final class DependencyResolverTests: XCTestCase {

    private let sha = String(repeating: "b", count: 64)

    private func makeIndex(_ packages: [(String, [String])]) -> SchemaIndex {
        SchemaIndex(formatVersion: 1, generatedAt: nil, baseURL: nil, categories: [],
                    packages: packages.map { id, requires in
                        StorePackage(id: id, name: id, category: "other", description: "",
                                     file: "\(id).zip", size: 100, sha256: sha,
                                     schemas: [StoreSchemaRef(id: "\(id)_schema", name: id,
                                                              languageTag: nil)],
                                     requires: requires, license: nil, upstream: nil,
                                     recommended: false, layoutNote: nil)
                    })
    }

    func testDependenciesComeFirst() {
        let idx = makeIndex([("app", ["base"]), ("base", [])])
        guard case .ok(let plan) = DependencyResolver.resolve(index: idx, selected: ["app"],
                                                             installed: []) else {
            return XCTFail("應該成功")
        }
        XCTAssertEqual(plan.toDownload.map(\.id), ["base", "app"])
    }

    /// ⚠ 相依成環是**合法的**,不是錯誤:真實索引裡 luna-pinyin 與 stroke
    /// 互相 requires(互為反查詞典)。安裝流程會先解壓完再部署一次。
    func testCyclesAreLegal() {
        let idx = makeIndex([("a", ["b"]), ("b", ["a"])])
        guard case .ok(let plan) = DependencyResolver.resolve(index: idx, selected: ["a"],
                                                             installed: []) else {
            return XCTFail("環不該讓解析失敗")
        }
        XCTAssertEqual(Set(plan.toDownload.map(\.id)), ["a", "b"])
        XCTAssertFalse(plan.cycles.isEmpty)
    }

    func testAlreadyInstalledIsSkipped() {
        let idx = makeIndex([("app", ["base"]), ("base", [])])
        guard case .ok(let plan) = DependencyResolver.resolve(index: idx, selected: ["app"],
                                                             installed: ["base"]) else {
            return XCTFail("應該成功")
        }
        XCTAssertEqual(plan.toDownload.map(\.id), ["app"])
        XCTAssertEqual(plan.alreadyInstalled, ["base"])
    }

    func testMissingDependencyIsReportedWithItsRequirer() {
        let idx = makeIndex([("app", ["ghost"])])
        guard case .missingDependency(let missing, let by) =
                DependencyResolver.resolve(index: idx, selected: ["app"], installed: []) else {
            return XCTFail("應該回報缺相依")
        }
        XCTAssertEqual(missing, "ghost")
        XCTAssertEqual(by, "app")
    }

    /// 估計值必須被標成估計值。實測只有一組樣本(約 32 倍)。
    func testInstalledSizeIsAnEstimate() {
        let idx = makeIndex([("a", [])])
        guard case .ok(let plan) = DependencyResolver.resolve(index: idx, selected: ["a"],
                                                             installed: []) else {
            return XCTFail()
        }
        XCTAssertEqual(plan.estimatedInstalledBytes,
                       plan.totalBytes * DependencyResolver.installedSizeMultiplier)
    }
}

final class ArchiveGuardTests: XCTestCase {

    func testPathTraversalIsRejected() {
        for bad in ["../evil.yaml", "a/../../evil.yaml", "/etc/passwd", "C:/x.yaml",
                    "a//b.yaml", "./a.yaml", "a\\b.yaml", "dir /x.yaml", "dir./x.yaml",
                    "a/b/c/d/e/f.yaml"] {
            XCTAssertNotNil(ArchiveGuard.pathProblem(bad), "應該拒絕:\(bad)")
        }
    }

    func testGoodPathsPass() {
        for good in ["luna_pinyin.schema.yaml", "opencc/t2s.json", "lua/init.lua",
                     "build/x.yaml", "LICENSE", "a/b/c/d.yaml"] {
            XCTAssertNil(ArchiveGuard.pathProblem(good), "不該拒絕:\(good)")
        }
    }

    func testControlCharactersAndLongPaths() {
        XCTAssertNotNil(ArchiveGuard.pathProblem("a\u{0}b.yaml"))
        XCTAssertNotNil(ArchiveGuard.pathProblem(String(repeating: "a", count: 300) + ".yaml"))
    }

    /// ⚠ `.bin` 刻意不在白名單:那是 librime 自己產生的東西,
    /// 套件裡帶它沒有意義,而那正是藏二進位酬載最好的地方。
    func testBinIsNotAllowed() {
        XCTAssertNotNil(ArchiveGuard.extensionProblem("luna.table.bin"))
        XCTAssertNotNil(ArchiveGuard.extensionProblem("evil.dylib"))
        XCTAssertNotNil(ArchiveGuard.extensionProblem("evil.sh"))
    }

    /// ⚠ `.lua` 刻意在白名單:現代方案沒有 lua/ 就是空殼。
    /// 這是明知故犯,理由與 sandbox patch 的關係見 ArchiveLimits 的註解。
    func testLuaIsAllowed() {
        XCTAssertNil(ArchiveGuard.extensionProblem("lua/rime.lua"))
        XCTAssertNil(ArchiveGuard.extensionProblem("a.dict.yaml"))
        XCTAssertNil(ArchiveGuard.extensionProblem("opencc/x.ocd2"))
        XCTAssertNil(ArchiveGuard.extensionProblem("LICENSE"))
    }

    func testHiddenFilesRejected() {
        XCTAssertNotNil(ArchiveGuard.extensionProblem(".DS_Store"))
        XCTAssertNotNil(ArchiveGuard.extensionProblem("dir/.hidden.yaml"))
        XCTAssertNotNil(ArchiveGuard.extensionProblem("dir/.yaml"))
    }

    func testBareNamesAreCaseSensitive() {
        XCTAssertNil(ArchiveGuard.extensionProblem("README"))
        XCTAssertNotNil(ArchiveGuard.extensionProblem("readme"))
    }

    /// isInside 必須在**正規化之後**比,而且 APFS 預設不分大小寫。
    func testIsInside() {
        let root = URL(fileURLWithPath: "/tmp/luminakey-root")
        XCTAssertTrue(ArchiveGuard.isInside(root.appendingPathComponent("a.yaml"), root: root))
        XCTAssertTrue(ArchiveGuard.isInside(root, root: root))
        XCTAssertFalse(ArchiveGuard.isInside(URL(fileURLWithPath: "/tmp/luminakey-rootless/a"),
                                             root: root))
        XCTAssertFalse(ArchiveGuard.isInside(URL(fileURLWithPath: "/tmp/other"), root: root))
    }

    func testLimitsMatchAndroid() {
        // 同一個套件在兩端必須得到同一個判定,否則「手機裝得起來、
        // 電腦裝不起來」會變成沒有人解釋得了的事。
        let l = ArchiveLimits()
        XCTAssertEqual(l.maxEntries, 2_000)
        XCTAssertEqual(l.maxEntryBytes, 64 * 1024 * 1024)
        XCTAssertEqual(l.maxTotalBytes, 256 * 1024 * 1024)
        XCTAssertEqual(l.maxCompressionRatio, 200)
        XCTAssertEqual(l.ratioFloorBytes, 4 * 1024)
        XCTAssertEqual(l.maxPathLength, 255)
        XCTAssertEqual(l.maxDepth, 4)
    }

    func testMalformedArchiveIsRejectedNotCrashed() {
        let tmp = FileManager.default.temporaryDirectory
            .appendingPathComponent("not-a-zip-\(UUID().uuidString).zip")
        try? Data("這不是一個 zip 檔".utf8).write(to: tmp)
        defer { try? FileManager.default.removeItem(at: tmp) }
        let report = ArchiveGuard.inspect(tmp)
        XCTAssertFalse(report.isSafe)
        XCTAssertEqual(report.rejections.first?.kind, .malformed)
    }
}

final class NetworkGateTests: XCTestCase {

    /// **預設拒絕。** 不是「忘了設定就放行」。
    func testDefaultPolicyIsClosed() {
        let saved = NetworkGate.policy
        defer { NetworkGate.policy = saved }
        NetworkGate.policy = { false }
        XCTAssertFalse(NetworkGate.isEnabled)
        let r = NetworkGate.fetchText("https://example.invalid/index.json", purpose: .storeIndex)
        XCTAssertTrue(r.isBlocked, "開關關閉時必須是 blocked,而不是一般的網路錯誤")
    }

    /// 被擋下來與連不上是**兩件事**:前者要顯示一顆開關,後者要顯示重試。
    func testBlockedIsDistinguishableFromFailure() {
        let saved = NetworkGate.policy
        defer { NetworkGate.policy = saved }
        NetworkGate.policy = { true }
        let r = NetworkGate.fetchText("ftp://example.invalid/x", purpose: .storeIndex)
        XCTAssertFalse(r.isBlocked)
        XCTAssertNotNil(r.errorMessage)
    }

    func testResolveURLThreeTierFallback() {
        XCTAssertEqual(NetworkGate.resolveURL(indexURL: "https://a/i.json", baseURL: nil,
                                              file: "https://b/x.zip"), "https://b/x.zip")
        XCTAssertEqual(NetworkGate.resolveURL(indexURL: "https://a/dir/i.json",
                                              baseURL: "https://cdn/p", file: "x.zip"),
                       "https://cdn/p/x.zip")
        XCTAssertEqual(NetworkGate.resolveURL(indexURL: "https://a/dir/i.json", baseURL: nil,
                                              file: "x.zip"), "https://a/dir/x.zip")
    }

    func testHostOfNeverReturnsNil() {
        XCTAssertEqual(NetworkGate.hostOf("https://example.com/a/b?c=d"), "example.com")
        XCTAssertFalse(NetworkGate.hostOf("garbage").isEmpty)
    }

    /// 紀錄裡只有主機名 —— 沒有路徑、沒有查詢字串、沒有任何輸入內容。
    func testLogEntryRoundTripKeepsOnlyHost() {
        let e = NetworkLogEntry(host: "example.com", purpose: .storePackage,
                                label: "萬象", outcome: .ok, bytes: 1234, detail: "")
        let decoded = NetworkLogFile.decode(NetworkLogFile.encode(e))
        XCTAssertEqual(decoded?.host, "example.com")
        XCTAssertEqual(decoded?.bytes, 1234)
        XCTAssertEqual(decoded?.purpose, .storePackage)
        XCTAssertNil(NetworkLogFile.decode("這不是 JSON"))
    }
}
