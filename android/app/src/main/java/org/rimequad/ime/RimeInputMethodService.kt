package org.rimequad.ime

import android.content.Intent
import android.content.res.Configuration
import android.inputmethodservice.InputMethodService
import android.text.InputType
import android.util.Log
import android.view.KeyEvent
import android.view.View
import android.view.inputmethod.EditorInfo
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.platform.LocalViewConfiguration
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.launchIn
import kotlinx.coroutines.flow.onEach
import kotlinx.coroutines.launch
import org.rimequad.ime.core.AndroidKeyMap
import org.rimequad.ime.core.RimeCore
import org.rimequad.ime.core.RimeRuntime
import org.rimequad.ime.keyboard.ConfigRepository
import org.rimequad.ime.keyboard.KeyboardEvent
import org.rimequad.ime.keyboard.KeyboardTypes
import org.rimequad.ime.keyboard.SchemaLanguages
import org.rimequad.ime.keyboard.KeyboardUiState
import org.rimequad.ime.keyboard.LayoutHost
import org.rimequad.ime.home.CurrentKeyboard
import org.rimequad.ime.home.KeyboardPanelRequest
import org.rimequad.ime.keyboard.PanelRoute
import org.rimequad.ime.keyboard.RemappingLayoutRepository
import org.rimequad.ime.keyboard.RimeKeyboard
import org.rimequad.ime.keyboard.ThemeChoice
import org.rimequad.ime.keyboard.UserLayoutStore
import org.rimequad.ime.prefs.KeyBehavior
import org.rimequad.ime.prefs.LocalKeyBehavior
import org.rimequad.ime.prefs.LongPressViewConfiguration
import org.rimequad.ime.prefs.PrefsStore
import org.rimequad.ime.prefs.SettingsActivity
import org.rimequad.ime.prefs.SpaceBehavior
import org.rimequad.ime.prefs.UserPrefs
import org.rimequad.ime.prefs.applyThemePrefs
import org.rimequad.ime.prefs.applyUserOverrides
import org.rimequad.ime.store.InstalledRegistry
import org.rimequad.ime.store.StoreSettings
import org.rimequad.ime.theme.ActionVerb
import org.rimequad.ime.theme.KeyAction
import org.rimequad.ime.theme.Keysym
import org.rimequad.ime.theme.SendSpec
import org.rimequad.ime.theme.Theme
import org.rimequad.ime.ui.RimeTheme

/**
 * IME 本體。
 *
 * 執行緒：rime_shell.h 要求同一 session 的呼叫序列化在同一條執行緒上。
 * 本類別所有 RimeCore 呼叫都發生在 IME 主執行緒 —— Compose 事件回呼、
 * onKeyDown、以及 RimeRuntime 切回主執行緒後的 phase 回呼，全都是主執行緒。
 *
 * 軟鍵盤的每一個鍵都來自 `core/layouts` 下的 yaml（見 [LayoutHost]）：
 * 這個類別只負責把佈局描述的 `send` / `tap` 翻成 rime_shell 呼叫，
 * 不再知道「QWERTY 長什麼樣」。
 */
class RimeInputMethodService : InputMethodService() {

    private companion object {
        const val TAG = "RimeIME"

        /** librime 的開關名。設定畫面與候選列工具列指的是同兩個開關。 */
        const val OPT_SIMPLIFICATION = "simplification"
        const val OPT_ASCII_PUNCT = "ascii_punct"

        /**
         * 字形互斥選項組。隨附的 luna_pinyin / luna_pinyin_tw 用這一組表達簡繁,
         * 而不是 `simplification`。見 [applyVariant] 的說明。
         */
        const val OPT_ZH_HANS = "zh_hans"
        const val OPT_ZH_HANT = "zh_hant"
        val VARIANT_OPTIONS = listOf("zh_hans", "zh_hant", "zh_hant_hk", "zh_hant_tw")

        /** X11 的 space keysym。空白鍵行為偏好靠它認出空白鍵。 */
        const val KEYSYM_SPACE = 0x0020

        /** 這一組配色等於「沒有指定主題」，見 buildThemeChoices。 */
        const val DEFAULT_THEME_FAMILY = "default"
    }

    private var session: Long = RimeCore.INVALID_SESSION
    private var host: ComposeKeyboardHost? = null

    private lateinit var config: ConfigRepository
    private lateinit var layoutHost: LayoutHost

    /** 使用者自訂鍵位。設定畫面與本服務在同一個行程，拿到的是同一個實例。 */
    private lateinit var userLayouts: UserLayoutStore

    /**
     * 上一次套用過的 [UserLayoutStore.version]。
     *
     * 設定畫面改了自訂之後，本服務不見得活著、也不見得有 view，所以不走回呼，
     * 改成「下次要用鍵盤的時候比對一次」（[onStartInputView]）。慢一拍是可以
     * 接受的 —— 使用者按下交換之後總得先回到某個輸入框才看得到鍵盤。
     */
    private var userLayoutsVersion = 0

    private var uiState by mutableStateOf(KeyboardUiState())

    /* ─────────────────── 使用者偏好 ───────────────────
     *
     * 三層覆寫的最上層(docs 的設計契約):
     *   主題 YAML → platform_overrides.android → 使用者偏好
     * 前兩層在 ThemeParser 內完成,第三層是 [applyUserOverrides],
     * 只作用在**已解析完成**的 Theme 物件上,不寫回任何 yaml。
     */

    private lateinit var prefsStore: PrefsStore

    /**
     * 目前生效的偏好。刻意用 [mutableStateOf] 而不是普通 var:
     * 長按延遲與重複速度這類設定不會改變 [uiState],若 prefs 不是可觀測的,
     * Compose 就不會因為它改變而重組,使用者要切出去再切回來才會生效 ——
     * 那就不叫「立即生效」了。
     */
    private var prefs by mutableStateOf(UserPrefs())

    /**
     * 偏好變更的收集 scope。
     *
     * ⚠ Dispatchers.Main.immediate 不是隨手挑的:偏好一變就要重下
     * rs_set_option,而 rime_shell.h 要求同一 session 的呼叫序列化在同一條
     * 執行緒上。IME 的那條執行緒就是主執行緒,所以收集者也必須落在主執行緒。
     */
    private val prefsScope = CoroutineScope(Dispatchers.Main.immediate + SupervisorJob())

    /**
     * 拖曳中的鍵盤高度草稿。null = 沒有人在拖。
     *
     * 刻意不寫進偏好：拖曳每移動一個像素就寫一次 DataStore 交易是不可行的，
     * 而且使用者按「回原本」時那些中間值必須完全不存在過。
     */
    private var heightDraft: Float? = null

    /** 上一次看到的方案 id，用來偵測方案變動並套用 §9.1.1 的自動換佈局。 */
    private var lastSchemaId: String = ""

    /** onKeyDown 消費掉的 keycode，onKeyUp 要對應吃掉，否則宿主會收到落單的 up。 */
    private val consumedKeys = HashSet<Int>()

