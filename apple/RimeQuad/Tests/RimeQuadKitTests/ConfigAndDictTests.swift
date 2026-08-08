//
//  ConfigAndDictTests.swift — default.custom.yaml 的外科手術、詞庫、方案掃描、IPC
//

import XCTest
@testable import RimeQuadKit

final class RimeConfigPatchTests: XCTestCase {

    func testReadsBothItemForms() {
        let text = """
        patch:
          schema_list:
            - schema: luna_pinyin_tw
            - bopomofo_tw
        """
        XCTAssertEqual(RimeConfigPatch.readSchemaList(text: text),
                       ["luna_pinyin_tw", "bopomofo_tw"])
    }

    func testMissingOrBrokenFileIsEmptyNotAnError() {
        XCTAssertEqual(RimeConfigPatch.readSchemaList(text: ""), [])
        XCTAssertEqual(RimeConfigPatch.readSchemaList(text: "這不是 YAML: [[["), [])
        XCTAssertEqual(RimeConfigPatch.readSchemaList(text: "patch:\n  other: 1"), [])
    }

    /// ⚠ **使用者寫的東西一個字都不能掉。** 只有 schema_list 那幾行會被改寫。
    func testSurgeryKeepsEverythingElse() {
        let original = """
        # 我自己的設定,不要動它
        patch:
          "key_binder/bindings/+":
            - { when: composing, accept: Tab, send: Escape }
          schema_list:
            - schema: luna_pinyin_tw    # 拼音(臺灣字形)
          "menu/page_size": 7
        """
        let out = RimeConfigPatch.writeSchemaList(into: original,
                                                  ids: ["luna_pinyin_tw", "bopomofo_tw"])
        XCTAssertTrue(out.contains("# 我自己的設定,不要動它"))
        XCTAssertTrue(out.contains("key_binder/bindings/+"))
        XCTAssertTrue(out.contains("menu/page_size: 7"))
        // 行尾註解跟著那個 id 活下來。
        XCTAssertTrue(out.contains("- schema: luna_pinyin_tw    # 拼音(臺灣字形)"))
        XCTAssertEqual(RimeConfigPatch.readSchemaList(text: out),
                       ["luna_pinyin_tw", "bopomofo_tw"])
    }

    func testOrderIsPreservedExactly() {
        let text = "patch:\n  schema_list:\n    - schema: a\n    - schema: b\n    - schema: c\n"
        let out = RimeConfigPatch.writeSchemaList(into: text, ids: ["c", "a", "b"])
        XCTAssertEqual(RimeConfigPatch.readSchemaList(text: out), ["c", "a", "b"])
    }

    func testCreatesBlockWhenAbsent() {
        XCTAssertEqual(RimeConfigPatch.readSchemaList(
            text: RimeConfigPatch.writeSchemaList(into: "", ids: ["x"])), ["x"])
        let withPatch = RimeConfigPatch.writeSchemaList(into: "patch:\n  other: 1\n", ids: ["y"])
        XCTAssertEqual(RimeConfigPatch.readSchemaList(text: withPatch), ["y"])
        XCTAssertTrue(withPatch.contains("other: 1"))
        let noPatch = RimeConfigPatch.writeSchemaList(into: "# 只有一行註解\n", ids: ["z"])
        XCTAssertEqual(RimeConfigPatch.readSchemaList(text: noPatch), ["z"])
        XCTAssertTrue(noPatch.contains("# 只有一行註解"))
    }

    func testPageSizeRoundTrip() {
        var text = RimeConfigPatch.writePageSize(into: "", size: 9)
        XCTAssertEqual(RimeConfigPatch.readPageSize(text: text), 9)
        text = RimeConfigPatch.writePageSize(into: text, size: 5)
        XCTAssertEqual(RimeConfigPatch.readPageSize(text: text), 5)
        // nil = 移除,回到方案自己的預設。留一個空的鍵不是同一件事。
        text = RimeConfigPatch.writePageSize(into: text, size: nil)
        XCTAssertNil(RimeConfigPatch.readPageSize(text: text))
    }

    func testPageSizeAndSchemaListCoexist() {
        var text = RimeConfigPatch.writeSchemaList(into: "", ids: ["a", "b"])
        text = RimeConfigPatch.writePageSize(into: text, size: 7)
        XCTAssertEqual(RimeConfigPatch.readSchemaList(text: text), ["a", "b"])
        XCTAssertEqual(RimeConfigPatch.readPageSize(text: text), 7)
    }

    func testNestedMenuFormIsRead() {
        XCTAssertEqual(RimeConfigPatch.readPageSize(
            text: "patch:\n  menu:\n    page_size: 6\n"), 6)
    }
}

final class UserPhrasesTests: XCTestCase {

