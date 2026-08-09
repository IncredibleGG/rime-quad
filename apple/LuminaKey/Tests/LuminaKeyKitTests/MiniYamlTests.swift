import XCTest
@testable import LuminaKeyKit

final class MiniYamlTests: XCTestCase {

    private func parse(_ s: String) throws -> YamlNode {
        try MiniYaml.parse(source: "t.yaml", text: s).root
    }

    /// §3.3 的核心：讀取層**不得**做隱式型別解析。
    /// `y` / `n` / `on` / `off` 在 YAML 1.1 是布林、1.2 是字串 ——
    /// 若這裡先變成布林，`keysym: y` 就永遠救不回來了。
    func testScalarsStayStrings() throws {
        let root = try parse("""
        a: y
        b: no
        c: on
        d: 010
        e: 1.0
        """)
        let m = root.mappingValue!
        XCTAssertEqual(m["a"]?.scalarValue, "y")
        XCTAssertEqual(m["b"]?.scalarValue, "no")
        XCTAssertEqual(m["c"]?.scalarValue, "on")
        XCTAssertEqual(m["d"]?.scalarValue, "010")
        XCTAssertEqual(m["e"]?.scalarValue, "1.0")
    }

    func testNullForms() throws {
        let m = try parse("a: null\nb: ~\nc:\n").mappingValue!
        XCTAssertTrue(m["a"]!.isNullScalar)
        XCTAssertTrue(m["b"]!.isNullScalar)
        XCTAssertTrue(m["c"]!.isNullScalar)
    }

    func testQuotesAndEscapes() throws {
        let m = try parse(#"""
        a: "line\nbreak"
        b: 'it''s'
        c: "\u4F60\u597D"
        d: "# not a comment"
        """#).mappingValue!
        XCTAssertEqual(m["a"]?.scalarValue, "line\nbreak")
        XCTAssertEqual(m["b"]?.scalarValue, "it's")
        XCTAssertEqual(m["c"]?.scalarValue, "你好")
        XCTAssertEqual(m["d"]?.scalarValue, "# not a comment")
    }

    func testCommentStripping() throws {
        let m = try parse("""
        # whole line
        a: 1   # trailing
        b: "x#y"
        """).mappingValue!
        XCTAssertEqual(m["a"]?.scalarValue, "1")
        XCTAssertEqual(m["b"]?.scalarValue, "x#y")
    }

    func testFlowCollections() throws {
        let m = try parse("""
        a: [1, "two", null]
        b: { x: 1, y: "z" }
        c: [{ k: v }, { k: w }]
        """).mappingValue!
        XCTAssertEqual(m["a"]?.sequenceValue?.count, 3)
        XCTAssertEqual(m["a"]?.sequenceValue?[1].scalarValue, "two")
        XCTAssertTrue(m["a"]!.sequenceValue![2].isNullScalar)
        XCTAssertEqual(m["b"]?.mappingValue?["y"]?.scalarValue, "z")
        XCTAssertEqual(m["c"]?.sequenceValue?.count, 2)
        XCTAssertEqual(m["c"]?.sequenceValue?[1].mappingValue?["k"]?.scalarValue, "w")
    }

    func testBlockSequenceOfMappings() throws {
        let m = try parse("""
        items:
          - source: schema_name
            tap: "schema:picker"
          - source: variant
        """).mappingValue!
        let items = m["items"]!.sequenceValue!
        XCTAssertEqual(items.count, 2)
        XCTAssertEqual(items[0].mappingValue?["tap"]?.scalarValue, "schema:picker")
        XCTAssertEqual(items[1].mappingValue?["source"]?.scalarValue, "variant")
    }

    /// §3.2：重複 key 是唯一「必須 WARNING 並採最後一次」的禁止構造。
    func testDuplicateKeyWarnsAndLastWins() throws {
        let doc = try MiniYaml.parse(source: "t.yaml", text: "a: 1\na: 2\n")
        XCTAssertEqual(doc.warnings.count, 1)
        XCTAssertEqual(doc.warnings[0].key, "a")
        XCTAssertEqual(doc.root.mappingValue?["a"]?.scalarValue, "2")
    }

    func testTabIndentRejected() {
        XCTAssertThrowsError(try parse("a:\n\tb: 1\n"))
    }

    func testAnchorsRejected() {
        XCTAssertThrowsError(try parse("a: 1\n&anchor\n"))
    }

    func testBomIsSkipped() throws {
        let m = try parse("\u{FEFF}a: 1\n").mappingValue!
        XCTAssertEqual(m["a"]?.scalarValue, "1")
    }

    func testCrlfNormalised() throws {
        let m = try parse("a: 1\r\nb: 2\r\n").mappingValue!
        XCTAssertEqual(m["b"]?.scalarValue, "2")
    }

    func testEmptyFlowMappingIsAValidRoot() throws {
        let root = try parse("{}")
        XCTAssertEqual(root.mappingValue?.count, 0)
    }

    func testLineNumbersAreReported() throws {
        let m = try parse("a: 1\nb: 2\nc: 3\n").mappingValue!
        XCTAssertEqual(m["c"]?.line, 3)
    }
}
