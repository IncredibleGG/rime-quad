package org.rimequad.ime.keyboard

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import org.rimequad.ime.theme.KeyboardLayout
import org.rimequad.ime.theme.LayoutKey
import org.rimequad.ime.theme.LoadResult
import org.rimequad.ime.theme.RepoFixtures
import org.rimequad.ime.theme.SendSpec
import java.io.File

/**
 * 使用者自訂鍵位的引擎層。
 *
 * 夾具直接讀 repo 的 `core/layouts`（見 [RepoFixtures]），所以這裡守的是
 * **真的會裝進 APK 的那幾份 yaml**，不是測試專用的複製品 —— 上游哪天把
 * qwerty 的 `a` 鍵改名或改寬度，這批測試會先紅，而不是等使用者踩到。
 */
class KeyRemapTest {

    @get:Rule
    val tmp = TemporaryFolder()

    private val repo = FixtureRepo()

    private fun load(id: String): KeyboardLayout = repo.loadLayout(id).value!!

    /* ══════════════════ 1. 交換位置：送出內容不變 ══════════════════ */

    /**
     * 使用者舉的第一個例子：26 鍵的 a 與 s 對調。
     *
     * 斷言的重點**不是**「兩顆鍵換了位置」，而是「標著 a 的那顆鍵仍然送 a」。
     * 前者只是搬家，後者才是這個功能能不能安全存在的前提。
     */
    @Test
    fun swappingAAndSMovesThemWithoutChangingWhatTheySend() {
        val base = load("qwerty")
        val remap = LayoutRemap("qwerty", listOf(RemapOp.Swap("lower", "a", "s")))
        val out = applyKeyRemap(base, remap).layout
        assertNotNull("a ⇄ s 應該成立", out)

        assertEquals("a 應該搬到 s 原本的格子", slotOf(base, "lower", "s"), slotOf(out!!, "lower", "a"))
        assertEquals("s 應該搬到 a 原本的格子", slotOf(base, "lower", "a"), slotOf(out, "lower", "s"))

        val a = keyOf(out, "lower", "a")
        assertEquals("a", a.label)
        assertEquals("標著 a 的鍵必須仍然送 keysym a", "a", (a.send as SendSpec.Keysym).name)
        val s = keyOf(out, "lower", "s")
        assertEquals("s", s.label)
        assertEquals("s", (s.send as SendSpec.Keysym).name)
    }

    /**
     * 使用者舉的第二個例子：九宮格兩顆鍵對調。
     *
     * 這一條是整個功能最危險的地方。九宮格「ABC」鍵送的是 `A`，那是
     * `t9_pinyin` 的 `speller/alphabet: 'ADGJMPTW'` 契約。若交換時連 send
     * 一起換過去，使用者按「ABC」會送出 `D`，librime 老實地當成 def 去查，
     * 打出來的每一個字都是錯的 —— 而鍵面上還寫著 ABC，他永遠查不出原因。
     */
    @Test
    fun swappingNinePadKeysKeepsTheSchemaContract() {
        val base = load("t9-pinyin")
        val out = applyKeyRemap(base, LayoutRemap("t9-pinyin", listOf(RemapOp.Swap("t9", "k2", "k3"))))
            .layout
        assertNotNull(out)

        assertEquals(slotOf(base, "t9", "k3"), slotOf(out!!, "t9", "k2"))
        val abc = keyOf(out, "t9", "k2")
        assertEquals("ABC", abc.label)
        assertEquals("鍵面 ABC 必須仍然送 A", "A", (abc.send as SendSpec.Keysym).name)
        val def = keyOf(out, "t9", "k3")
        assertEquals("DEF", def.label)
        assertEquals("D", (def.send as SendSpec.Keysym).name)
    }

    /** 只有位置變，其他一切不變 —— 用簽章多重集把這件事釘死。 */
    @Test
    fun aSwapChangesNothingButOrder() {
        val base = load("qwerty")
        val out = applyKeyRemap(base, LayoutRemap("qwerty", listOf(RemapOp.Swap("lower", "a", "s"))))
            .layout!!
        for (layer in base.layers) {
            val before = layer.rows.flatMap { it.keys }.map(::keySignature).sorted()
            val after = out.layer(layer.id)!!.rows.flatMap { it.keys }.map(::keySignature).sorted()
            assertEquals("層 ${layer.id} 的鍵不該有任何一顆變了內容", before, after)
        }
    }

    /* ══════════════════ 2. 驗證：擋下會壞掉的自訂 ══════════════════ */

