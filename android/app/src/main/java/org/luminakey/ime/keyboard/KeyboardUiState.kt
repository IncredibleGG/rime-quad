package org.luminakey.ime.keyboard

import org.luminakey.ime.core.RimeCandidate
import org.luminakey.ime.core.RimeSchema
import org.luminakey.ime.core.RimeStatus
import org.luminakey.ime.prefs.UserPrefs
import org.luminakey.ime.theme.KeyAction
import org.luminakey.ime.theme.KeyboardLayout
import org.luminakey.ime.theme.LayoutLayer
import org.luminakey.ime.theme.SendSpec
import org.luminakey.ime.theme.Theme

/**
 * 鍵盤上的面板層級。
 *
 * ── 為什麼設定要放在鍵盤上 ──────────────────────────────────────────────
 * **使用者需要調整的當下，他正在打字，而不是在 App 裡。** 凡是改完會在鍵盤上
 * 看見的，就在鍵盤上改：高度要就地拖、配色要就地換、按鍵音要就地按得出聲。
 * 看不到結果的設定不叫設定，叫填表。
 *
 * ── 出口是結構性的，不是每個面板自己記得加 ──────────────────────────────
 * 每一個面板都只蓋住上面幾列字母鍵，**底列（空白、換行、中／英、退格）
 * 全程露在外面**。這條規矩有三個理由，一個比一個硬：
 *   1. 使用者不必先關掉面板才能繼續打字；
 *   2. 出口永遠看得見，按錯了直接繼續打字就好；
 *   3. 專案剛修過「從九宮格切英語就切不回來」，並加了一條把導覽鍵當成一張圖走的
 *      測試（LayoutEscapeTest）。「底列永不被蓋」讓所有鍵盤內面板在那張圖上
 *      **天生連通** —— 不是靠每個面板各自記得加一顆返回鍵，而是結構上不可能斷。
 *
 * 另外每個面板還有三條便利回路：`✕` 收掉整層、`‹` 回上一層、系統返回鍵。
 */
enum class PanelRoute {
    /** 沒有面板，就是普通鍵盤。 */
    NONE,

    /** 一頁六格的快速調整。⚙ 打開的就是它。 */
    QUICK,

    /** 鍵盤類型（方案 × 佈局）。`schema:picker` 打開的也是它。 */
    TYPES,

    /** 就地拖曳調高度：鍵盤變暗、上緣長出把手。 */
    HEIGHT,

    /** 手感：面板收成頂端一條，整個鍵盤讓出來給你按。 */
    FEEL,

    /** 候選字：候選列自己就是預覽。 */
    CANDIDATES,

    /** 外觀：配色與深淺，鍵盤即時變色。 */
    APPEARANCE,

    /** 打出來的字：繁簡、標點、空白鍵行為。 */
    TEXT,
    ;

    /**
     * 按 `‹` 或系統返回鍵時該回到哪裡。
     *
     * 純函式，由 PanelRouteTest 直接驗：**每一個 route 都必須在有限步內
     * 回到 [NONE]**。這條性質是「進得去出不來」那條防線在面板這一側的對應物。
     */
    fun back(): PanelRoute = when (this) {
        NONE -> NONE
        QUICK, TYPES -> NONE
        HEIGHT, FEEL, CANDIDATES, APPEARANCE, TEXT -> QUICK
    }

    /** 面板要不要遮住鍵區。手感與候選字是「收成頂端一條」，把鍵盤讓出來。 */
    val coversKeys: Boolean
        get() = this == QUICK || this == TYPES || this == APPEARANCE || this == TEXT
}

