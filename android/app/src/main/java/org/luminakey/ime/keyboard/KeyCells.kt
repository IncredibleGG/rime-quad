package org.luminakey.ime.keyboard

/**
 * 鍵縫不是死區：**每一顆鍵的命中格含它兩側的一半間距**。
 *
 * ── 問題長什麼樣（實測，emulator-5558，1080×2400 @420dpi，default-light）──
 * `qwerty` 的字母鍵畫出來是 34.74 × 47.95 dp，而鍵距 6 dp、列距 12 dp
 * **不屬於任何一顆鍵**：手指落在兩顆鍵中間的那幾 dp 上，什麼都不會發生。
 * Material 建議的最小觸控目標是 48 × 48 dp。
 *
 * 一支 10 欄的鍵盤在 411 dp 寬的螢幕上，每欄本來就只有 41 dp —— **48 dp 在
 * 寬度上是做不到的**（同一台機器上 Gboard 量到 35.4 dp）。所以目標不是湊到
 * 48，而是**把縫還給鍵**：34.74 × 47.95 → 40.74 × 59.95 dp，螢幕上每一個
 * 像素都屬於離它最近的那顆鍵。
 *
 * ── 為什麼不能只是把 `Arrangement.spacedBy` 拿掉 ─────────────────────────
 * 拿掉之後 Compose 會把整條寬度重新按 weight 分配，每顆鍵**畫出來的**大小
 * 就變了（非等寬的那幾列差得更明顯）。這一輪的硬要求是「命中面積變、外觀
 * 不變」，所以間距要**折進 weight**，再由鍵自己往內縮回來：
 *
 *   · 容器：`spacedBy(0)`，padding 往外讓出半個間距（[outerPad]）；
 *   · 每一格的 weight = 原本畫出來的大小 + 它兩側各自要吃的那一段（[weights]）；
 *   · 鍵在**手勢之後、背景之前**縮回去（[padStart] 等），
 *     於是命中格是整格、畫出來的還是原來那一塊。
 *
 * ── 外側那半個縫討不回來的情形 ──────────────────────────────────────────
 * 主題的 padding 比半個間距還小時（`default-light` 的 `padding.top = 4`
 * 對 `row_spacing = 12`），外側只讓得出 4 dp。[outerPad] 因此取兩者的小者：
 * 內側每一道縫都還在，最外側那一圈以 padding 為上限 —— 螢幕邊緣外沒有空間，
 * 這一點是物理的，不是選擇。
 *
 * ── 排不下的時候（工單 #104）──────────────────────────────────────────
 * `Modifier.weight()` 的第一行是 `require(weight > 0.0)`。這裡算出來的每一個
 * 數字最後都會走進那一行（`KeyboardView.kt` 1638 / 1698 / 1740 / 1746），
 * 所以**「不可能是 ≤ 0」這件事釘在本檔的出口**，不在四個呼叫點各寫一次。
 *
 * 原本沒有下界：`unit = (availableDp - spacingDp * gaps) / denom` 在
 * `availableDp` 小到連間距都塞不下時是負的，整列 weight 跟著變負 ——
 * 於是**鍵盤在組字途中整個崩掉**（實測：CI 上那幾張 44 KB 的空白截圖
 * 與 READY 逾時就是這個，見 [KeyWeightFloorTest]）。
 *
 * 降級的規矩只有兩條，兩條都只在「本來就畫不出東西」的寬度底下生效：
 *
 *   1. **間距排不下就不排間距**（[visibleSizes]）。剩下的寬度仍然按宣告的
 *      `width` 等比分掉 —— 空白鍵仍然是最寬的那一顆，鍵的先後不會翻轉。
 *      使用者看到的是「縫被擠沒了的鍵盤」，不是重排過的鍵盤。
 *      ⚠ 不選「等分」：等分會讓空白鍵縮成跟 `a` 一樣寬，而這種狀態多半是
 *        暫態（轉向、視窗重建），閃一下重排過的鍵盤比擠一下更難看。
 *      ⚠ 不選「最小鍵寬 + 橫向裁切」：`weight` 是**比例**，一列永遠剛好填滿
 *        分到的寬度，「畫不下就凸出去」在這個模型裡表達不出來；真做出來也
 *        只是把鍵畫到視窗外 —— 一顆按不到的鍵比一顆窄鍵糟。
 *      ⚠ 不選「不畫那一列」：少一列的鍵盤是使用者按不到 Enter 的鍵盤，
 *        而且那是第二條程式路徑，卡在降級狀態的風險由它引進。
 *   2. **出口一律夾到 [MIN_WEIGHT] 以上**（[weights]）。連寬度本身都 ≤ 0
 *      的那一幀（`BoxWithConstraints` 給 0 dp、或視窗重建的暫態）沒有任何
 *      比例可言，整列平均分 —— 反正那個容器是零寬，畫面上什麼都沒有。
 *
 * **降級不會卡住**：本檔全部是純函式，`innerW` 每一次量測都從
 * `BoxWithConstraints` 的 `maxWidth` 重算，沒有任何東西被 `remember` 起來。
 * 暫態過去、下一次量測拿到正常寬度，畫面就自己回來了（[KeyWeightFloorTest]
 * 的 `degradationIsNotSticky` 守著這一條）。
 */