    /**
     * 方案市集的帳本。IME 只讀它一件事：方案 id → recommended_layout。
     * 設定畫面裝完新方案後帳本會變，所以每次部署完成（phase READY）重讀一次。
     */
    private var storeRegistry: InstalledRegistry? = null

    /** 市集與 IME 之間的交接點；這裡只讀 [StoreSettings.pendingSchema]。 */
    private var storeSettings: StoreSettings? = null

    /**
     * 這個編輯框要不要繞過 librime、直接把字面上屏。判定見 [shouldBypassRime]。
     * 由 [onStartInput] / [onStartInputView] 依宿主給的 [EditorInfo] 決定。
     */
    private var bypassRime = false

    private fun reloadStoreRegistry() {
        val user = RimeRuntime.userDirOrNull ?: return
        storeRegistry = runCatching { InstalledRegistry.load(user) }.getOrNull()
        // 帳本裡的語言標記比隨 APK 出貨的那一份新，而且知道方案是哪個套件裝的
        // （方案 id 不是全域唯一的）。疊上去給鍵盤類型選單的分組用。
        SchemaLanguages.applyInstalled(storeRegistry?.languageTags() ?: emptyMap())
    }

    /**
     * 市集在設定畫面啟用了新方案之後，鍵盤要切過去。
     *
     * 為什麼不是設定畫面直接切：`rs_select_schema()` 要 session，而 session
     * 必須由本 service 在自己的執行緒上建立與使用（rime_shell.h 的執行緒
     * 約定）。所以設定畫面只留下一個字串，由這裡在**部署完成、session 重建
     * 之後**兌現，兌現完就清掉。
     *
     * ⚠ 方案還沒出現在 `rs_schema_list()` 裡時**不可以**把 pendingSchema 清掉：
     * 那代表部署失敗被回滾了，方案之後可能還會回來。清掉就等於把使用者的
     * 「我要用這個方案」這件事默默丟了。
     */
    private fun consumePendingSchema() {
        val settings = storeSettings ?: return
        val wanted = settings.pendingSchema ?: return
        if (session == RimeCore.INVALID_SESSION) return
        if (RimeCore.schemaList().none { it.id == wanted }) {
            Log.w(TAG, "待切換的方案 $wanted 不在 schema 清單裡，留著等下一次")
            return
        }
        settings.pendingSchema = null
        Log.i(TAG, "市集要求切換到方案 $wanted")
        selectSchema(wanted)
    }

    private val phaseListener: (RimeRuntime.Phase) -> Unit = { phase -> onPhase(phase) }

    override fun onCreate() {
        super.onCreate()
        uiState = uiState.copy(isStub = RimeCore.isStub())
        // 偏好必須在第一次畫鍵盤之前就位,否則鍵盤會先用主題高度畫一次
        // 再跳成使用者設定的高度。current 首次存取會阻塞讀一次檔案(幾百 bytes)。
        prefsStore = PrefsStore.get(applicationContext)
        prefs = prefsStore.current
        // 解壓在背景執行緒，rs_init 之後才切回主執行緒，這裡不會阻塞。
        RimeRuntime.start(applicationContext)

        // 佈局與主題不依賴 librime，可以先載起來 —— 鍵盤在部署完成前就畫得出來。
        config = ConfigRepository(applicationContext)
        // 使用者自訂鍵位掛在 repository 這一層：狀態機（LayoutHost）完全不必
        // 知道這件事存在，切換規則一行都沒改。見 RemappingLayoutRepository。
        userLayouts = UserLayoutStore.get(RimeRuntime.userDataDirOf(applicationContext))
        userLayoutsVersion = userLayouts.version
        layoutHost = LayoutHost(
            RemappingLayoutRepository(config) { id -> userLayouts.remapFor(id) }
        )
        // 市集裝進來的方案不可能出現在隨附佈局的 for_schema 裡（那些 yaml 跟著
        // APK 出貨，不會知道未來會裝什麼），所以佈局只能靠索引宣告的
        // recommended_layout 決定。少了這一行，使用者從市集裝了注音方案卻會
        // 看到 QWERTY 鍵面，一個字都打不出來。
        //
        // 優先序：排在 for_schema 精確命中**之後**（見 LayoutHost.applySchema）。
        // 本機佈局自己聲明「我適用於這個方案」時，不該被遠端索引蓋過去。
        layoutHost.recommendedLayoutResolver = { id -> storeRegistry?.layoutForSchema(id) }
        storeSettings = StoreSettings(applicationContext)
        reloadStoreRegistry()
        // §9.1.1 的 SHOULD：使用者明確挑過的佈局要在**第一次 applySchema 之前**
        // 就位，否則開機那一次自動切換會覆蓋掉他的選擇，而那正是這條規則要擋的。
        layoutHost.setPinnedLayouts(UserPrefs.decodeLayoutPins(prefs.layoutPins))
        layoutHost.ensureLoaded()
        layoutHost.applyThemePrefs(prefs, systemInDarkMode())
        syncConfigToUi()

        // 「設定變更立即生效」的唯一機制:設定畫面一寫入 DataStore,這裡就
        // 收到新值,重新選主題、重新套覆寫、重新下 rs_set_option。
        // 不需要重啟輸入法,也不需要 IME 與設定畫面互相知道對方存在。
        prefsStore.flow
            .onEach { onPrefsChanged(it) }
            .launchIn(prefsScope)

        RimeRuntime.addListener(phaseListener)
        Log.i(TAG, "onCreate: stub=${RimeCore.isStub()} phase=${RimeRuntime.phase}")
    }

    override fun onCreateInputView(): View {
        host?.onDestroy()
        val newHost = ComposeKeyboardHost(this)
        host = newHost
        // decorView 必須傳進去，否則 Compose 找不到 ViewTreeLifecycleOwner
        // 而在 attach 當下崩潰。詳見 ComposeKeyboardHost 的註解。
        return newHost.createView(window?.window?.decorView) {
            RimeTheme {
                // 偏好 → 渲染層的全部接線就是這兩個 CompositionLocal:
                //   · LocalKeyBehavior       震動開關與強度、按鍵音與音量、重複時間
                //   · LocalViewConfiguration 長按門檻(detectTapGestures 讀的就是它)
                // 這樣 KeyboardView 幾乎不必改,顏色與尺寸仍舊完全由主題驅動。
                val current = uiState.theme
                // ⚠ 這兩個 remember 不是效能優化，是正確性。
                //
                // 這個 lambda 每一次 uiState 變動都會重跑 —— 也就是**每按一顆鍵**。
                // 沒有 remember 的話，每次都會產生一份新的 KeyBehavior 與
                // LongPressViewConfiguration；`LocalViewConfiguration` 的值一換，
                // Compose 就對整棵樹每一個 `Modifier.pointerInput` 呼叫
                // `onViewConfigurationChange()` → `resetPointerInputHandler()`，
                // 把**手指還按著的那顆鍵**的手勢協程取消掉。
                // 使用者看到的就是「點一下鍵就變灰，再也變不回來」。
                //
                // 兩道防線：這裡讓值穩定（不再無謂地換），
                // KeyView 的 `try/finally` 讓按下狀態即使被取消也一定歸位。
                val behavior = remember(current?.feedback, prefs) {
                    if (current == null) KeyBehavior.DEFAULT
                    else KeyBehavior.of(current.feedback, prefs)
                }
                val baseViewConfiguration = LocalViewConfiguration.current
                val viewConfiguration = remember(baseViewConfiguration, behavior.longPressMs) {
                    LongPressViewConfiguration(
                        base = baseViewConfiguration,
                        longPressMs = behavior.longPressMs.toLong(),
                    )
                }
                CompositionLocalProvider(
                    LocalKeyBehavior provides behavior,
                    LocalViewConfiguration provides viewConfiguration,
                ) {
                    RimeKeyboard(state = uiState, onEvent = ::handleEvent)
                }
            }
        }
    }

