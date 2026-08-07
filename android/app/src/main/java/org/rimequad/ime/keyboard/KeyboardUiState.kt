package org.rimequad.ime.keyboard

import org.rimequad.ime.core.RimeCandidate
import org.rimequad.ime.core.RimeSchema
import org.rimequad.ime.core.RimeStatus
import org.rimequad.ime.theme.KeyAction
import org.rimequad.ime.theme.KeyboardLayout
import org.rimequad.ime.theme.LayoutLayer
import org.rimequad.ime.theme.SendSpec
import org.rimequad.ime.theme.Theme

/**
 * 軟鍵盤畫面的完整狀態。由 IME service 依 rime 快照與已載入的
 * 主題／佈局重建。
 *
 * ⚠ [layout] 與 [theme] 都來自 yaml。這一層**沒有**任何寫死的鍵盤 ——
 * 先前的 `keyboard.KeyboardLayout`（寫死 QWERTY）已刪除。
 */
data class KeyboardUiState(
    val preedit: String = "",
    val candidates: List<RimeCandidate> = emptyList(),
    val highlighted: Int = -1,
    val pageNo: Int = 0,
    val isLastPage: Boolean = true,
    val status: RimeStatus = RimeStatus(),
    /** true = 目前是 stub 假實作，UI 要明講。 */
    val isStub: Boolean = true,
    /** so 載入或 rs_init 失敗時的訊息，非 null 就顯示在候選列。 */
    val fatalMessage: String? = null,
    /** 解壓資料／首次部署等「還不能用，但不是壞掉」的狀態訊息。 */
    val busyMessage: String? = null,

    /** 目前生效的主題（來自 core/themes 的 yaml）。 */
    val theme: Theme? = null,
    /** 目前生效的佈局（來自 core/layouts 的 yaml）。 */
    val layout: KeyboardLayout? = null,
    /** 目前顯示的層 id。 */
    val layerId: String = "",
    /** 已啟用的方案清單（rs_schema_list）。 */
    val schemas: List<RimeSchema> = emptyList(),
    /**
     * 攤平後的鍵盤類型清單（方案 × 佈局），依語言分組。見 [KeyboardTypes]。
     * 在 IME service 側算好，因為只有那裡拿得到 [LayoutHost] 與方案清單。
     */
    val keyboardTypes: List<KeyboardTypeGroup> = emptyList(),
    /** 鍵盤類型選單是否展開。 */
    val schemaPickerOpen: Boolean = false,
    /** 主題／佈局載入時累積的警示，顯示在候選列供除錯。 */
    val configProblem: String? = null,
) {
    val layer: LayoutLayer? get() = layout?.layer(layerId) ?: layout?.layers?.firstOrNull()
}

/** 鍵盤送出的事件。IME service 是唯一的處理者。 */
sealed interface KeyboardEvent {
    /** §9.4 的 send：keysym 走引擎，text 繞過引擎。 */
    data class Send(val spec: SendSpec) : KeyboardEvent

    /** §9.5 的 action。 */
    data class Act(val action: KeyAction) : KeyboardEvent

    data class Candidate(val indexOnPage: Int) : KeyboardEvent
    data class CandidateLongPress(val indexOnPage: Int) : KeyboardEvent
    data class Page(val backward: Boolean) : KeyboardEvent

    data object OpenSchemaPicker : KeyboardEvent
    data object CloseSchemaPicker : KeyboardEvent

    /**
     * 使用者在鍵盤類型選單裡選了一項：方案與佈局要**同時**換過去。
     *
     * `layoutId` 為 null = 這個方案沒有可用佈局，只換方案，佈局交給 §9.1.1。
     */
    data class SelectKeyboardType(val schemaId: String, val layoutId: String?) : KeyboardEvent
}