    func testParseTsv() {
        let r = UserPhrases.parse("""
        # 註解
        你好\tni hao\t3
        黃小明\thuang xiao ming
        """)
        XCTAssertEqual(r.phrases.count, 2)
        XCTAssertEqual(r.phrases[0], UserPhrase(text: "你好", code: "ni hao", weight: 3))
        XCTAssertEqual(r.phrases[1].weight, 1, "沒有第三欄時權重是 1")
        XCTAssertTrue(r.problems.isEmpty)
    }

    /// ⚠ CRLF:Windows 端匯出的檔案帶 \r,留著會讓權重欄變成「1\r」而解析失敗,
    /// 錯誤訊息看起來卻像使用者打錯字。
    func testCrlfIsHandled() {
        let r = UserPhrases.parse("你好\tni hao\t3\r\n世界\tshi jie\t2\r\n")
        XCTAssertEqual(r.phrases.count, 2)
        XCTAssertEqual(r.phrases[0].weight, 3)
    }

    /// 一行寫壞不該讓整本詞庫消失。
    func testBadLinesAreReportedNotFatal() {
        let r = UserPhrases.parse("好的\tokay\n沒有tab\n\t只有編碼\n空詞\t\n對的\tdui de\n")
        XCTAssertEqual(r.phrases.map(\.text), ["好的", "對的"])
        XCTAssertEqual(r.problems.count, 3)
    }

    func testCodeIsNormalised() {
        let r = UserPhrases.parse("你好\t NI  Hao \t1\n")
        XCTAssertEqual(r.phrases.first?.code, "ni hao",
                       "大小寫與多重空白要收斂,否則同一個詞會變成兩筆")
    }

    func testRoundTrip() {
        let list = [UserPhrase(text: "你好", code: "ni hao", weight: 3),
                    UserPhrase(text: "안녕", code: "annyeong", weight: 1)]
        XCTAssertEqual(UserPhrases.parse(UserPhrases.serialise(list)).phrases, list)
    }

    func testAddingIsIdempotentOnIdentity() {
        var list: [UserPhrase] = []
        var isNew: Bool
        (list, isNew) = UserPhrases.adding(UserPhrase(text: "甲", code: "jia"), to: list)
        XCTAssertTrue(isNew)
        (list, isNew) = UserPhrases.adding(UserPhrase(text: "甲", code: "jia", weight: 9),
                                           to: list)
        XCTAssertFalse(isNew)
        XCTAssertEqual(list.count, 1)
        XCTAssertEqual(list[0].weight, 9)
    }

    func testNewestFirst() {
        var list = [UserPhrase(text: "舊", code: "jiu")]
        (list, _) = UserPhrases.adding(UserPhrase(text: "新", code: "xin"), to: list)
        XCTAssertEqual(list.first?.text, "新", "剛加的詞應該一眼看得到")
    }

    /// 匯入是**合併**,不是取代 —— 使用者按的是「匯入」。
    func testMergeKeepsExistingAndCountsCorrectly() {
        let mine = [UserPhrase(text: "甲", code: "jia", weight: 5),
                    UserPhrase(text: "乙", code: "yi", weight: 1)]
        let theirs = [UserPhrase(text: "甲", code: "jia", weight: 2),
                      UserPhrase(text: "丙", code: "bing", weight: 1)]
        let r = UserPhrases.merging(theirs, into: mine)
        XCTAssertEqual(r.added, 1)
        XCTAssertEqual(r.skipped, 1)
        XCTAssertEqual(r.updated, 0)
        XCTAssertEqual(r.list.count, 3)
        XCTAssertEqual(r.list.first(where: { $0.text == "甲" })?.weight, 5,
                       "衝突時取權重大的那一筆")
    }

    func testRemoving() {
        let list = [UserPhrase(text: "甲", code: "jia"), UserPhrase(text: "乙", code: "yi")]
        XCTAssertEqual(UserPhrases.removing(identity: list[0].identity, from: list).count, 1)
    }

