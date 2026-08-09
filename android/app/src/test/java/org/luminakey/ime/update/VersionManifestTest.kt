package org.luminakey.ime.update

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * version.json 的解析、版本比較與 sha256 比對。
 *
 * 這三件事決定「要不要往使用者的機器上裝一個 APK」，所以全部做成純函式，
 * 用 JVM 單元測試直接打 —— 不需要 Android 執行環境，也就不需要 Robolectric。
 */
class VersionManifestTest {

    private val url = "https://example.invalid/rime/version.json"
    private val sha = "7856b19a5f10a6acb8c7cb485f55b2a9343348e01f620c73dd52af826ff7412a"

    private fun json(
        versionCode: String? = "26080721",
        versionName: String? = "\"0.1.0-dev+26080721.3c8a8d9\"",
        size: String? = "27965997",
        sha256: String? = "\"$sha\"",
        url: String? = "\"https://example.invalid/rime/rime.apk\"",
        extra: String = "",
    ): String = buildString {
        append("{\n")
        versionCode?.let { append("  \"version_code\": $it,\n") }
        versionName?.let { append("  \"version_name\": $it,\n") }
        size?.let { append("  \"size\": $it,\n") }
        sha256?.let { append("  \"sha256\": $it,\n") }
        url?.let { append("  \"url\": $it,\n") }
        append(extra)
        append("  \"commit\": \"3c8a8d9\"\n}")
    }

    private fun ok(text: String): VersionManifest {
        val r = VersionManifestParser.parse(text, url)
        assertTrue("預期解析成功，實際：$r", r is ManifestParseResult.Ok)
        return (r as ManifestParseResult.Ok).manifest
    }

    private fun err(text: String): String {
        val r = VersionManifestParser.parse(text, url)
        assertTrue("預期解析失敗，實際：$r", r is ManifestParseResult.Err)
        return (r as ManifestParseResult.Err).message
    }

    /* ───────────────────────── 解析 ───────────────────────── */

    @Test
    fun parsesAWellFormedManifest() {
        val m = ok(json())
        assertEquals(26080721L, m.versionCode)
        assertEquals("0.1.0-dev+26080721.3c8a8d9", m.versionName)
        assertEquals(27965997L, m.size)
        assertEquals(sha, m.sha256)
        assertEquals("https://example.invalid/rime/rime.apk", m.url)
        assertEquals("3c8a8d9", m.commit)
        assertEquals("", m.notes)
    }

    @Test
    fun notesAreOptionalAndTrimmed() {
        val m = ok(json(extra = "  \"notes\": \"  修好了九宮格  \",\n"))
        assertEquals("修好了九宮格", m.notes)
    }

    @Test
    fun rejectsAManifestWithoutVersionCode() {
        // 舊格式的 version.json（發布腳本加上 version_code 之前產生的）沒有這個
        // 欄位。此時**沒有**任何可靠的方式判斷新舊，只能拒收；絕不可以退回去
        // 比 version_name 字串，那遲早比出 "0.10.0" < "0.9.0"。
        val msg = err(json(versionCode = null))
        assertTrue(msg, msg.contains("version_code"))
    }

    @Test
    fun rejectsNonPositiveVersionCode() {
        assertTrue(err(json(versionCode = "0")).contains("正整數"))
        assertTrue(err(json(versionCode = "-3")).contains("正整數"))
    }

    @Test
    fun rejectsAMalformedSha256() {
        assertTrue(err(json(sha256 = "\"deadbeef\"")).contains("sha256"))
        assertTrue(err(json(sha256 = "\"${sha.dropLast(1)}Z\"")).contains("sha256"))
        assertTrue(err(json(sha256 = null)).contains("sha256"))
    }

    @Test
    fun normalisesUppercaseSha256() {
        // 伺服器側換個工具就可能吐大寫。那不該變成「摘要不符」。
        val m = ok(json(sha256 = "\"${sha.uppercase()}\""))
        assertEquals(sha, m.sha256)
    }

    @Test
    fun rejectsAbsurdOrMissingSize() {
        assertTrue(err(json(size = null)).contains("size"))
        assertTrue(err(json(size = "0")).contains("size"))
        // 宣告的 size 同時是下載的硬上限，所以一個荒謬的值必須當場擋下，
        // 而不是等到把使用者的儲存空間塞爆。
        assertTrue(err(json(size = "${VersionManifestParser.MAX_APK_BYTES + 1}")).contains("size"))
    }

    @Test
    fun rejectsNonHttpDownloadUrls() {
        val msg = err(json(url = "\"file:///data/local/tmp/evil.apk\""))
        assertTrue(msg, msg.contains("http"))
    }

