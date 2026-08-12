package org.luminakey.ime.prefs

import androidx.compose.runtime.Composable
import androidx.compose.ui.res.stringArrayResource
import org.luminakey.ime.R

/**
 * 檔位標籤的**在地化來源**。
 *
 * ── 為什麼標籤不能留在 [PrefLevels] 裡 ──────────────────────────────────
 * [PrefLevels] 是純函式：不碰 Android、不碰 Compose，所以那幾組檔位換算
 * 可以直接由 JVM 單元測試守著（PrefLevelsTest）。而「關／小／中／大」要跟著
 * 系統語言走，就一定得經過資源系統，也就一定得要一個 Context。兩件事的
 * 相依方向相反，硬塞在同一個物件裡的下場是純函式被 Android 汙染。
 *
 * 所以切開：**數字留在 PrefLevels，字搬到 strings.xml**。這裡是把兩者接起來
 * 的那一層，薄到只有六個 getter。
 *
 * ── 順序即契約 ──────────────────────────────────────────────────────────
 * 陣列的第 n 項對應 `PrefLevels.indexOf*()` 回傳的 n。少一項或多一項都會讓
 * 分段控制與實際值錯開，而畫面上完全看不出來 —— 使用者只會覺得「我選中，
 * 它卻是小」。三份 strings.xml 的長度一致由 `scripts/check_strings.py` 守著，
 * 對不對得上 PrefLevels 的檔位數由同一支腳本比對。
 */
object PrefLabels {

    val sound: List<String>
        @Composable get() = stringArrayResource(R.array.levels_sound).toList()

    val timbre: List<String>
        @Composable get() = stringArrayResource(R.array.levels_timbre).toList()

    val haptic: List<String>
        @Composable get() = stringArrayResource(R.array.levels_haptic).toList()

    val longPress: List<String>
        @Composable get() = stringArrayResource(R.array.levels_long_press).toList()

    val candidateCount: List<String>
        @Composable get() = stringArrayResource(R.array.levels_candidate_count).toList()

    val candidateSize: List<String>
        @Composable get() = stringArrayResource(R.array.levels_candidate_size).toList()
}