    /// ⚠ 使用者自己寫的 `<schema>.custom.yaml` 不可以被蓋掉 ——
    /// 那裡面常有他調了很久的按鍵綁定,而覆蓋是不可逆的。
    func testMountRefusesToOverwriteUserOwnedFile() {
        let dir = FileManager.default.temporaryDirectory
            .appendingPathComponent("rimequad-mount-\(UUID().uuidString)")
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: dir) }

        XCTAssertEqual(UserPhrases.mount(schemaId: "luna_pinyin", userDir: dir), .written)
        XCTAssertEqual(UserPhrases.mount(schemaId: "luna_pinyin", userDir: dir), .alreadyOurs)

        let mine = dir.appendingPathComponent("bopomofo.custom.yaml")
        try? "patch:\n  my: setting\n".write(to: mine, atomically: true, encoding: .utf8)
        XCTAssertEqual(UserPhrases.mount(schemaId: "bopomofo", userDir: dir),
                       .skippedUserOwned("bopomofo.custom.yaml"))
        XCTAssertEqual(try? String(contentsOf: mine, encoding: .utf8), "patch:\n  my: setting\n")
    }

    func testPatchTextMentionsTheRightFile() {
        // user_dict 必須與 fileName 對得起來,否則掛了等於沒掛。
        XCTAssertTrue(UserPhrases.schemaPatchText().contains("user_dict: custom_phrase"))
        XCTAssertEqual(UserPhrases.fileName, "custom_phrase.txt")
        XCTAssertTrue(UserPhrases.isOurs(UserPhrases.schemaPatchText()))
        XCTAssertFalse(UserPhrases.isOurs("patch:\n  x: 1\n"))
    }
}

final class SchemaCatalogTests: XCTestCase {

    func testReadsHeader() {
        let h = SchemaCatalog.readHeader("""
        # comment
        schema:
          schema_id: luna_pinyin_tw
          name: "朙月拼音·臺灣正體"
          version: "0.35"
          author:
            - 佛振
        switches:
          - name: ascii_mode
        """)
        XCTAssertEqual(h.schemaId, "luna_pinyin_tw")
        XCTAssertEqual(h.name, "朙月拼音·臺灣正體")
        XCTAssertEqual(h.version, "0.35")
    }

    /// ⚠ 不用 RTS 解析器。第三方方案用的是完整 YAML(錨點、標籤、__include),
    /// 拿 RTS 去解析會在合法的方案上報錯,然後那個方案就從清單裡消失了。
    func testExoticYamlDoesNotBreakIt() {
        let h = SchemaCatalog.readHeader("""
        __include: pinyin.yaml:/patch
        schema:
          schema_id: weird
          name: &n 奇怪的方案
        engine:
          filters: [*n]
        """)
        XCTAssertEqual(h.schemaId, "weird")
        XCTAssertEqual(h.name, "奇怪的方案")
    }

    func testRowsPutEnabledFirstInUserOrder() {
        let installed = [
            InstalledSchema(id: "a", name: "A", url: URL(fileURLWithPath: "/a"), isBuiltin: true),
            InstalledSchema(id: "b", name: "B", url: URL(fileURLWithPath: "/b"), isBuiltin: false),
        ]
        let rows = SchemaCatalog.rows(installed: installed, enabled: ["b", "ghost"])
        XCTAssertEqual(rows.map(\.id), ["b", "ghost", "a"])
        XCTAssertEqual(rows[0].enabled, true)
        // 已啟用但檔案不在的 id **要留著**:偷偷拿掉會讓使用者的清單
        // 在他不知情的時候變短,而那通常代表安裝或同步出了問題。
        XCTAssertEqual(rows[1].installed, false)
        XCTAssertEqual(rows[2].enabled, false)
    }
}

final class SchemaPreflightTests: XCTestCase {

    private func makeDir() -> URL {
        let d = FileManager.default.temporaryDirectory
            .appendingPathComponent("rimequad-pf-\(UUID().uuidString)")
        try? FileManager.default.createDirectory(at: d, withIntermediateDirectories: true)
        return d
    }

    func testMissingDictionaryIsNamed() {
        let dir = makeDir()
        defer { try? FileManager.default.removeItem(at: dir) }
        let schema = dir.appendingPathComponent("x.schema.yaml")
        try? """
        schema:
          schema_id: x
        translator:
          dictionary: nowhere
        """.write(to: schema, atomically: true, encoding: .utf8)

        let report = SchemaPreflight.check(schemaFile: schema, searchDirs: [dir])
        XCTAssertFalse(report.ok)
        XCTAssertEqual(report.missing.first?.fileName, "nowhere.dict.yaml")
        XCTAssertEqual(report.missing.first?.kind, .dictionary)
        // 訊息要講得出是哪一本詞庫 —— rs_last_error() 在部署失敗時是空字串。
        XCTAssertTrue(report.missing.first!.message.hant.contains("nowhere.dict.yaml"))
    }

    func testPresentFilesPass() {
        let dir = makeDir()
        defer { try? FileManager.default.removeItem(at: dir) }
        try? "".write(to: dir.appendingPathComponent("here.dict.yaml"),
                      atomically: true, encoding: .utf8)
        let schema = dir.appendingPathComponent("y.schema.yaml")
        try? "schema:\n  schema_id: y\ntranslator:\n  dictionary: here\n"
            .write(to: schema, atomically: true, encoding: .utf8)
        XCTAssertTrue(SchemaPreflight.check(schemaFile: schema, searchDirs: [dir]).ok)
    }

