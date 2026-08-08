package org.rimequad.ime.keyboard

import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.yield
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * 迴歸測試：按下狀態不得被「鎖」在按下色。
 *
 * ── 使用者看到的 ────────────────────────────────────────────────────────
 * 「本來都是白色的，點一下他就變成灰色了，變不回來白色了。但是下面那個
 * 九宮格拼音都是白色的。」（三星 S24U，九宮格 `cn-t9-pinyin`）
 *
 * ── 因果鏈（每一段都在真機／模擬器上驗過）────────────────────────────────
 * 1. IME 的 `setContent` lambda 每一次 `uiState` 變動都會重跑 —— 也就是
 *    **每按一顆鍵**。它當時每次都 new 一份 `LongPressViewConfiguration`
 *    提供給 `LocalViewConfiguration`。
 * 2. `LocalViewConfiguration` 的值一換，`LayoutNode.viewConfiguration` 的
 *    setter 就對整棵樹每一個 `Modifier.pointerInput` 呼叫
 *    `onViewConfigurationChange()`，其實作是 `resetPointerInputHandler()`。
 * 3. 於是**手指還按著的那一顆鍵**，它的手勢協程當場被取消，
 *    `tryAwaitRelease()` 以 `PointerInputResetException` 拋出。
 * 4. 舊碼把 `pressed = false` 寫在 `tryAwaitRelease()` **後面**，那一行
 *    因此一次都沒執行。這個狀態的唯一擁有者就是那條已死的協程，
 *    沒有任何地方會把它改回來 —— 鍵就永遠是按下色。
 *
 * 空白鍵是唯一沒中的鍵，而那正好是最關鍵的線索：它有 `long_press`，
 * 所以 `fireOnDown == false`，按下的當下不送鍵、`uiState` 不變、
 * 不會有第 1 步，於是它每次都乾淨地放開。
 *
 * ── 這支測試守的是哪一半 ────────────────────────────────────────────────
 * 修法有兩道防線：
 *   · 上游：`RimeInputMethodService` 把那兩個物件 `remember` 起來、並給
 *     `LongPressViewConfiguration` 值相等性（見 KeyBehaviorIdentityTest），
 *     讓重置根本不再發生；
 *   · 下游：本檔測的 [trackPressed] —— **就算**重置再度發生，
 *     按下狀態也一定歸位。
 *
 * 只修上游是不夠的：任何一個新的 CompositionLocal、任何一次
 * `pointerInput` key 的調整，都會讓同一個症狀原地復活。
 *
 * 渲染那一半（顏色真的變回去、多次點擊、切佈局）測不到 JVM 上 ——
 * 那需要整個 Compose 執行期與觸控注入，本專案的單元測試沒有 Robolectric
 * 也沒有 compose-ui-test。那一半由 S24U 幾何下的實機截圖驗收。
 */
class KeyPressStateTest {

    @Test
    fun pressThenReleaseGoesBackToNotPressed() = runBlocking {
        val seen = mutableListOf<Boolean>()
        val release = CompletableDeferred<Unit>()

        val job = launch {
            trackPressed(seen::add) { release.await() }
        }
        yield()
        assertEquals("按下的當下就要是按下色", listOf(true), seen)

        release.complete(Unit)
        job.join()
        assertEquals(listOf(true, false), seen)
    }

    /** 缺陷本身：協程被取消時，按下狀態仍然必須歸位。 */
    @Test
    fun cancellingTheGestureCoroutineStillClearsPressed() = runBlocking {
        val seen = mutableListOf<Boolean>()
        val never = CompletableDeferred<Unit>()

        val job = launch { trackPressed(seen::add) { never.await() } }
        yield()
        assertEquals(listOf(true), seen)

        // Compose 重置 pointerInput 時，取消就是這樣送到懸掛點上的。
        job.cancel()
        job.join()

        assertEquals("取消也要走到歸位那一行", listOf(true, false), seen)
    }

    /** 放開路徑丟例外（例如上層的 onEvent 爆掉）也不能把鍵留在按下色。 */
    @Test
    fun anExceptionInTheAwaitPathStillClearsPressed() = runBlocking {
        val seen = mutableListOf<Boolean>()
        var thrown = false
        try {
            trackPressed(seen::add) { throw IllegalStateException("boom") }
        } catch (e: IllegalStateException) {
            thrown = true
        }
        assertTrue(thrown)
        assertEquals(listOf(true, false), seen)
    }

    /**
     * 連按多次：每一次都必須是「按下 → 放開」的完整一輪，
     * 不能有任何一輪停在按下色。使用者的截圖是**幾乎每一顆鍵**都灰掉，
     * 那正是一次一次累積出來的。
     */
    @Test
    fun repeatedPressesNeverAccumulateAStuckKey() = runBlocking {
        val seen = mutableListOf<Boolean>()
        repeat(10) { i ->
            val gate = CompletableDeferred<Unit>()
            val job = launch { trackPressed(seen::add) { gate.await() } }
            yield()
            // 一半正常放開、一半被重置取消 —— 兩條路都不准留下按下色。
            if (i % 2 == 0) gate.complete(Unit) else job.cancel()
            job.join()
        }
        assertEquals(20, seen.size)
        seen.chunked(2).forEach { assertEquals(listOf(true, false), it) }
        assertFalse("最後不能停在按下色", seen.last())
    }

    /**
     * 巢狀取消：外層 scope 被砍（Compose 卸下整個節點）時也一樣。
     */
    @Test
    fun cancellingTheWholeScopeClearsPressed() = runBlocking {
        val seen = mutableListOf<Boolean>()
        val scope = CoroutineScope(Dispatchers.Unconfined)
        val never = CompletableDeferred<Unit>()
        scope.launch { trackPressed(seen::add) { never.await() } }
        assertEquals(listOf(true), seen)

        scope.cancel()
        assertEquals(listOf(true, false), seen)
    }
}

private fun CoroutineScope.cancel() = coroutineContext[kotlinx.coroutines.Job]!!.cancel()