    override fun onStartInput(info: EditorInfo?, restarting: Boolean) {
        super.onStartInput(info, restarting)
        // 實體鍵盤可能在 input view 出現之前就開始送字，政策要先定下來。
        applyEditorPolicy(info)
    }

    override fun onStartInputView(info: EditorInfo?, restarting: Boolean) {
        super.onStartInputView(info, restarting)
        host?.onStartInput()
        refreshUserLayoutsIfChanged()
        ensureSession()
        consumePendingSchema()
        applyEditorPolicy(info)
        refreshFromRime(consumeCommit = false)
        // 設定頁的「鍵盤高度 › 開始調」留下的一次性請求。**高度不是一個數字，
        // 是一個手勢** —— 設定頁裡只有一顆會把使用者帶到鍵盤上的按鈕，
        // 所以鍵盤升起來時要自己進入拖曳模式，否則他到了這裡還要再找一次。
        if (KeyboardPanelRequest.consume(this) == KeyboardPanelRequest.HEIGHT) {
            openPanel(PanelRoute.HEIGHT)
        }
    }

    /**
     * 依宿主的 [EditorInfo] 決定這個編輯框走不走 librime。
     *
     * ── 為什麼需要這個判定 ────────────────────────────────────────────────
     * 在這段之前，本 service 從來沒有讀過 `EditorInfo.inputType`，對每一種
     * 編輯框都一視同仁地送進 librime、再用 `setComposingText()` 畫組字區。
     * `scripts/verify_input_matrix.sh` 的型別矩陣把代價逼了出來：
     *
     * · **`TYPE_CLASS_NUMBER` 的框整個是死的。** 數字框的 `KeyListener` 會濾掉
     *   非數字字元，於是每一通 `setComposingText("ni hao")` 都被宿主靜靜丟棄
     *   ——使用者看到的就是「打字完全沒反應」；更糟的是 librime 那邊還一路
     *   把 preedit 累積下去，引擎狀態與畫面徹底脫節。
     * · **密碼會被 librime 學走。** 走 librime 就會進 speller 與使用者詞典。
     *   密碼不該留在任何詞庫裡；在密碼框關掉組字與學習是各家 IME 的慣例。
     *
     * ── 回傳 true 的意思 ─────────────────────────────────────────────────
     * 「這個框不經過 librime」：軟鍵盤的按鍵直接走 [fallbackKey] 上屏字面，
     * 實體鍵盤則原封不動交還宿主自己處理。
     */
    /**
     * 使用者剛在設定裡改過自訂鍵位 → 丟掉佈局快取重載一次。
     * 沒有變更時這是一次 int 比較，放在 `onStartInputView` 上不會有成本。
     */
    private fun refreshUserLayoutsIfChanged() {
        val v = userLayouts.version
        if (v == userLayoutsVersion) return
        userLayoutsVersion = v
        layoutHost.invalidateLayouts()
        syncConfigToUi()
        Log.i(TAG, "使用者自訂鍵位已變更（v$v），佈局重載為 ${layoutHost.layout?.id}")
    }

    private fun shouldBypassRime(info: EditorInfo?): Boolean {
        val type = info?.inputType ?: return false
        // TYPE_NULL：宿主明說「我自己處理 raw key event，別給我組字」。
        if (type == InputType.TYPE_NULL) return true
        return when (type and InputType.TYPE_MASK_CLASS) {
            // 數字／電話／日期時間框只收得下字面字元，組字區必定被濾掉。
            InputType.TYPE_CLASS_NUMBER,
            InputType.TYPE_CLASS_PHONE,
            InputType.TYPE_CLASS_DATETIME,
            -> true

            InputType.TYPE_CLASS_TEXT -> when (type and InputType.TYPE_MASK_VARIATION) {
                InputType.TYPE_TEXT_VARIATION_PASSWORD,
                InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD,
                InputType.TYPE_TEXT_VARIATION_WEB_PASSWORD,
                -> true

                else -> false
            }

            else -> false
        }
    }

    private fun applyEditorPolicy(info: EditorInfo?) {
        val bypass = shouldBypassRime(info)
        if (bypass != bypassRime) {
            Log.i(
                TAG,
                "編輯框政策：bypassRime=$bypass inputType=0x" +
                    Integer.toHexString(info?.inputType ?: 0),
            )
        }
        bypassRime = bypass
        if (bypass) {
            // 上一個框可能還留著組字狀態；不能讓它漏進密碼／數字框。
            if (session != RimeCore.INVALID_SESSION) RimeCore.clearComposition(session)
            currentInputConnection?.finishComposingText()
        }
    }

    override fun onFinishInputView(finishingInput: Boolean) {
        super.onFinishInputView(finishingInput)
        if (session != RimeCore.INVALID_SESSION) {
            RimeCore.clearComposition(session)
        }
        currentInputConnection?.finishComposingText()
        // 面板不跨編輯框存活：使用者切去別的 app 再回來，看到的是鍵盤，
        // 不是他上次忘了關的設定面板。
        heightDraft = null
        uiState = uiState.copy(panel = PanelRoute.NONE, heightDraft = null)
        host?.onFinishInput()
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        // §8.2 執行期規則第 2 條：跟隨系統時切到 counterpart。
        // §8.2 執行期規則第 2 條,再疊上使用者的「固定淺色／固定深色」偏好。
        layoutHost.applyThemePrefs(
            prefs,
            (newConfig.uiMode and Configuration.UI_MODE_NIGHT_MASK) ==
                Configuration.UI_MODE_NIGHT_YES,
        )
        syncConfigToUi()
    }

    override fun onDestroy() {
        prefsScope.cancel()
        RimeRuntime.removeListener(phaseListener)
        host?.onDestroy()
        host = null
        if (session != RimeCore.INVALID_SESSION) {
            RimeCore.sessionDestroy(session)
            session = RimeCore.INVALID_SESSION
        }
        super.onDestroy()
    }