    /**
     * 寬度不同的鍵交換 → 兩列的 `Σwidth` 一個變 9.5、一個變 10.5，
     * 規範 §9 要求等於 `units`。不擋的話鍵盤每一次打開都會歪掉一整列。
     */
    @Test
    fun swappingKeysOfDifferentWidthIsRejected() {
        val base = load("qwerty")
        val outcome = applyKeyRemap(
            base,
            LayoutRemap("qwerty", listOf(RemapOp.Swap("lower", "a", "shift"))),
        )
        assertNull("寬度不同的交換必須被拒絕", outcome.layout)
        assertTrue(
            "錯誤訊息要說出是寬度的問題：${outcome.problems}",
            outcome.problems.any { it.contains("寬度總和") },
        )
    }

    @Test
    fun anUnknownKeyIdIsRejectedRatherThanGuessed() {
        val outcome = applyKeyRemap(
            load("qwerty"),
            LayoutRemap("qwerty", listOf(RemapOp.Swap("lower", "a", "no-such-key"))),
        )
        assertNull(outcome.layout)
        assertTrue(outcome.problems.any { it.contains("no-such-key") })
    }

    @Test
    fun anUnknownLayerIsRejected() {
        val outcome = applyKeyRemap(
            load("qwerty"),
            LayoutRemap("qwerty", listOf(RemapOp.Swap("no-such-layer", "a", "s"))),
        )
        assertNull(outcome.layout)
        assertTrue(outcome.problems.any { it.contains("no-such-layer") })
    }

    @Test
    fun swappingAKeyWithItselfIsRejected() {
        val outcome = applyKeyRemap(
            load("qwerty"),
            LayoutRemap("qwerty", listOf(RemapOp.Swap("lower", "a", "a"))),
        )
        assertNull(outcome.layout)
    }

    @Test
    fun aRemapForAnotherLayoutIsRejected() {
        val outcome = applyKeyRemap(
            load("qwerty"),
            LayoutRemap("t9-pinyin", listOf(RemapOp.Swap("t9", "k2", "k3"))),
        )
        assertNull(outcome.layout)
    }

    /**
     * **v1 的硬限制。** 這一條測的是「若有人日後在引擎層加一個改 send 的操作，
     * 不變式會不會擋住他」。這裡直接把一顆鍵的 send 換掉再送進不變式檢查 ——
     * 現有的 Swap / Move 產生不出這種佈局，正是因為有這條防線。
     */
    @Test
    fun changingWhatAKeySendsIsAlwaysRejected() {
        val base = load("qwerty")
        val tampered = mapKey(base, "lower", "a") { k ->
            k.copy(send = SendSpec.Keysym("s", (keyOf(base, "lower", "s").send as SendSpec.Keysym).code, 0))
        }
        val problems = invariantProblems(base, tampered)
        assertTrue(
            "改了 send 必須被擋下：$problems",
            problems.any { it.contains("不得改變任何一顆鍵送出什麼") },
        )
    }

    /**
     * 必要鍵不可移除。v1 的操作刪不掉鍵，所以這一條現在永遠通過 ——
     * 留著是為了讓「日後有人加一個隱藏鍵的功能」在第一次跑測試時就撞牆。
     */
    @Test
    fun removingAnEssentialKeyIsRejected() {
        val base = load("qwerty")
        val stripped = removeKey(base, "lower", "backspace")
        val problems = invariantProblems(base, stripped)
        assertTrue(
            "刪掉退格鍵必須被擋下：$problems",
            problems.any { it.contains("退格") },
        )
    }

    /* ══════════════════ 3. 逃生不變式（重用同一份邏輯）══════════════════ */

    /** 合法的交換不會動到導覽圖，整份驗證（含逃生走訪）必須通過。 */
    @Test
    fun aLegalSwapPassesTheFullValidator() {
        val problems = LayoutRemapValidator.validate(
            repo,
            LayoutRemap("qwerty", listOf(RemapOp.Swap("lower", "a", "s"))),
        )
        assertTrue(problems.joinToString("\n"), problems.isEmpty())
    }

    @Test
    fun theValidatorRejectsAWidthBreakingSwap() {
        val problems = LayoutRemapValidator.validate(
            repo,
            LayoutRemap("qwerty", listOf(RemapOp.Swap("lower", "a", "shift"))),
        )
        assertTrue("應該被擋下", problems.isNotEmpty())
    }

