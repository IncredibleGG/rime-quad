package org.rimequad.ime.keyboard

import org.rimequad.ime.core.RimeCandidate

/** 軟鍵盤畫面的完整狀態。由 IME service 依 rime 快照重建。 */
data class KeyboardUiState(
    val preedit: String = "",
    val candidates: List<RimeCandidate> = emptyList(),
    val highlighted: Int = -1,
    val pageNo: Int = 0,
    val isLastPage: Boolean = true,
    val schemaName: String = "",
    val asciiMode: Boolean = false,
    /** true = 目前是 stub 假實作，UI 要明講。 */
    val isStub: Boolean = true,
    /** so 載入或 rs_init 失敗時的訊息，非 null 就顯示在候選列。 */
    val fatalMessage: String? = null,
    /** 解壓資料／首次部署等「還不能用，但不是壞掉」的狀態訊息。 */
    val busyMessage: String? = null,
    val shift: Boolean = false,
    val layer: KeyLayer = KeyLayer.LETTERS,
)

/** 鍵盤送出的事件。IME service 是唯一的處理者。 */
sealed interface KeyboardEvent {
    data class Key(val key: KeyDef) : KeyboardEvent
    data class Candidate(val indexOnPage: Int) : KeyboardEvent
    data class CandidateLongPress(val indexOnPage: Int) : KeyboardEvent
    data class Page(val backward: Boolean) : KeyboardEvent
    data object ToggleShift : KeyboardEvent
    data object ToggleLayer : KeyboardEvent
    data object ToggleLanguage : KeyboardEvent
}