    @Test
    fun resolvesRelativeUrlsAgainstTheManifestLocation() {
        // 整個目錄搬到別的主機時不必改 version.json 的內容。
        val m = ok(json(url = "\"rime-latest.apk\""))
        assertEquals("https://example.invalid/rime/rime-latest.apk", m.url)
    }

    @Test
    fun fallsBackToTheFileFieldWhenUrlIsAbsent() {
        val m = ok(json(url = null, extra = "  \"file\": \"rime-1.apk\",\n"))
        assertEquals("https://example.invalid/rime/rime-1.apk", m.url)
        assertEquals("rime-1.apk", m.fileName)
    }

    @Test
    fun rejectsGarbageInsteadOfGuessing() {
        assertTrue(err("這不是 JSON").isNotEmpty())
        assertTrue(err("[1, 2, 3]").contains("物件"))
    }

    @Test
    fun acceptsCommentsLikeTheRestOfTheProject() {
        // MiniJson 刻意接受註解（docs/schema-store.md 的範例本身就是 jsonc）。
        val m = ok("// 發布於 2026-08-07\n" + json())
        assertEquals(26080721L, m.versionCode)
    }

    /* ───────────────────────── 版本比較 ───────────────────────── */

    @Test
    fun aStrictlyHigherVersionCodeIsAnUpdate() {
        val m = ok(json(versionCode = "26080721"))
        assertEquals(UpdateVerdict.UPDATE_AVAILABLE, UpdateCheck.verdict(26080720L, m))
        assertEquals(UpdateVerdict.UPDATE_AVAILABLE, UpdateCheck.verdict(1L, m))
    }

    @Test
    fun anEqualVersionCodeIsNotAnUpdate() {
        val m = ok(json(versionCode = "26080721"))
        assertEquals(UpdateVerdict.UP_TO_DATE, UpdateCheck.verdict(26080721L, m))
    }

    @Test
    fun aLowerRemoteVersionIsReportedAsADowngradeNotAsUpToDate() {
        // 併進 UP_TO_DATE 會掩蓋「發布端把 version.json 指到舊檔」這種事故，
        // 而 Android 本來就不允許降級安裝 —— 給使用者一顆按下去必定失敗的
        // 「更新」按鈕比不給更糟。
        val m = ok(json(versionCode = "26080700"))
        assertEquals(UpdateVerdict.DOWNGRADE, UpdateCheck.verdict(26080721L, m))
    }

    /* ───────────────────────── sha256 比對 ───────────────────────── */

    @Test
    fun sha256ComparisonIgnoresCaseAndSurroundingWhitespace() {
        assertTrue(UpdateCheck.sha256Matches(sha, sha))
        assertTrue(UpdateCheck.sha256Matches(sha.uppercase(), sha))
        assertTrue(UpdateCheck.sha256Matches("  $sha\n", sha))
    }

    @Test
    fun sha256ComparisonRejectsAnyContentDifference() {
        // 一個位元組的差別就必須不符 —— 這條線的整個意義就在這裡。
        val flipped = sha.dropLast(1) + if (sha.last() == 'a') 'b' else 'a'
        assertFalse(UpdateCheck.sha256Matches(sha, flipped))
        assertFalse(UpdateCheck.sha256Matches(sha, ""))
        assertFalse(UpdateCheck.sha256Matches("", sha))
        // 截斷的摘要不算「前綴相符」。
        assertFalse(UpdateCheck.sha256Matches(sha, sha.dropLast(1)))
    }

    /* ─────────────── 真實檔案（回歸用）─────────────── */

    @Test
    fun parsesTheShapePublishApkShProduces() {
        val real = """
            {
              "version_name": "20260807-2105-3c8a8d9",
              "version_code": 26080721,
              "commit": "3c8a8d9",
              "file": "rime-android-debug-20260807-2105-3c8a8d9.apk",
              "size": 27965997,
              "sha256": "$sha",
              "url": "https://pub-x.r2.dev/rime/rime-android-debug-20260807-2105-3c8a8d9.apk",
              "latest_url": "https://pub-x.r2.dev/rime/rime-latest.apk",
              "notes": ""
            }
        """.trimIndent()
        val m = ok(real)
        assertEquals(26080721L, m.versionCode)
        assertEquals("rime-android-debug-20260807-2105-3c8a8d9.apk", m.fileName)
        assertEquals("", m.notes)
        assertEquals(UpdateVerdict.UPDATE_AVAILABLE, UpdateCheck.verdict(1L, m))
    }
}