    private fun systemInDarkMode(): Boolean =
        (resources.configuration.uiMode and Configuration.UI_MODE_NIGHT_MASK) ==
            Configuration.UI_MODE_NIGHT_YES

    /* ─────────────────── 初始化狀態 ─────────────────── */

    private fun onPhase(phase: RimeRuntime.Phase) {
        when (phase) {
            RimeRuntime.Phase.IDLE,
            RimeRuntime.Phase.EXTRACTING,
            -> uiState = uiState.copy(busyMessage = "正在準備輸入法資料…", fatalMessage = null)

            RimeRuntime.Phase.DEPLOYING ->
                uiState = uiState.copy(
                    busyMessage = "首次啟動：正在編譯詞庫，需要一到兩分鐘…",
                    fatalMessage = null,
                )

            RimeRuntime.Phase.READY -> {
                uiState = uiState.copy(
                    busyMessage = null,
                    fatalMessage = null,
                    schemas = RimeCore.schemaList(),
                )
                // 市集可能剛裝了新方案，帳本變了。
                reloadStoreRegistry()
                // 部署會讓舊 session 失效，這裡一律重建。
                rebuildSession()
                // 方案 id 可能沒變，但佈局的來源（帳本）變了。清掉快取的
                // lastSchemaId，強迫下一次快照重新套用一次佈局。
                lastSchemaId = ""
                consumePendingSchema()
                refreshFromRime(consumeCommit = false)
            }

            RimeRuntime.Phase.FAILED ->
                uiState = uiState.copy(
                    busyMessage = null,
                    fatalMessage = RimeRuntime.initError ?: "rime 初始化失敗",
                )
        }
        Log.i(TAG, "phase=$phase session=$session")
    }

    private fun rebuildSession() {
        if (session != RimeCore.INVALID_SESSION) {
            RimeCore.sessionDestroy(session)
            session = RimeCore.INVALID_SESSION
        }
        session = RimeCore.sessionCreate()
        if (session == RimeCore.INVALID_SESSION) {
            Log.e(TAG, "建立 session 失敗: ${RimeCore.lastError()}")
            return
        }
        // 新 session 不記得上一個 session 的開關,偏好要重新推一次。
        applyRimeOptions()
    }

    /** 部署後舊 session 會失效（見 rime_shell.h），這裡負責重建。 */
    private fun ensureSession() {
        if (!RimeRuntime.isReady) return
        if (!RimeCore.sessionAlive(session)) {
            rebuildSession()
            Log.i(TAG, "session 已失效，重建為 $session")
        }
    }

    /* ─────────────────── 使用者偏好 ─────────────────── */

    /**
     * 偏好變更的唯一入口。跑在主執行緒上(見 [prefsScope]),因此可以安全地
     * 呼叫 rs_set_option —— rime_shell.h 的執行緒約定在這條路徑上仍然成立。
     */
    private fun onPrefsChanged(next: UserPrefs) {
        val before = prefs
        prefs = next
        // 換主題／換深淺才需要重新載入主題檔;純值覆寫走 [effectiveTheme]。
        if (before.themeId != next.themeId || before.appearanceMode != next.appearanceMode) {
            layoutHost.applyThemePrefs(next, systemInDarkMode())
        }
        // 「回復全部預設」會把使用者指定過的佈局一起刪掉，狀態機必須跟著忘記，
        // 否則刪了設定鍵盤還記得 —— 那正是使用者按下那顆按鈕想解決的問題。
        if (before.layoutPins != next.layoutPins) {
            layoutHost.setPinnedLayouts(UserPrefs.decodeLayoutPins(next.layoutPins))
        }
        applyRimeOptions()
        syncConfigToUi()
    }

    /**
     * 把偏好裡的 librime 開關推給引擎。
     *
     * ⚠ 「未設定」(null)刻意**不呼叫** rs_set_option。那一項的語義是
     * 「不干預,讓方案自己的預設生效」;若把 null 當成 false 送出去,
     * 使用者一裝好就會被強制切成繁體與全形,而他從來沒做過那個選擇。
     */
    private fun applyRimeOptions() {
        if (session == RimeCore.INVALID_SESSION) return
        prefs.simplification?.let { applyVariant(it) }
        prefs.asciiPunct?.let { want ->
            if (RimeCore.getOption(session, OPT_ASCII_PUNCT) != want) {
                RimeCore.setOption(session, OPT_ASCII_PUNCT, want)
            }
        }
    }

    /** 使用者切成簡體之前,原本開著的那個字形選項。用來把「切回繁體」還原到正確的一支。 */
    private var savedVariantOption: String? = null

    /**
     * 簡繁切換。
     *
     * ⚠ 這裡不能只設 `simplification`。**隨附的 luna_pinyin 系列根本沒有那個開關** ——
     * 它們用的是一組互斥選項:
     *
     * ```yaml
     * - options: [ zh_hant, zh_hans, zh_hant_hk, zh_hant_tw ]
     *   states:  [ 傳統漢字, 简化字, 香港字形, 臺灣字形 ]
     * ```
     *
     * 只送 `simplification` 的話,在模擬器上實測打 `guojia` 仍然得到「國家」,
     * 設定看起來完全沒作用。`simplification` 是 simplifier 元件的**預設**
     * option_name,別的方案(例如五筆·簡入繁出)才會用到,所以兩種都要送。
     *
     * 切回繁體時**還原原本那一支**而不是硬設 `zh_hant`:使用者原本在
     * 「臺灣字形」,切一趟簡體再切回來卻變成「傳統漢字」,字形會整批改變,
     * 那不是他要求的。
     */
    private fun applyVariant(wantSimplified: Boolean) {
        if (RimeCore.getOption(session, OPT_SIMPLIFICATION) != wantSimplified) {
            RimeCore.setOption(session, OPT_SIMPLIFICATION, wantSimplified)
        }
        val current = VARIANT_OPTIONS.firstOrNull { RimeCore.getOption(session, it) }
        if (wantSimplified) {
            if (current != OPT_ZH_HANS) {
                if (current != null) savedVariantOption = current
                RimeCore.setOption(session, OPT_ZH_HANS, true)
            }
        } else if (current == OPT_ZH_HANS) {
            RimeCore.setOption(session, savedVariantOption ?: OPT_ZH_HANT, true)
        }
    }

    /** 把候選列工具列上的開關選擇回寫偏好,讓兩個入口不會漂移。 */
    private fun rememberOptionChoice(option: String, value: Boolean) {
        when (option) {
            OPT_SIMPLIFICATION ->
                prefsScope.launch { prefsStore.update { it.copy(simplification = value) } }

            OPT_ASCII_PUNCT ->
                prefsScope.launch { prefsStore.update { it.copy(asciiPunct = value) } }
        }
    }

    /* ─────────────────── 主題 ─────────────────── */

    private var overrideBase: Theme? = null
    private var overridePrefs: UserPrefs? = null
    private var overrideResult: Theme? = null