/** 「外觀」面板要選的一個配色（一組淺／深主題）。 */
data class ThemeChoice(val familyId: String, val name: String, val pinId: String)

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

    /**
     * 剛剛發生的那件事,講一句話,然後自己消失。
     *
     * ── 為什麼需要它（走查 A3／A4）────────────────────────────────────
     * 使用者按下工具列的「繁／简」之後,整個畫面唯一的變化是**那一個字翻面**。
     * 沒有任何一句話說發生了什麼,而且那個字本身也講不清楚它是「現在的狀態」
     * 還是「按下去會變成的狀態」——走查連拍九張截圖,位元組完全相同。
     *
     * 這一格就是那句話:「現在打的是繁體字」。它走候選列（使用者的視線剛好
     * 在那裡）,片刻之後自己消失,不擋任何操作。
     *
     * ⚠ 它的優先序在 [busyMessage] **之下**：「鍵盤還沒好」比「剛剛切了簡繁」
     * 要緊得多,而且那兩件事同時成立時,蓋掉前者是危險的。
     */
    val transientNotice: String? = null,

    /**
     * librime 現在收得了鍵嗎（`RimeRuntime.Phase.READY`）。
     *
     * ── 為什麼要一個獨立的欄位，而不是看 `busyMessage != null` ───────────
     * 因為那兩件事會分岔，而分岔的方向剛好是最危險的那一邊。`busyMessage`
     * 是**訊息**：它可以因為文案調整、因為某條路徑忘了設而是 null；
     * 而「引擎收不收得了鍵」是**事實**。上一版沒有這個事實，鍵盤畫得出來、
     * 按得動，`handleSend` 就把 `n` 原樣送進宿主（工單 #105 實測：全新安裝
     * 打 `nihao` → 宿主拿到 `nihao`）。
     *
     * 真正擋鍵的是服務層的 [org.luminakey.ime.core.InputReadiness]。這個欄位
     * 是它在畫面上的那一半：**擋了就一定要看得見**，否則使用者得到的是
     * 「按了沒反應」—— 這個專案抓過六次的那種缺陷。
     *
     * 預設 true：既有的每一份 `KeyboardUiState()` 與所有測試維持原本的行為，
     * 只有 IME service 的 `onPhase()` 會把它翻成 false。
     */
    val engineReady: Boolean = true,

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
    /** 「外觀」面板的配色清單，打開面板時才算。 */
    val themeChoices: List<ThemeChoice> = emptyList(),
    /** 目前開著哪一個面板。 */
    val panel: PanelRoute = PanelRoute.NONE,
    /**
     * 使用者偏好的當前值。面板上每一格都要印出**現在是什麼**，
     * 這是純圖示格與可用面板的分水嶺 —— 不用點進去就知道現在的值。
     */
    val prefs: UserPrefs = UserPrefs(),
    /**
     * 拖曳中的高度倍率草稿；null = 沒有人在拖。
     *
     * 拖曳期間不寫偏好：那會變成每一個像素一次 DataStore 交易。草稿只活在
     * 記憶體裡，按「好了」才落地、按「回原本」直接丟掉。
     */
    val heightDraft: Float? = null,
    /** 主題／佈局載入時累積的警示，顯示在候選列供除錯。 */
    val configProblem: String? = null,
    /**
     * 九宮格消歧：使用者**已經確定**的音節，依序。
     *
     * 由 IME service 持有（只有那裡拿得到 `rs_get_input()`），單向送到 UI。
     * 它的長度就是「現在該問第幾個音節」，也是消歧欄要顯示哪一批讀音的依據。
     */
    val confirmedSyllables: List<String> = emptyList(),
    /**
     * 這個方案**做得到**逐音節改寫嗎（由 IME service 的啟動探針回答）。
     *
     * 為什麼是 UI 的事:做不到的時候消歧欄**整條不出現**。做得到與做不到,
     * 畫面上的差別必須看得出來 —— 畫一排按下去什麼都不會發生的讀音,
     * 就是這個專案抓過六次的那種「念得出名字、按下去沒反應」的鍵。
     *
     * 預設 false 是 fail-safe:還沒問到答案之前不要畫。
     */
    val syllableRewriteReady: Boolean = false,
) {
    val layer: LayoutLayer? get() = layout?.layer(layerId) ?: layout?.layers?.firstOrNull()
}

/** 鍵盤送出的事件。IME service 是唯一的處理者。 */
sealed interface KeyboardEvent {
    /** §9.4 的 send：keysym 走引擎，text 繞過引擎。 */
    data class Send(val spec: SendSpec) : KeyboardEvent

    /** §9.5 的 action。 */
    data class Act(val action: KeyAction) : KeyboardEvent

    /**
     * 選第 [indexOnPage] 個候選 —— ⚠ **頁內相對索引**,不得攤平。
     *
     * 兩個來源:點候選列上那一格,或按專用數字列的 `1`–`9`
     * (工單 #99,判準見 [SelectionDigitKeys])。兩者送同一個事件,
     * 所以「按 3」與「點第 3 格」不可能分岔。
     */
    data class Candidate(val indexOnPage: Int) : KeyboardEvent
    data class CandidateLongPress(val indexOnPage: Int) : KeyboardEvent
    data class Page(val backward: Boolean) : KeyboardEvent

    /** 開一個面板。[PanelRoute.NONE] 等於全部收掉。 */
    data class OpenPanel(val route: PanelRoute) : KeyboardEvent

    /** 回上一層（見 [PanelRoute.back]）。 */
    data object PanelBack : KeyboardEvent

    /**
     * 使用者在鍵盤類型選單裡選了一項：方案與佈局要**同時**換過去。
     *
     * `layoutId` 為 null = 這個方案沒有可用佈局，只換方案，佈局交給 §9.1.1。
     */
    data class SelectKeyboardType(val schemaId: String, val layoutId: String?) : KeyboardEvent

    /**
     * 改一項偏好。
     *
     * 刻意是一個 lambda 而不是十幾個具名事件：面板上每一格改的都是
     * [UserPrefs] 的某一個欄位，十幾個只差欄位名的事件型別除了讓
     * `handleEvent` 變成一面牆之外沒有任何好處。偏好變更本來就有唯一的
     * 消費點（`prefsStore.update`），這裡直接把要做的事交給它。
     */
    data class EditPrefs(val block: (UserPrefs) -> UserPrefs) : KeyboardEvent

    /** 拖曳中的高度預覽。只動記憶體裡的草稿，不寫偏好。 */
    data class DragHeight(val scale: Float) : KeyboardEvent

    /** 「好了」：把草稿寫進偏好。 */
    data object CommitHeight : KeyboardEvent

    /** 「回原本」：丟掉草稿，並把使用者設過的高度一起清掉。 */
    data object ResetHeight : KeyboardEvent

    /** 「全部設定 ›」：開 App。這是面板上最不顯眼的一項，因為預設就是不去 App。 */
    data object OpenAppSettings : KeyboardEvent

    /**
     * 九宮格消歧欄：使用者點了一個讀音。
     *
     * 這**不是**候選篩選 —— 它會把引擎的輸入串改寫掉（見 [T9Syllables] 檔尾
     * 那一段），所以必須由 IME service 處理：只有那裡拿得到 session。
     */
    data class SelectSyllable(val syllable: String) : KeyboardEvent
}