    /**
     * 證明驗證裡真的接了逃生走訪，而不是接了一個永遠回空清單的東西。
     *
     * 作法是把 t9-pinyin 英數層那顆「九宮」回程鍵抽掉 —— 這正是真機回報過的
     * 缺陷形狀（進得去、出不來）。抽掉之後 [LayoutEscape] 必須報死路。
     */
    @Test
    fun theEscapeWalkStillCatchesALayoutYouCannotLeave() {
        val trapped = object : LayoutRepository by repo {
            override fun loadLayout(id: String): LoadResult<KeyboardLayout> {
                val r = repo.loadLayout(id)
                val v = r.value ?: return r
                if (id != "t9-pinyin") return r
                return LoadResult(removeKey(removeKey(v, "en", "to_t9"), "en_upper", "to_t9"), r.diagnostics)
            }
        }
        val problems = LayoutEscape.check("t9-pinyin") { LayoutHost(CachingLayoutRepository(trapped)) }
        assertTrue("抽掉回程鍵之後必須被判為死路", problems.isNotEmpty())
        assertTrue(problems.joinToString("\n").contains("死路"))
    }

    /**
     * **別人的缺陷不可以拿來擋使用者的自訂。**
     *
     * 模擬器上實測踩到的：在 qwerty 交換 a 與 s，卻被一串「cn-stroke 走
     * switch_layout:@primary 之後回不來」擋下 —— 那些死路是隨附佈局本來就有的
     * （見交付報告），使用者看不懂，而且他也修不了，結果是一顆永遠按不下去的
     * 按鈕。判準必須是「你的自訂有沒有讓情況變糟」。
     *
     * 這裡把 t9-pinyin 的回程鍵抽掉當成「本來就有的缺陷」，再驗證一個與它
     * 完全無關的 qwerty 交換 —— 必須通過。
     */
    @Test
    fun preexistingDeadEndsInOtherLayoutsDoNotBlockALegalSwap() {
        val broken = object : LayoutRepository by repo {
            override fun loadLayout(id: String): LoadResult<KeyboardLayout> {
                val r = repo.loadLayout(id)
                val v = r.value ?: return r
                if (id != "t9-pinyin") return r
                return LoadResult(removeKey(removeKey(v, "en", "to_t9"), "en_upper", "to_t9"), r.diagnostics)
            }
        }
        // 先確認這個夾具真的是壞的，否則這條測試會變成永遠通過的裝飾。
        assertTrue(
            "夾具本身必須先有死路，這條測試才有意義",
            LayoutEscape.check("t9-pinyin") { LayoutHost(CachingLayoutRepository(broken)) }.isNotEmpty(),
        )
        val problems = LayoutRemapValidator.validate(
            broken,
            LayoutRemap("qwerty", listOf(RemapOp.Swap("lower", "a", "s"))),
        )
        assertTrue("不得被別份佈局既有的死路擋下：$problems", problems.isEmpty())
    }

    /* ══════════════════ 4. 儲存與重置 ══════════════════ */

    @Test
    fun jsonRoundTripsAndDropsGarbage() {
        val remaps = listOf(
            LayoutRemap("qwerty", listOf(RemapOp.Swap("lower", "a", "s"))),
            LayoutRemap("t9-pinyin", listOf(RemapOp.Move("t9", "k2", "k3"))),
        )
        assertEquals(remaps, LayoutRemapJson.decode(LayoutRemapJson.encode(remaps)))

        // 壞掉的自訂檔不該讓輸入法打不開字：解不開的條目安靜略過。
        assertEquals(emptyList<LayoutRemap>(), LayoutRemapJson.decode("{ this is not json"))
        assertEquals(
            emptyList<LayoutRemap>(),
            LayoutRemapJson.decode("""{"layouts":[{"id":"qwerty","ops":[{"op":"teleport"}]}]}"""),
        )
    }

    /**
     * 重置回到的是**基礎佈局當前的原樣**，不是「開始改之前的快照」。
     *
     * 這一條是產品承諾：一個改不回去（或只能改回某個舊版）的設定本身就是陷阱。
     * 儲存層存的是「操作」而不是「改完的佈局」，所以刪掉操作就等於回到原樣，
     * 而且基礎佈局日後更新了也會跟著更新。
     */
    @Test
    fun resettingReturnsToTheCurrentBaseLayoutNotASnapshot() {
        val dir = tmp.newFolder("user")
        val store = UserLayoutStore.get(dir)
        store.put(LayoutRemap("qwerty", listOf(RemapOp.Swap("lower", "a", "s"))))

        val remapped = RemappingLayoutRepository(repo) { store.remapFor(it) }
        assertEquals(
            slotOf(load("qwerty"), "lower", "s"),
            slotOf(remapped.loadLayout("qwerty").value!!, "lower", "a"),
        )

        store.reset("qwerty")
        assertNull(store.remapFor("qwerty"))
        assertEquals(
            "重置後每一顆鍵都要回到基礎佈局的位置",
            signatureGrid(load("qwerty")),
            signatureGrid(remapped.loadLayout("qwerty").value!!),
        )
    }