    func testDependenciesSequenceIsResolved() {
        var refs: [(String, SchemaPreflight.Kind)] = []
        SchemaPreflight.collect(text: "dependencies:\n  - stroke\n  - terra_pinyin\n", into: &refs)
        XCTAssertEqual(refs.map(\.0), ["stroke.schema.yaml", "terra_pinyin.schema.yaml"])
    }

    func testIncludeTargets() {
        XCTAssertEqual(SchemaPreflight.includeTarget("pinyin:/patch"), "pinyin.yaml")
        XCTAssertEqual(SchemaPreflight.includeTarget("symbols.yaml:/x"), "symbols.yaml")
        XCTAssertNil(SchemaPreflight.includeTarget("/local/node"))
    }

    /// 誤判會擋下完全正常的套件,比漏判更糟。不含連字號的 `language:`
    /// 多半不是語言模型檔名。
    func testLanguageWithoutHyphenIsNotAGrammarFile() {
        var refs: [(String, SchemaPreflight.Kind)] = []
        SchemaPreflight.collect(text: "grammar:\n  language: chinese\n", into: &refs)
        XCTAssertTrue(refs.isEmpty)
        SchemaPreflight.collect(text: "grammar:\n  language: zh-hans-t-essay-bgw\n", into: &refs)
        XCTAssertEqual(refs.first?.0, "zh-hans-t-essay-bgw.gram")
    }
}

final class IPCTests: XCTestCase {

    /// userInfo **只能放 [String: String]**:DistributedNotificationCenter 會把它
    /// 序列化成 property list,自訂型別在對面變成 nil,而且不會有錯誤 ——
    /// 只是訊息永遠不到。
    func testRequestRoundTrip() {
        let r = IPCRequest(verb: .selectSchema, arg: "luna_pinyin_tw")
        XCTAssertEqual(IPC.decodeRequest(IPC.encode(r)), r)
        XCTAssertNil(IPC.decodeRequest(nil))
        XCTAssertNil(IPC.decodeRequest(["id": "x"]))
        XCTAssertNil(IPC.decodeRequest(["id": "", "verb": "deploy"]))
    }

    func testReplyRoundTripWithMultilineDetails() {
        let r = IPCReply(id: "abc", kind: .fail, elapsedMs: 1234, text: "失敗",
                         details: ["第一行", "第二行"])
        XCTAssertEqual(IPC.decodeReply(IPC.encode(r)), r)
        XCTAssertEqual(IPC.decodeReply(IPC.encode(IPCReply(id: "a", kind: .ok)))?.details, [])
    }

    private final class Clock {
        var t: TimeInterval = 0
        func now() -> TimeInterval { t }
    }

    /// 沒接上與接上了但卡住是**兩件事**,使用者的下一步完全不同。
    func testNoResponderVersusStalled() {
        let clock = Clock()
        let w = IPCWaiter(requestId: "r", now: clock.now)
        clock.t = IPC.handshakeTimeout + 1
        XCTAssertEqual(w.tick(), .noResponder)

        let clock2 = Clock()
        let w2 = IPCWaiter(requestId: "r", now: clock2.now)
        w2.accept(IPCReply(id: "r", kind: .progress, elapsedMs: 100))
        clock2.t = IPC.stallTimeout + 1
        XCTAssertEqual(w2.tick(), .stalled)
    }

    func testProgressKeepsItAlive() {
        let clock = Clock()
        let w = IPCWaiter(requestId: "r", now: clock.now)
        for i in 1...10 {
            clock.t += IPC.handshakeTimeout - 0.5
            w.accept(IPCReply(id: "r", kind: .progress, elapsedMs: i * 1000))
            XCTAssertFalse(w.isSettled, "還在流動就不算逾時")
        }
    }

    func testOtherRequestsAreIgnored() {
        let clock = Clock()
        let w = IPCWaiter(requestId: "mine", now: clock.now)
        XCTAssertFalse(w.accept(IPCReply(id: "someone-else", kind: .ok)))
        XCTAssertEqual(w.state, .waitingForFirstReply)
    }

    /// 遲到的進度不可以把已經到手的結果蓋掉。
    func testLateProgressDoesNotOverwriteResult() {
        let clock = Clock()
        let w = IPCWaiter(requestId: "r", now: clock.now)
        w.accept(IPCReply(id: "r", kind: .ok, elapsedMs: 7200))
        w.accept(IPCReply(id: "r", kind: .progress, elapsedMs: 100))
        guard case .finished(let reply) = w.state else { return XCTFail("應該已經結束") }
        XCTAssertEqual(reply.kind, .ok)
        XCTAssertEqual(reply.elapsedMs, 7200)
    }
}
