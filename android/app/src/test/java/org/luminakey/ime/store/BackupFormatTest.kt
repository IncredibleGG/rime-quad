package org.luminakey.ime.store

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.luminakey.ime.prefs.AppearanceMode
import org.luminakey.ime.prefs.UserPrefs

/**
 * 格式本身：manifest 的往返、版本相容性規則、偏好的載體。
 *
 * 這一整組**不碰檔案系統也不碰 Android**，因為它們測的是規範
 * （`docs/backup-format.md` §5、§6），不是 IO。
 */
class BackupFormatTest {

    private fun manifest(version: Int = BackupFormat.FORMAT_VERSION) = BackupManifest(
        formatVersion = version,
        createdAt = 1_754_000_000L,
        producer = BackupProducer("android", "0.9.1", 26080810L, 1),
        userDbs = listOf(
            BackupUserDb("luna_pinyin", BackupFormat.ENCODING_LEVELDB_DIR, "dict/luna_pinyin.userdb", true),
            BackupUserDb("bopomofo", BackupFormat.ENCODING_USERDB_TEXT, "dict/bopomofo.userdb.txt", false),
        ),
        schemas = listOf(
            BackupSchemaRef("luna_pinyin_tw", "拼音（臺灣字形）", null, true),
            BackupSchemaRef("double_pinyin", "雙拼", "double-pinyin", false),
        ),
        enabledSchemas = listOf("luna_pinyin_tw", "double_pinyin"),
        files = listOf(
            BackupFile("dict/luna_pinyin.userdb/CURRENT", 16, "ab".repeat(32)),
            BackupFile("config/default.custom.yaml", 240, "cd".repeat(32)),
        ),
        omitted = BackupPlan.DECLARED_OMISSIONS,
    )

    @Test
    fun `manifest 編碼再解碼之後每一個欄位都相同`() {
        val m = manifest()
        val back = ok(BackupManifestJson.decode(BackupManifestJson.encode(m)))
        assertEquals(m, back)
    }

    @Test
    fun `解碼器不接受別的 app 的 json`() {
        val r = BackupManifestJson.decode("""{"kind":"something-else","format_version":1}""")
        assertEquals(BackupProblem.MANIFEST_BROKEN, err(r).problem)
    }

    @Test
    fun `解碼器不會被壞掉的 json 弄崩`() {
        for (bad in listOf("", "{", "not json at all", "[1,2,3]", "{\"kind\":", "null")) {
            val r = BackupManifestJson.decode(bad)
            assertTrue("「$bad」應該是一個 Err 而不是例外", r is BackupManifestJson.Result.Err)
        }
    }

    /* ─────────────── 相容性規則（docs/backup-format.md §6）─────────────── */

    @Test
    fun `比本版新的備份要說清楚是要升級 App`() {
        val r = BackupManifestJson.decode(
            BackupManifestJson.encode(manifest(BackupFormat.FORMAT_VERSION + 1))
        )
        val issue = err(r)
        assertEquals(BackupProblem.TOO_NEW, issue.problem)
        // 訊息要帶得出「它是幾版、我們到幾版」，否則回報者說不清楚。
        assertEquals(
            listOf((BackupFormat.FORMAT_VERSION + 1).toString(), BackupFormat.FORMAT_VERSION.toString()),
            issue.args,
        )
    }

    @Test
    fun `比支援下限舊的備份要被明確拒絕而不是盡力而為`() {
        val r = BackupManifestJson.decode(
            BackupManifestJson.encode(manifest(BackupFormat.MIN_READABLE_VERSION - 1))
        )
        assertEquals(BackupProblem.TOO_OLD, err(r).problem)
    }

    @Test
    fun `版本判定就是那三種，沒有第四種`() {
        assertEquals(BackupFormat.Verdict.OK, BackupFormat.verdictFor(BackupFormat.FORMAT_VERSION))
        assertEquals(BackupFormat.Verdict.TOO_NEW, BackupFormat.verdictFor(BackupFormat.FORMAT_VERSION + 1))
        assertEquals(BackupFormat.Verdict.TOO_OLD, BackupFormat.verdictFor(BackupFormat.MIN_READABLE_VERSION - 1))
    }