    /**
     * 三層覆寫的最後一步。
     *
     * 有快取的理由不只是省 CPU:Compose 靠**物件識別**決定要不要跳過重組,
     * 每次按鍵都產生一個新的 Theme 物件會讓整塊鍵盤每一鍵都重畫一次。
     * 偏好未設定時 [applyUserOverrides] 直接回傳原物件,所以預設路徑
     * 連一個配置都沒有。
     */
    private fun effectiveTheme(): Theme? {
        val base = layoutHost.theme ?: return null
        // 拖曳中的高度草稿就是「這一刻的偏好」——套在同一條路徑上，
        // 使用者拖到哪裡看到的就是最後的樣子，不必另做一套預覽渲染。
        val want = if (heightDraft != null) prefs.copy(keyboardHeightScale = heightDraft) else prefs
        if (base === overrideBase && want == overridePrefs) return overrideResult
        val out = applyUserOverrides(base, want)
        overrideBase = base
        overridePrefs = want
        overrideResult = out
        return out
    }

    /** 把 [LayoutHost] 的當前佈局／層／主題搬進 UI 狀態,並套上使用者覆寫。 */
    private fun syncConfigToUi() {
        uiState = uiState.copy(
            theme = effectiveTheme(),
            layout = layoutHost.layout,
            layerId = layoutHost.layerId,
            // 面板上每一格都要印出「現在是什麼」,所以偏好必須跟著進 UI 狀態。
            prefs = prefs,
            heightDraft = heightDraft,
            configProblem = layoutHost.problem,
        )
    }