    @Test
    fun resetAllClearsEveryLayout() {
        val store = UserLayoutStore.get(tmp.newFolder("user-all"))
        store.put(LayoutRemap("qwerty", listOf(RemapOp.Swap("lower", "a", "s"))))
        store.put(LayoutRemap("t9-pinyin", listOf(RemapOp.Swap("t9", "k2", "k3"))))
        assertTrue(store.hasAny())
        store.resetAll()
        assertTrue(store.all().isEmpty())
        assertNull(store.remapFor("qwerty"))
        assertNull(store.remapFor("t9-pinyin"))
    }

    /** 重開 app 之後自訂還在 —— 儲存層這一半在這裡測，端到端那一半在模擬器上。 */
    @Test
    fun aRemapSurvivesBeingReadBackFromDisk() {
        val dir = tmp.newFolder("persist")
        UserLayoutStore.get(dir).put(LayoutRemap("qwerty", listOf(RemapOp.Swap("lower", "a", "s"))))

        // 換一個目錄重讀同一份檔案：模擬「行程重啟、從檔案重新載入」。
        val other = tmp.newFolder("persist-restart")
        File(dir, UserLayoutStore.FILE_NAME).copyTo(File(other, UserLayoutStore.FILE_NAME))
        val reopened = UserLayoutStore.get(other)
        assertEquals(
            listOf(RemapOp.Swap("lower", "a", "s")),
            reopened.remapFor("qwerty")?.ops,
        )
    }

    /**
     * 正式路徑上，套不上的自訂**退回基礎佈局**而不是讓鍵盤消失。
     * 上游佈局改版把某顆鍵刪掉之後，使用者該拿到一個能用的鍵盤加一則說明。
     */
    @Test
    fun aStaleRemapFallsBackToTheBaseLayoutAtRuntime() {
        val stale = LayoutRemap("qwerty", listOf(RemapOp.Swap("lower", "a", "key-that-was-removed")))
        val lenient = RemappingLayoutRepository(repo) { if (it == "qwerty") stale else null }
        val result = lenient.loadLayout("qwerty")
        assertNotNull("正式路徑不得因為一份過期的自訂就沒有鍵盤", result.value)
        assertTrue(result.warnings.isNotEmpty())

        val strict = RemappingLayoutRepository(repo, strict = true) { if (it == "qwerty") stale else null }
        assertNull("驗證路徑必須明確失敗，不得默默用基礎佈局假裝通過", strict.loadLayout("qwerty").value)
    }

    /* ══════════════════ 夾具 ══════════════════ */

    private fun slotOf(layout: KeyboardLayout, layerId: String, keyId: String): Pair<Int, Int> {
        val layer = layout.layer(layerId)!!
        layer.rows.forEachIndexed { r, row ->
            val i = row.keys.indexOfFirst { it.id == keyId }
            if (i >= 0) return r to i
        }
        throw AssertionError("$layerId 裡沒有 $keyId")
    }

    private fun keyOf(layout: KeyboardLayout, layerId: String, keyId: String): LayoutKey =
        layout.layer(layerId)!!.rows.flatMap { it.keys }.first { it.id == keyId }

    private fun signatureGrid(layout: KeyboardLayout): List<List<String>> =
        layout.layers.flatMap { l -> l.rows.map { r -> r.keys.map(::keySignature) } }

    private fun mapKey(
        layout: KeyboardLayout,
        layerId: String,
        keyId: String,
        f: (LayoutKey) -> LayoutKey,
    ): KeyboardLayout = layout.copy(
        layers = layout.layers.map { l ->
            if (l.id != layerId) l
            else l.copy(rows = l.rows.map { r -> r.copy(keys = r.keys.map { if (it.id == keyId) f(it) else it }) })
        }
    )

    private fun removeKey(layout: KeyboardLayout, layerId: String, keyId: String): KeyboardLayout =
        layout.copy(
            layers = layout.layers.map { l ->
                if (l.id != layerId) l
                else l.copy(rows = l.rows.map { r -> r.copy(keys = r.keys.filter { it.id != keyId }) })
            }
        )
}