    /**
     * ⚠ 版本判定必須**先於**欄位檢查。
     *
     * 一份未來版本的 manifest 對現在的解析器來說「缺欄位」是必然的。
     * 若先報「格式壞掉」，使用者會去找一個不存在的壞檔案，
     * 而他真正該做的事是升級 App。
     */
    @Test
    fun `未來版本就算欄位長得完全不一樣，也要報成版本太新`() {
        val future = """
            {
              "kind": "${BackupFormat.KIND}",
              "format_version": ${BackupFormat.FORMAT_VERSION + 3},
              "everything_else_is_different": true
            }
        """.trimIndent()
        assertEquals(BackupProblem.TOO_NEW, err(BackupManifestJson.decode(future)).problem)
    }

    @Test
    fun `未知的欄位一律忽略，不當成錯誤`() {
        val text = BackupManifestJson.encode(manifest())
            .replaceFirst("\"created_at\"", "\"a_field_from_the_future\": [1,2,3],\n  \"created_at\"")
        val back = ok(BackupManifestJson.decode(text))
        assertEquals(manifest().files, back.files)
    }

    @Test
    fun `flushed 缺席時當成 false —— 沒有證據就不是有`() {
        val text = """
            {"kind":"${BackupFormat.KIND}","format_version":1,
             "user_db":[{"name":"x","root":"dict/x.userdb"}],
             "files":[{"path":"dict/x.userdb/CURRENT","size":1,"sha256":"00"}]}
        """.trimIndent()
        val m = ok(BackupManifestJson.decode(text))
        assertFalse(m.userDbs.single().flushed)
        assertTrue(m.hasUnflushedUserDb)
        // encoding 缺席時退回 leveldb-dir（v1 唯一存在的載體）。
        assertEquals(BackupFormat.ENCODING_LEVELDB_DIR, m.userDbs.single().encoding)
    }

    /* ─────────────── 讀不動的載體 ─────────────── */

    /**
     * ⚠ 規範 §3.1 有兩種詞典載體，而 Android 只讀得動 `leveldb-dir`。
     *
     * 若匯入只是「把 dict/ 底下的目錄搬過去」，一份 `rime-userdb-text` 的備份
     * 會**完全正常地匯入成功**，只是使用者的詞庫一本都沒回來 —— 沒有任何
     * 錯誤訊息。這條測試守的就是「讀不動要說出是哪一本」。
     */
    @Test
    fun `讀不動的載體要被指名，不可以安靜地少一本`() {
        val m = manifest()
        val unreadable = m.unreadableUserDbs(BackupFormat.READABLE_ENCODINGS)
        assertEquals(listOf("bopomofo"), unreadable.map { it.name })

        // 兩種都支援時就沒有讀不動的 —— 等 rs_sync_user_data() 進 ABI 之後
        // READABLE_ENCODINGS 多加一個字串，這一行就是那天的驗收。
        assertTrue(
            m.unreadableUserDbs(
                setOf(BackupFormat.ENCODING_LEVELDB_DIR, BackupFormat.ENCODING_USERDB_TEXT)
            ).isEmpty()
        )
    }

    @Test
    fun `未知的載體也算讀不動`() {
        val m = manifest().copy(
            userDbs = listOf(BackupUserDb("x", "some-future-format", "dict/x", true))
        )
        assertEquals(listOf("x"), m.unreadableUserDbs(BackupFormat.READABLE_ENCODINGS).map { it.name })
    }

    /* ─────────────── 路徑白名單 ─────────────── */

    @Test
    fun `只有五個已知前綴底下的路徑會被接受`() {
        assertTrue(BackupFormat.isAllowedEntry("dict/x.userdb/CURRENT"))
        assertTrue(BackupFormat.isAllowedEntry("schema/luna_pinyin.dict.yaml"))
        assertTrue(BackupFormat.isAllowedEntry("config/default.custom.yaml"))
        assertTrue(BackupFormat.isAllowedEntry("settings/prefs.json"))
        assertTrue(BackupFormat.isAllowedEntry(BackupFormat.LAYOUT_ENTRY))
        assertTrue(BackupFormat.isAllowedEntry(BackupFormat.LEGACY_LAYOUT_ENTRY))

        // 清單檔在容器根目錄,不在任何一個前綴底下 —— 新舊兩個名字都一樣。
        assertFalse(BackupFormat.isAllowedEntry(BackupFormat.MANIFEST_NAME))
        assertFalse(BackupFormat.isAllowedEntry(BackupFormat.LEGACY_MANIFEST_NAME))
        assertFalse(BackupFormat.isAllowedEntry("net/connections.tsv"))
        assertFalse(BackupFormat.isAllowedEntry("dict/"))      // 只有前綴，沒有檔名
        assertFalse(BackupFormat.isAllowedEntry(""))
        assertFalse(BackupFormat.isAllowedEntry("../escape"))
    }

