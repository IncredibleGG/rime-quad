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
 */
object KeyCells {

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
     */
    fun weights(
        sizesDp: List<Float>,
        spacingDp: Float,
        outerStartDp: Float,
        outerEndDp: Float,
    ): List<Float> = sizesDp.mapIndexed { i, s ->
        s + padStart(i, spacingDp, outerStartDp) + padEnd(i, sizesDp.size, spacingDp, outerEndDp)
    }

    /**
     * 改動前每一格畫出來的大小（dp）—— 也就是「外觀不變」要對齊的那一組數字。
     *
     * 這正是 `KeyGrid` 原本交給 `Arrangement.spacedBy` + `weight` 去算的東西：
     * 先扣掉全部間距，剩下的按 weight 分。§9.3 的「Σwidth > units 時該列等比
     * 壓縮」由分母取兩者大者表達。
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
        val unit = (availableDp - spacingDp * gaps) / denom
        return weightsIn.map { it * unit }
    }
}