object KeyCells {

    /**
     * 交給 `Modifier.weight()` 的下界。
     *
     * 值本身沒有意義（`weight` 是比例，整列同時取這個值就是平均分），
     * 意義只有一個：**不是 0、不是負數、不是 NaN**。
     */
    const val MIN_WEIGHT: Float = 1e-3f

    /**
     * 一格的四邊各要往內縮多少（dp）。
     *
     * 這幾個數字加起來就是「命中格比畫出來的那一塊大多少」。
     */
    data class Inset(
        val start: Float,
        val end: Float,
        val top: Float,
        val bottom: Float,
    ) {
        companion object {
            /** 不折縫 —— 行為與這個修法之前完全相同，測試的對照組。 */
            val ZERO = Inset(0f, 0f, 0f, 0f)
        }
    }

    /**
     * 容器兩端各自讓出多少（dp）。
     *
     * 讓出半個間距，第一格的可見邊緣就回到原本的 `padding` 位置；
     * padding 不夠時以 padding 為上限（見檔頭最後一段）。
     */
    fun outerPad(paddingDp: Float, spacingDp: Float): Float =
        minOf(spacingDp / 2f, maxOf(paddingDp, 0f)).coerceAtLeast(0f)

    /** 第 [index] 格的**起始側**要吃多少（dp）。 */
    fun padStart(index: Int, spacingDp: Float, outerStartDp: Float): Float =
        if (index <= 0) outerStartDp else spacingDp / 2f

    /** 第 [index] 格的**結束側**要吃多少（dp）。 */
    fun padEnd(index: Int, count: Int, spacingDp: Float, outerEndDp: Float): Float =
        if (index >= count - 1) outerEndDp else spacingDp / 2f

    /**
     * 每一格該吃多少 weight。
     *
     * @param sizesDp 每一格**改動前畫出來的**大小（dp）。見 [visibleSizes]。
     * @return 與 [sizesDp] 等長；總和 = Σ sizesDp + 全部間距 + 兩端讓出的量，
     *   也就是容器扣掉新 padding 之後的那個長度。
     *   **每一項都保證 > 0 且非 NaN**（見檔頭「排不下的時候」第 2 條）。
     */
    fun weights(
        sizesDp: List<Float>,
        spacingDp: Float,
        outerStartDp: Float,
        outerEndDp: Float,
    ): List<Float> = sizesDp.mapIndexed { i, s ->
        val raw = s + padStart(i, spacingDp, outerStartDp) +
            padEnd(i, sizesDp.size, spacingDp, outerEndDp)
        atLeastMinWeight(raw)
    }

    /**
     * `Modifier.weight()` 收得下的值 —— 這是本檔對外的唯一保證。
     *
     * NaN 也走這一條：`require(NaN > 0.0)` 是 `false`，Compose 一樣會丟。
     */
    fun atLeastMinWeight(weight: Float): Float =
        if (weight.isNaN() || weight < MIN_WEIGHT) MIN_WEIGHT else weight

    /**
     * 改動前每一格畫出來的大小（dp）—— 也就是「外觀不變」要對齊的那一組數字。
     *
     * 這正是 `KeyGrid` 原本交給 `Arrangement.spacedBy` + `weight` 去算的東西：
     * 先扣掉全部間距，剩下的按 weight 分。§9.3 的「Σwidth > units 時該列等比
     * 壓縮」由分母取兩者大者表達。
     *
     * 間距扣不完（`availableDp` ≤ 全部間距）時**放棄間距**，把剩下的寬度按
     * [weightsIn] 等比分掉；連寬度都 ≤ 0 就全部回 0，由 [weights] 的下界接手。
     * 理由與被否掉的其他三個降級方式見檔頭「排不下的時候」。
     *
     * @param weightsIn 每一格的 weight（鍵是 `width`，列是 `row.weight`；
     *   列末的 slack 由呼叫端當成一格附在最後）。
     * @param total 分母（鍵：`layer.units`；列：Σ row.weight）。
     * @param availableDp 扣掉 padding 之後、**含**間距的那段長度。
     */
    fun visibleSizes(
        weightsIn: List<Float>,
        total: Float,
        availableDp: Float,
        spacingDp: Float,
    ): List<Float> {
        if (weightsIn.isEmpty()) return emptyList()
        val denom = maxOf(total, weightsIn.sum()).coerceAtLeast(1e-4f)
        val gaps = (weightsIn.size - 1).coerceAtLeast(0)
        val room = availableDp - spacingDp * gaps
        // ⚠ `room > 0f` 對 NaN 是 false —— NaN 走的是降級那一條，最後由
        //   [weights] 的下界收掉。任何一條路都不會有 NaN 走到 Modifier.weight()。
        val unit = if (room > 0f) room / denom else maxOf(availableDp, 0f) / denom
        return weightsIn.map { it * unit }
    }
}