    /* ─────────────── 偏好 ─────────────── */

    @Test
    fun `偏好往返之後語義相同，包含「未設定」`() {
        val prefs = UserPrefs(
            keyboardHeightScale = 1.15f,
            candidateCount = 7,
            soundEnabled = true,
            themeId = "solarized",
            appearanceMode = AppearanceMode.DARK,
            layoutPins = "luna_pinyin=cn-t9-pinyin",
        )
        val back = UserPrefs.fromMap(BackupPrefsJson.decode(BackupPrefsJson.encode(prefs.toMap())))
        assertEquals(prefs, back)
        // 沒設定過的仍然是 null，不可以被補成預設值（見 UserPrefs 檔頭）。
        assertNull(back.hapticStrength)
        assertNull(back.simplification)
    }

    /**
     * ⚠ 連網開關**不可以**跟著備份跑。
     *
     * 它是安全預設而不是喜好：一個從檔案還原的動作，不該把一台新手機
     * 的連網能力打開。這條測試同時守寫入端與讀取端。
     */
    @Test
    fun `連網開關與引導狀態不會跟著備份走`() {
        val prefs = UserPrefs(
            networkEnabled = true,
            offlineNoticeSeen = true,
            onboardingDone = true,
            candidateCount = 5,
        )
        val text = BackupPrefsJson.encode(prefs.toMap())
        assertFalse("連網開關不可以出現在備份裡：$text", text.contains(UserPrefs.K_NETWORK_ENABLED))
        assertFalse(text.contains(UserPrefs.K_OFFLINE_NOTICE_SEEN))
        assertFalse(text.contains(UserPrefs.K_ONBOARDING_DONE))

        // 讀取端再擋一次：手改過的備份也不行。
        val handMade = """
            {"format_version":1,"values":{"${UserPrefs.K_NETWORK_ENABLED}":true,
             "${UserPrefs.K_ONBOARDING_DONE}":true,"${UserPrefs.K_CANDIDATE_COUNT}":9}}
        """.trimIndent()
        val decoded = BackupPrefsJson.decode(handMade)
        assertFalse(decoded.containsKey(UserPrefs.K_NETWORK_ENABLED))
        assertFalse(decoded.containsKey(UserPrefs.K_ONBOARDING_DONE))
        assertEquals(9, (decoded[UserPrefs.K_CANDIDATE_COUNT] as Number).toInt())
    }

    @Test
    fun `壞掉的偏好不會讓整個匯入垮掉`() {
        assertEquals(emptyMap<String, Any?>(), BackupPrefsJson.decode("not json"))
        assertEquals(emptyMap<String, Any?>(), BackupPrefsJson.decode("{}"))
    }

    @Test
    fun `小數不會被磨成整數`() {
        val m = BackupPrefsJson.decode(
            BackupPrefsJson.encode(mapOf(UserPrefs.K_HEIGHT_SCALE to 1.15f))
        )
        assertEquals(1.15f, (m[UserPrefs.K_HEIGHT_SCALE] as Number).toFloat(), 0.0001f)
    }

    /* ─────────────── 工具 ─────────────── */

    private fun ok(r: BackupManifestJson.Result<BackupManifest>): BackupManifest = when (r) {
        is BackupManifestJson.Result.Ok -> r.value
        is BackupManifestJson.Result.Err -> throw AssertionError("預期成功，得到 ${r.issue}")
    }

    private fun err(r: BackupManifestJson.Result<BackupManifest>): BackupIssue = when (r) {
        is BackupManifestJson.Result.Err -> r.issue
        is BackupManifestJson.Result.Ok -> throw AssertionError("預期失敗，卻成功了")
    }
}