    /* ─────────────────── 實體鍵盤 ───────────────────
     *
     * `adb shell input keyevent` 與真正的實體鍵盤走的都是這條路徑
     * （宿主 → InputMethodService.onKeyDown → rs_process_key），
     * 和軟鍵盤點擊完全不同。scripts/verify_rime_compose.sh 就是靠這條
     * 路徑證明 librime 真的在工作。
     */

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        // 系統返回鍵是面板的第四條回路（另外三條：底列全程露著、`✕`、`‹`）。
        // 少了這一條，使用者按返回會直接把整個鍵盤收掉 —— 面板還開著，
        // 下次叫出鍵盤又是那個面板，看起來就像卡住了。
        if (keyCode == KeyEvent.KEYCODE_BACK && uiState.panel != PanelRoute.NONE) {
            openPanel(uiState.panel.back())
            consumedKeys.add(keyCode)
            return true
        }
        if (processHardwareKey(event, release = false)) {
            consumedKeys.add(keyCode)
            return true
        }
        return super.onKeyDown(keyCode, event)
    }

    override fun onKeyUp(keyCode: Int, event: KeyEvent): Boolean {
        if (consumedKeys.remove(keyCode)) {
            // key-up 也送給 librime（有些方案靠 RS_MOD_RELEASE 做上屏），
            // 但無論它吃不吃，這個 up 都不該再交給宿主。
            processHardwareKey(event, release = true)
            return true
        }
        return super.onKeyUp(keyCode, event)
    }

    private fun processHardwareKey(event: KeyEvent, release: Boolean): Boolean {
        // 密碼／數字框：回 false 讓宿主自己處理實體按鍵，不經過 librime。
        if (bypassRime) return false
        ensureSession()
        if (session == RimeCore.INVALID_SESSION) return false

        val keysym = AndroidKeyMap.keysymOf(event)
        if (keysym == 0) return false

        val mods = AndroidKeyMap.modifiersOf(event, release)
        val consumed = RimeCore.processKey(session, keysym, mods)
        if (consumed) refreshFromRime()
        return consumed
    }

    /* ─────────────────── 軟鍵盤事件 ─────────────────── */

    private fun handleEvent(event: KeyboardEvent) {
        when (event) {
            is KeyboardEvent.Send -> handleSend(event.spec)
            is KeyboardEvent.Act -> handleAction(event.action)

            is KeyboardEvent.Candidate -> {
                RimeCore.selectCandidate(session, event.indexOnPage)
                refreshFromRime(allowAutoCommit = true)
            }

            is KeyboardEvent.CandidateLongPress -> {
                RimeCore.deleteCandidate(session, event.indexOnPage)
                refreshFromRime()
            }

            is KeyboardEvent.Page -> {
                RimeCore.changePage(session, event.backward)
                refreshFromRime()
            }

            is KeyboardEvent.OpenPanel -> openPanel(event.route)

            is KeyboardEvent.PanelBack -> openPanel(uiState.panel.back())

            is KeyboardEvent.SelectKeyboardType -> {
                selectKeyboardType(event.schemaId, event.layoutId)
                // 選完直接回到八格，不是整片收掉：使用者剛換了鍵盤，
                // 下一件想做的事多半還在同一個面板上（例如順手把高度調一下）。
                openPanel(PanelRoute.QUICK)
            }

            // 面板上每一格改的都是偏好的某一個欄位；偏好本來就有唯一的消費點，
            // 這裡直接把要做的事交給它。寫入是非同步的，但 prefsStore.flow 的
            // 收集者就在主執行緒上（見 prefsScope），畫面下一幀就會反映。
            is KeyboardEvent.EditPrefs ->
                prefsScope.launch { prefsStore.update(event.block) }

            // 拖曳期間只動記憶體裡的草稿：每一個像素一次 DataStore 交易是不可行的。
            is KeyboardEvent.DragHeight -> {
                heightDraft = event.scale
                syncConfigToUi()
            }

            is KeyboardEvent.CommitHeight -> {
                val v = heightDraft
                heightDraft = null
                if (v != null) prefsScope.launch { prefsStore.update { it.copy(keyboardHeightScale = v) } }
                openPanel(PanelRoute.QUICK)
            }

            // 「回原本」= 丟掉草稿**並且**把使用者設過的高度刪掉，回到主題檔的值。
            // 只丟草稿是不夠的：使用者上一次調過的高度還留著，畫面會停在一個
            // 他明明按了「回原本」卻沒有回去的地方。
            is KeyboardEvent.ResetHeight -> {
                heightDraft = null
                prefsScope.launch { prefsStore.update { it.copy(keyboardHeightScale = null) } }
                syncConfigToUi()
            }

            is KeyboardEvent.OpenAppSettings -> {
                openPanel(PanelRoute.NONE)
                startActivity(
                    Intent(this, SettingsActivity::class.java)
                        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                )
            }
        }
    }

    /* ─────────────────── 鍵盤上的面板 ─────────────────── */

    /**
     * 面板的唯一切換點。
     *
     * 清單在**打開的當下**才算，不是持續維護的狀態：方案可能剛從市集裝進來、
     * 佈局可能剛被使用者放進 user_data_dir、主題目錄可能剛多了一份 —— 三者
     * 都不會通知我們。開一次算一次是最省事也最不會過期的作法。
     */
    private fun openPanel(route: PanelRoute) {
        var next = uiState.copy(panel = route)
        if (route == PanelRoute.TYPES) {
            val schemas = RimeCore.schemaList()
            next = next.copy(
                schemas = schemas,
                keyboardTypes = KeyboardTypes.build(
                    schemas,
                    layoutHost.layoutBriefs(ConfigRepository.LOCALE),
                ),
            )
        }
        if (route == PanelRoute.APPEARANCE && uiState.themeChoices.isEmpty()) {
            next = next.copy(themeChoices = buildThemeChoices())
        }
        // 離開高度編輯器時把草稿收掉，不要讓一個沒按「好了」的預覽跟著使用者走。
        if (route != PanelRoute.HEIGHT && heightDraft != null) heightDraft = null
        uiState = next
        syncConfigToUi()
    }

    /**
     * 「外觀」面板上那一排配色。
     *
     * 一組淺／深主題在使用者眼裡是**一個**配色 —— 深淺是旁邊那個控制項的事。
     * 所以這裡按 id 去掉 `-light` / `-dark` 收成家族，並一律釘淺色那一份：
     * 跟隨系統時 LayoutHost 會在夜間自己跳到它宣告的 counterpart。
     */
    private fun buildThemeChoices(): List<ThemeChoice> {
        val ids = runCatching { config.themeIds() }.getOrDefault(emptyList())
        val out = LinkedHashMap<String, ThemeChoice>()
        for (id in ids.sorted()) {
            val family = id.removeSuffix("-light").removeSuffix("-dark")
            if (out.containsKey(family)) continue
            val theme = config.loadTheme(id).value ?: continue
            var name = theme.name.get(ConfigRepository.LOCALE).ifEmpty { id }
            for (suffix in listOf("淺色", "深色", "Light", "Dark")) {
                if (name.endsWith(suffix)) name = name.dropLast(suffix.length)
            }
            name = name.trimEnd(' ', '・', '·', '-').ifEmpty { family }
            val pin = if (id.endsWith("-dark")) "$family-light" else id
            // 「預設」那一組對應的是**沒有指定主題**，不是「指定 default-light」。
            // 存一份預設值的副本正是 UserPrefs 檔頭第一條要避開的事：主題檔日後
            // 更新了，抄過副本的使用者會被永遠釘在舊的那一份上。
            val pinned = if (family == DEFAULT_THEME_FAMILY) {
                ""
            } else if (ids.contains(pin)) {
                pin
            } else {
                id
            }
            out[family] = ThemeChoice(family, name, pinned)
        }
        return out.values.toList()
    }

    /* ─────────────────── 鍵盤類型選單 ─────────────────── */

    /**
     * 使用者在選單裡挑了一種鍵盤：方案與佈局**同時**換過去。
     *
     * 順序要緊：先把「使用者明確挑了這份佈局」記進 [LayoutHost]，再切方案。
     * `selectSchema` 會讓下一次快照重跑 §9.1.1，而 §9.1.1 的第 0 步就是讀這筆
     * 記錄 —— 於是佈局是被自動規則**依使用者的意圖**選中的，不是事後再硬掰
     * 回來的。最後那次 `switchLayout` 只是保險：方案切換失敗（例如方案剛被
     * 刪掉）時，使用者至少還是換到了他點的那份鍵盤。
     */
    private fun selectKeyboardType(schemaId: String, layoutId: String?) {
        layoutHost.pinLayout(schemaId, layoutId)
        persistLayoutPins()
        selectSchema(schemaId)
        if (layoutId != null && layoutHost.layout?.id != layoutId) {
            layoutHost.switchLayout(layoutId)
        }
        CurrentKeyboard.publish(this, schemaId, layoutHost.layout?.id)
        syncConfigToUi()
    }

    /** 把 [LayoutHost] 的指定表寫回偏好。非同步，寫失敗只影響下次開機時的記憶。 */
    private fun persistLayoutPins() {
        val encoded = UserPrefs.encodeLayoutPins(layoutHost.pinnedLayouts())
        prefsScope.launch {
            runCatching { prefsStore.update { it.copy(layoutPins = encoded) } }
                .onFailure { Log.w(TAG, "寫入佈局指定失敗", it) }
        }
    }

    /** §9.4：`send` 有且僅有兩種形態，互斥。 */
    private fun handleSend(spec: SendSpec) {
        ensureSession()
        when (spec) {
            is SendSpec.Keysym -> {
                // 靜態表查得到就用它；查不到本應回落到 RimeGetKeycodeByName()，
                // 但 rime_shell 的 C ABI 目前沒有暴露那個函式（見回報中的 ABI 缺口），
                // 所以這裡只能讓該鍵變成 noop。
                val code = spec.code
                if (code == Keysym.VOID_SYMBOL) {
                    Log.w(TAG, "keysym '${spec.name}' 無法解析，該鍵視為 noop")
                    return
                }
                // ⚠ 規範缺口:「組字中按空白鍵要做什麼」在主題與佈局格式裡
                // 都沒有欄位可以描述,佈局只能寫 send: { keysym: "space" }。
                // 偏好選「一律送空白」時,先把組字沖出去再送半形空白 ——
                // 順序與 §9.4.1 的字面文字路徑相同,理由也相同(commitText
                // 會取代整個組字區)。未設定時完全不介入,行為與改動前一致。
                if (code == KEYSYM_SPACE && prefs.spaceBehavior == SpaceBehavior.ALWAYS_SPACE) {
                    // 送空白之前必須先把組字沖出去:InputConnection.commitText()
                    // 會**取代整個組字區**,直接送空白會讓使用者剛打好的 preedit
                    // 無聲消失,而 librime 那邊仍以為自己在組字。
                    //
                    // 不得改用 rs_clear_composition:使用者按空白的意思是
                    // 「把剛打的字上屏,然後加空白」,不是「把剛打的字丟掉」。
                    //
                    // ⚠ §9.4.1 的字面文字路徑(SendSpec.Text)有同樣的要求,
                    // 目前由另一位負責修復。這裡刻意自帶一份最小實作,不去依賴
                    // 那一側尚未定案的 helper;等那邊落地後可以合併成同一支。
                    if (RimeCore.commitComposition(session)) {
                        refreshFromRime(consumeCommit = true, allowAutoCommit = false)
                    }
                    currentInputConnection?.finishComposingText()
                    currentInputConnection?.commitText(" ", 1)
                    layoutHost.afterKeySent()
                    syncConfigToUi()
                    return
                }
                // bypassRime 時完全不問 librime，直接走字面上屏那條路。
                val consumed = !bypassRime && RimeCore.processKey(session, code, spec.modifiers)
                if (!consumed) fallbackKey(code)
                layoutHost.afterKeySent()
                refreshFromRime()
            }

            is SendSpec.Text -> {
                // 形態 B：**繞過 librime**，直接上屏。
                //
                // ⚠ 但不能直接 commitText —— 見 [flushCompositionForLiteralText]。
                flushCompositionForLiteralText()
                currentInputConnection?.commitText(spec.text, 1)
                layoutHost.afterKeySent()
                syncConfigToUi()
            }
        }
    }

    /**
     * §9.4.1：送出「字面文字」之前，必須先把組字內容沖出去。
     *
     * ── 不這麼做會發生什麼 ──────────────────────────────────────────────
     * `InputConnection.commitText()` 會**取代整個組字區**。所以在組字中直接
     * 送標點，使用者剛打好的 preedit 會無聲消失，而 librime 那邊完全不知情
     * ——它仍然認為自己在組字，下一次按鍵會把已經消失的 preedit 重新畫回來。
     * 重現路徑：注音方案打 `su3`（ㄋㄧˇ）→ 切到全形標點層 → 按「，」。
     *
     * ── 正確順序 ────────────────────────────────────────────────────────
     *   1. rs_commit_composition()
     *   2. 取下一份快照，把 commit_text 送給宿主
     *   3. finishComposingText()
     *   4. 才送字面文字（由呼叫端負責）
     *
     * ── 兩個容易搞錯的地方 ──────────────────────────────────────────────
     * · **不得用 rs_clear_composition() 代替。** 使用者按標點的意思是
     *   「把剛打的字上屏，然後加標點」，不是「把剛打的字丟掉」。
     * · **不套用 rs_commit_composition 註解裡的 menu.count 政策。** 那條政策
     *   回答的是「使用者按了確認鍵，現在該不該上屏」；這裡使用者按的是一顆
     *   無關的標點鍵，語義是「我這段打完了」，所以無論 menu.count 為何都
     *   應該沖出去。是兩個不同的問題，別混用。
     */
    private fun flushCompositionForLiteralText() {
        ensureSession()
        if (session == RimeCore.INVALID_SESSION) {
            // 沒有 session 也要把宿主那邊的組字區收乾淨，否則後面的
            // commitText 會把殘留的 preedit 一起吃掉。
            currentInputConnection?.finishComposingText()
            return
        }
        // 沒有在組字時 rs_commit_composition 回 false，什麼都不會發生。
        if (RimeCore.commitComposition(session)) {
            // 記憶體約定：每個輸入事件只 acquire 一次，並在那一次消費掉
            // commit_text。refreshFromRime 就是那唯一的一次。
            refreshFromRime(consumeCommit = true, allowAutoCommit = false)
        }
        currentInputConnection?.finishComposingText()
    }

    /** §9.5 的 action 分派。 */
    private fun handleAction(action: KeyAction) {
        val ic = currentInputConnection
        when (action.verb) {
            ActionVerb.NOOP -> Unit

            ActionVerb.LAYER -> action.arg?.let { layoutHost.setLayer(it) }
            ActionVerb.LAYER_ONCE -> action.arg?.let { layoutHost.setLayerOnce(it) }
            ActionVerb.LAYER_LOCK -> action.arg?.let { layoutHost.lockLayer(it) }
            ActionVerb.SWITCH_LAYOUT -> action.arg?.let { layoutHost.switchLayout(it) }

            ActionVerb.TOGGLE_OPTION -> action.arg?.let { opt ->
                val next = !RimeCore.getOption(session, opt)
                RimeCore.setOption(session, opt, next)
                // 工具列的簡繁／標點鍵與設定畫面是同一件事的兩個入口。
                // 不回寫的話,使用者在鍵盤上切成簡體、回設定畫面卻看到「繁體」,
                // 而且下一次 session 重建又會被偏好切回去。
                rememberOptionChoice(opt, next)
                refreshFromRime()
            }

            ActionVerb.SET_OPTION -> {
                val opt = action.args.getOrNull(0)
                val on = action.args.getOrNull(1) == "on"
                if (opt != null) {
                    RimeCore.setOption(session, opt, on)
                    refreshFromRime()
                }
            }

            ActionVerb.SCHEMA_NEXT -> cycleSchema(1)
            ActionVerb.SCHEMA_PREV -> cycleSchema(-1)
            // §8.6.6.1 的必備項。工具列的地球鍵、佈局裡任何 `tap: schema:picker`
            // 的鍵都落在這裡 —— 全部走同一個入口，選單內容只有一份。
            ActionVerb.SCHEMA_PICKER -> openPanel(PanelRoute.TYPES)

            ActionVerb.SCHEMA_SELECT -> action.arg?.let { selectSchema(it) }

            ActionVerb.CANDIDATE_SELECT -> {
                val n = action.arg?.toIntOrNull() ?: return
                RimeCore.selectCandidate(session, n)
                refreshFromRime(allowAutoCommit = true)
            }

            ActionVerb.CANDIDATE_DELETE -> {
                val n = action.arg?.toIntOrNull() ?: return
                RimeCore.deleteCandidate(session, n)
                refreshFromRime()
            }

            ActionVerb.CANDIDATE_NEXT_PAGE -> {
                RimeCore.changePage(session, false)
                refreshFromRime()
            }

            ActionVerb.CANDIDATE_PREV_PAGE -> {
                RimeCore.changePage(session, true)
                refreshFromRime()
            }

            // rime_shell 沒有「移動高亮」的函式，只有換頁。見回報中的 ABI 缺口。
            ActionVerb.CANDIDATE_NEXT, ActionVerb.CANDIDATE_PREV ->
                Log.w(TAG, "action ${action.raw} 尚無對應的 rime_shell 呼叫，忽略")

            ActionVerb.CURSOR_LEFT -> sendHostKey(KeyEvent.KEYCODE_DPAD_LEFT)
            ActionVerb.CURSOR_RIGHT -> sendHostKey(KeyEvent.KEYCODE_DPAD_RIGHT)
            ActionVerb.CURSOR_HOME -> sendHostKey(KeyEvent.KEYCODE_MOVE_HOME)
            ActionVerb.CURSOR_END -> sendHostKey(KeyEvent.KEYCODE_MOVE_END)

            ActionVerb.CLEAR -> {
                RimeCore.clearComposition(session)
                // `finishComposingText()` 只是**結束**組字區 —— 裡面的字會原樣
                // 留在輸入框裡。而 `clear`（九宮格的「重輸」）的語義是「這串打
                // 錯了，丟掉重來」，使用者按下去卻看到 MG 留在框裡，等於這顆鍵
                // 沒作用。要真的丟掉，得先把組字區的內容換成空字串。
                ic?.setComposingText("", 1)
                ic?.finishComposingText()
                refreshFromRime()
            }

            ActionVerb.HIDE_KEYBOARD -> requestHideSelf(0)

            // 規範 §8.6.6.1 把 settings 列為工具列必備項。改動前它直接開 App ——
            // 那正是這一版要避開的事：**使用者需要調整的當下，他正在打字。**
            // 現在這顆齒輪打開的是鍵盤上的六格面板，App 只留在面板右下角
            // 那一行最不顯眼的「全部設定 ›」後面。
            ActionVerb.SETTINGS -> openPanel(PanelRoute.QUICK)

            // 表情面板尚未實作，維持 noop 而不是假裝有效。
            ActionVerb.EMOJI -> Log.i(TAG, "emoji 面板尚未實作")
        }
        syncConfigToUi()
    }

    private fun sendHostKey(keyCode: Int) {
        val ic = currentInputConnection ?: return
        ic.sendKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, keyCode))
        ic.sendKeyEvent(KeyEvent(KeyEvent.ACTION_UP, keyCode))
    }

    /* ─────────────────── 方案切換 ─────────────────── */

    private fun cycleSchema(delta: Int) {
        val list = RimeCore.schemaList()
        if (list.isEmpty()) return
        val cur = list.indexOfFirst { it.id == uiState.status.schemaId }
        val next = ((if (cur < 0) 0 else cur) + delta + list.size) % list.size
        selectSchema(list[next].id)
    }

    /**
     * 切換方案，並依 §9.1.1 連帶換掉鍵盤佈局。
     * 注音方案因此會自動換上 `bopomofo-dachen`，不需要使用者再設定一次。
     */
    private fun selectSchema(schemaId: String) {
        ensureSession()
        if (!RimeCore.selectSchema(session, schemaId)) {
            Log.e(TAG, "切換方案 $schemaId 失敗: ${RimeCore.lastError()}")
            return
        }
        Log.i(TAG, "方案 → $schemaId")
        // 使用者從選單裡明確點了一個方案,語義是「把鍵盤換成這個方案該有的
        // 樣子」——**即使他點的就是目前這個方案**。清掉 lastSchemaId 逼下一次
        // 快照重跑 §9.1.1,否則 refreshFromRime 的「方案沒變就不動」會讓選單
        // 失去救援能力:人卡在別的佈局上(例如九宮格按了 ABC 跳去 qwerty),
        // 重選一次九宮格拼音卻什麼都不會發生。
        lastSchemaId = ""
        refreshFromRime(consumeCommit = false)
    }

    private fun fallbackKey(keysym: Int) {
        val ic = currentInputConnection ?: return
        when {
            keysym == AndroidKeyMap.BACKSPACE -> ic.deleteSurroundingText(1, 0)

            keysym == AndroidKeyMap.RETURN -> {
                if (!sendDefaultEditorAction(true)) {
                    ic.sendKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_ENTER))
                    ic.sendKeyEvent(KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_ENTER))
                }
            }

            // X11 規則：Latin-1 範圍內 keysym == 碼位。
            keysym in 0x20..0xFF -> ic.commitText(keysym.toChar().toString(), 1)

            keysym and 0x01000000 != 0 ->
                ic.commitText(String(Character.toChars(keysym and 0x00FFFFFF)), 1)
        }
    }

    /**
     * 從 rime 取快照並同步到 UI 與 InputConnection。
     *
     * rime_shell.h 明訂「每個輸入事件只 acquire 一次」，且 commit 在 acquire
     * 當下就被消費。所以整條路徑上只有這個函式會取快照，自動確認也遞迴回
     * 這裡而不是另外開一次 acquire —— 否則待讀取的 commit_text 會被吃掉。
     */
    private fun refreshFromRime(
        consumeCommit: Boolean = true,
        allowAutoCommit: Boolean = false,
    ) {
        val ic = currentInputConnection
        val snapshot = RimeCore.snapshot(session)

        if (snapshot == null) {
            uiState = uiState.copy(
                preedit = "",
                candidates = emptyList(),
                highlighted = -1,
                isStub = RimeCore.isStub(),
                theme = effectiveTheme(),
                layout = layoutHost.layout,
                layerId = layoutHost.layerId,
                prefs = prefs,
                heightDraft = heightDraft,
                configProblem = layoutHost.problem,
            )
            return
        }

        if (consumeCommit) {
            snapshot.commitText?.takeIf { it.isNotEmpty() }?.let { text ->
                ic?.commitText(text, 1)
            }
        }

        // 「選字」不等於「上屏」。判別條件是 menu.count，見 rime_shell.h：
        //   count > 0               → 還有段落待選，繼續選，不要 commit
        //   count == 0 && composing → 轉換完成待確認，呼叫 rs_commit_composition
        //   count == 0 && !composing→ 已經結束
        if (allowAutoCommit &&
            snapshot.status.isComposing &&
            snapshot.menu.candidates.isEmpty()
        ) {
            Log.i(TAG, "自動確認：count=0 且仍在組字，呼叫 rs_commit_composition")
            if (RimeCore.commitComposition(session)) {
                refreshFromRime(consumeCommit = true, allowAutoCommit = false)
                return
            }
        }

        // 組字串以 composing text 呈現，讓宿主應用看得到未上屏的內容。
        if (ic != null) {
            if (snapshot.composition.preedit.isNotEmpty()) {
                ic.setComposingText(snapshot.composition.preedit, 1)
            } else {
                ic.finishComposingText()
            }
        }

        // §9.1.1：方案變了就換佈局。放在這裡而不是 selectSchema，
        // 是因為方案也可能由 librime 自己切（例如 schema 的 switcher）。
        val schemaId = snapshot.status.schemaId
        if (schemaId.isNotEmpty() && schemaId != lastSchemaId) {
            lastSchemaId = schemaId
            layoutHost.applySchema(schemaId)
            // 換方案會重置 session 的開關,使用者選過的簡繁／標點要再推一次。
            applyRimeOptions()
            // App 那一側要顯示「現在打的是哪一種鍵盤」,而它拿不到 IME 的執行期
            // 狀態。這裡單向廣播一次,是整條線唯一的寫入點。
            CurrentKeyboard.publish(this, schemaId, layoutHost.layout?.id)
            Log.i(TAG, "方案 $schemaId → 佈局 ${layoutHost.layout?.id}")
        }

        // §8.6.6 的 max_visible(可被使用者偏好覆寫):0 = 有多少畫多少。
        // 在這裡截而不是在 KeyboardView 截,是因為 take 保序,截斷後的索引
        // 與 rime 的頁內索引仍然一一對應,選字與刪字路徑一行都不用改。
        val shownTheme = effectiveTheme()
        val cap = shownTheme?.candidates?.bar?.maxVisible ?: 0
        val visible =
            if (cap > 0) snapshot.menu.candidates.take(cap) else snapshot.menu.candidates

        uiState = uiState.copy(
            preedit = snapshot.composition.preedit,
            candidates = visible,
            highlighted = snapshot.menu.highlighted,
            pageNo = snapshot.menu.pageNo,
            isLastPage = snapshot.menu.isLastPage,
            status = snapshot.status,
            isStub = RimeCore.isStub(),
            theme = shownTheme,
            layout = layoutHost.layout,
            layerId = layoutHost.layerId,
            prefs = prefs,
            heightDraft = heightDraft,
            configProblem = layoutHost.problem,
            schemas = if (uiState.schemas.isEmpty()) RimeCore.schemaList() else uiState.schemas,
        )
    }
}
