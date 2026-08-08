package org.rimequad.ime.home

import android.content.Context
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import org.rimequad.ime.R
import org.rimequad.ime.core.RimeCore
import org.rimequad.ime.core.RimeRuntime
import org.rimequad.ime.core.RimeSchema
import org.rimequad.ime.keyboard.ConfigRepository
import org.rimequad.ime.keyboard.KeyboardType
import org.rimequad.ime.keyboard.KeyboardTypes
import org.rimequad.ime.keyboard.LanguageTable
import org.rimequad.ime.keyboard.LayoutHost
import org.rimequad.ime.keyboard.SchemaLanguages
import org.rimequad.ime.prefs.PrefsStore
import org.rimequad.ime.prefs.UserPrefs
import org.rimequad.ime.store.SchemaListPatch
import org.rimequad.ime.store.StoreSettings
import java.io.File

/**
 * 「你要注音還是拼音、全鍵還是九宮格」—— 第一次啟動全程只問這一件事。
 *
 * ── 為什麼是攤平的一份清單 ──────────────────────────────────────────────
 * 專案內部把「方案」（決定怎麼把按鍵碼變成字）與「佈局」（決定鍵盤長什麼樣）
 * 分成兩件事，工程上是對的。但使用者心裡只有一件事：「我要用注音大千」。
 * [KeyboardTypes] 已經把兩者攤平成一份清單，這裡直接用同一份 —— **不另做一套**，
 * 也因此引導頁的四張卡與設定裡的四張卡、鍵盤上浮層裡的那幾張卡是同一個模型：
 * 學過一次就永遠會了。
 *
 * ── 為什麼 App 不直接切方案 ──────────────────────────────────────────────
 * `rs_select_schema()` 需要一個 session，而 session 必須由 IME service 在它
 * 自己的執行緒上建立與使用（rime_shell.h 的執行緒約定）。所以 App 只留下
 * 兩樣東西：一筆佈局指定（走偏好，IME 收到就套用）與一個待切換方案
 * （走 [StoreSettings.pendingSchema]，IME 醒來時兌現）。這條路市集早就在用，
 * 這裡不另發明第二條。
 */

/* ─────────────────────── 目前是哪一種鍵盤 ─────────────────────── */

/**
 * IME 把「現在畫的是哪一種鍵盤」寫在這裡，App 讀它來顯示現值。
 *
 * 為什麼不放 DataStore：這是一份**由 IME 單向廣播的執行期事實**，不是使用者
 * 偏好。混進 UserPrefs 會讓「全部回復預設」把它也刪掉，然後首頁顯示「—」，
 * 而使用者其實什麼都沒變。獨立一個幾十 bytes 的 SharedPreferences 最省事，
 * 而且同步讀 —— 首頁第一幀就有值，不會先閃一個空白再跳出來。
 */
object CurrentKeyboard {

    private const val PREFS = "rimequad-current-keyboard"
    private const val KEY_SCHEMA = "schema_id"
    private const val KEY_LAYOUT = "layout_id"

    fun publish(context: Context, schemaId: String, layoutId: String?) {
        if (schemaId.isEmpty()) return
        context.applicationContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .edit()
            .putString(KEY_SCHEMA, schemaId)
            .putString(KEY_LAYOUT, layoutId)
            .apply()
    }

    /** `schemaId to layoutId`；還沒有人打過字時是 null。 */
    fun read(context: Context): Pair<String, String?>? {
        val p = context.applicationContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        val schema = p.getString(KEY_SCHEMA, null)?.takeIf { it.isNotEmpty() } ?: return null
        return schema to p.getString(KEY_LAYOUT, null)
    }
}

/**
 * App → 鍵盤的一次性請求：「下次醒來請直接把某個面板打開」。
 *
 * 目前只有一個用途：設定頁的「鍵盤高度 › 開始調」。**高度不是一個數字，
 * 是一個手勢** —— 設定頁裡沒有滑桿，只有一顆會把使用者帶到鍵盤上的按鈕，
 * 而按下去之後鍵盤必須自己進入拖曳模式，否則使用者到了鍵盤上還要再找一次。
 *
 * 用「請求 + 消費」而不是持久狀態：面板打開一次就算數，留著會變成
 * 「每次叫出鍵盤都跳出調高度」這種擺脫不掉的東西。
 */
object KeyboardPanelRequest {

    const val HEIGHT = "height"

    private const val PREFS = "rimequad-current-keyboard"
    private const val KEY = "pending_panel"

    fun request(context: Context, panel: String) {
        context.applicationContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .edit().putString(KEY, panel).apply()
    }

    /** 讀完就清掉。回 null = 沒有人請求。 */
    fun consume(context: Context): String? {
        val p = context.applicationContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        val v = p.getString(KEY, null) ?: return null
        p.edit().remove(KEY).apply()
        return v
    }
}

/* ─────────────────────── 清單 ─────────────────────── */

/**
 * 現在有哪些鍵盤可以選。
 *
 * 每次呼叫都重算：方案可能剛從市集裝進來、佈局可能剛被使用者放進
 * user_data_dir，兩者都不會通知我們。算一次很便宜，過期卻會讓使用者
 * 選到一個不存在的東西。
 */
fun availableKeyboards(context: Context): List<KeyboardType> {
    // 部署跑完之前 rs_schema_list() 回空 —— 但引導頁上「你要注音還是拼音」
    // 這個問題**正好**是要在等待那十幾秒裡問的，等到引擎答得出來，那一屏早就
    // 過去了。所以這裡在引擎沉默時退到檔案旁路，見 [schemasFromFiles]。
    val schemas = RimeCore.schemaList().ifEmpty { schemasFromFiles() }
    if (schemas.isEmpty()) return emptyList()
    val host = LayoutHost(ConfigRepository(context))
    val briefs = runCatching { host.layoutBriefs(ConfigRepository.LOCALE) }.getOrDefault(emptyList())
    return KeyboardTypes.build(schemas, briefs, localizedLanguages())
        .flatMap { it.types }
        .map { it.localized(context) }
}

/**
 * 分組小標的在地化。
 *
 * ── 為什麼不是加一份 strings.xml 就好 ──────────────────────────────────
 * 這些標題不是寫在程式裡的字面常數，是**資料**：`core/schema-languages.json`
 * 的 `languages[].name`，由打包端 `scripts/schema_store/languages.py` 產生，
 * 而且是四端共用的一份，只有一種語言（zh-Hant）。把它翻成三份等於要求
 * 打包端也懂在地化，然後每加一個語言就要重跑一次打包。
 *
 * 但那一份資料裡有比顯示名更有用的東西：**BCP 47 標記**（`zh-Hant-TW`、
 * `yue-Hant-HK`、`ja`…）。標記是機器可解的，平台自己就會把它渲染成使用者
 * 語言的說法 —— 這正是 ICU 存在的理由。所以：
 *
 *   · UI 是繁體中文 → 直接用打包端那份精心寫過的名字（「中文（臺灣正體）」
 *     比 ICU 的「中文（繁體，台灣）」好，那是人挑過的字）。
 *   · 其他語言 → 把標記交給平台渲染（en 得到 “Chinese (Traditional, Taiwan)”，
 *     zh-Hans 得到「中文（繁体，台湾）」）。渲染不出來就退回原本那份。
 *
 * 查不到標記的方案走 [KeyboardTypes.groupTitleOf] 的字面啟發式，回的是那五個
 * 常數，由 [localizedGroupTitle] 對到 strings.xml。
 */
private fun localizedLanguages(): LanguageTable {
    val table = SchemaLanguages.table
    if (table.names.isEmpty()) return table
    val ui = java.util.Locale.getDefault()
    if (ui.language == "zh" && !looksSimplified(ui)) return table
    return table.copy(
        names = table.names.mapValues { (tag, curated) ->
            runCatching { java.util.Locale.forLanguageTag(tag).getDisplayName(ui) }
                .getOrNull()
                ?.takeIf { it.isNotBlank() && !it.equals(tag, ignoreCase = true) }
                ?: curated
        }
    )
}

/** zh 底下只有簡體要走 ICU；繁體用打包端那份人寫的名字。 */
private fun looksSimplified(l: java.util.Locale): Boolean =
    l.script == "Hans" || (l.script.isEmpty() && l.country in setOf("CN", "SG"))

internal fun localizedGroupTitle(context: Context, raw: String): String = when (raw) {
    KeyboardTypes.ZH_TW -> context.getString(R.string.lang_group_zh_tw)
    KeyboardTypes.ZH_HK -> context.getString(R.string.lang_group_zh_hk)
    KeyboardTypes.YUE -> context.getString(R.string.lang_group_yue)
    KeyboardTypes.ZH -> context.getString(R.string.lang_group_zh)
    KeyboardTypes.OTHER -> context.getString(R.string.lang_group_other)
    else -> raw
}

/**
 * 「這個方案一份佈局都配不上」時 [KeyboardTypes] 會放一張寫著
 * [KeyboardType.AUTO_LABEL] 的卡。那個常數是純函式裡的字面值（它要能在
 * JVM 單元測試裡被斷言），所以在地化在這裡做，不在那裡做。
 */
private fun KeyboardType.localized(context: Context): KeyboardType =
    if (layoutName == KeyboardType.AUTO_LABEL) {
        copy(layoutName = context.getString(R.string.keyboard_auto_label))
    } else {
        this
    }

/**
 * 引導頁上要問的那個問題，只放得下**幾張卡**。
 *
 * 全清單是（方案 × 佈局）的乘積，隨方案愈裝愈多會膨脹到十幾二十項 ——
 * 那在設定頁裡是對的（他是特地來挑的），在引導頁上是災難：使用者只想趕快
 * 打字，卻先被丟一份清單。
 *
 * 挑法：**每個方案各留一到兩項**，順序沿用 [KeyboardTypes] 已經排好的
 * （為這個方案而做的佈局在前、primary 全鍵盤次之），總數封頂六張。
 * 方案多的時候每個只留一項，讓「有哪幾種輸入方式」這件事一眼看得完；
 * 剩下的在「設定 › 鍵盤」裡一項都不少。
 */
fun starterKeyboards(all: List<KeyboardType>): List<KeyboardType> {
    if (all.isEmpty()) return all
    val groups = all.groupBy { it.schemaId }
    val perSchema = if (groups.size >= 4) 1 else 2
    return groups.values.flatMap { it.take(perSchema) }.take(6).map { it.withoutIdSuffix() }
}

/**
 * 拿掉標題尾巴那個括號裡的 id（「九宮格拼音（cn-t9-pinyin）」→「九宮格拼音」）。
 *
 * [KeyboardTypes] 在**同名**佈局並排時會把 id 補在後面，那在「我特地來挑一個
 * 鍵盤」的清單裡是對的 —— 不補的話兩張卡長得一模一樣，猜錯就是換到一個他沒要
 * 的鍵盤。但引導頁上每個方案只留一張卡，沒有東西要分辨，那串 id 就只剩噪音，
 * 而且是使用者第一次看到這個 app 的時候。
 */
private fun KeyboardType.withoutIdSuffix(): KeyboardType {
    val i = layoutName.indexOf("（")
    if (i <= 0 || !layoutName.trimEnd().endsWith("）")) return this
    return copy(layoutName = layoutName.substring(0, i).trim())
}

/**
 * 依方案切開的清單：`方案名 → 這個方案底下的幾種鍵盤`。
 *
 * 攤平是對的 —— 使用者不必知道「方案」和「佈局」是兩個維度。但攤平之後，
 * 隨著國際佈局愈加愈多，同一份 QWERTY 會在每個方案底下各出現一次，
 * 二十幾張卡有一半只差第二行那個方案名，掃過去只看得到一片
 * 「QWERTY・…」。加回方案這一層小標，卡片就只剩「這個方案有哪幾種排法」，
 * 一眼掃得完。這是三星選單的做法（「简体中文（中国大陆）」當小標）。
 */
fun availableKeyboardGroups(context: Context): List<Pair<String, List<KeyboardType>>> {
    val all = availableKeyboards(context)
    val out = LinkedHashMap<String, MutableList<KeyboardType>>()
    for (t in all) out.getOrPut(t.subtitle) { ArrayList() } += t
    return out.entries.map { localizedGroupTitle(context, it.key) to it.value.toList() }
}

/** 目前這一種鍵盤在清單裡的那一項；找不到就回 null（顯示成「—」）。 */
fun currentKeyboardOf(context: Context, all: List<KeyboardType>): KeyboardType? {
    val (schemaId, layoutId) = CurrentKeyboard.read(context) ?: return null
    return all.firstOrNull { it.schemaId == schemaId && it.layoutId == layoutId }
        ?: all.firstOrNull { it.schemaId == schemaId }
}

/**
 * 首頁那一行灰字：「注音 · 大千排列」。
 *
 * 兩個名字都不翻譯 —— 方案名（「朙月拼音」）是專有名稱，佈局名由 yaml 的
 * §4.9 在地化欄位自己解析。這裡在地化的只有「兩者之間怎麼接」與「還沒選」。
 */
@Composable
fun describeKeyboard(type: KeyboardType?): String =
    if (type == null) stringResource(R.string.summary_no_keyboard_yet)
    else stringResource(R.string.summary_keyboard, type.title, type.subtitle)

/**
 * 使用者挑了一種鍵盤。
 *
 * 順序要緊：**先記佈局指定，再留下待切換方案**。IME 收到方案變更時會跑
 * §9.1.1 的自動換佈局，而那條規則的第 0 步就是讀使用者明確指定過的佈局 ——
 * 於是佈局是被自動規則「依使用者的意圖」選中的，不是事後再硬掰回來的。
 */
suspend fun applyKeyboardChoice(context: Context, type: KeyboardType) {
    val store = PrefsStore.get(context)
    val pins = UserPrefs.decodeLayoutPins(store.current.layoutPins).toMutableMap()
    if (type.layoutId != null) pins[type.schemaId] = type.layoutId else pins.remove(type.schemaId)
    store.update { it.copy(layoutPins = UserPrefs.encodeLayoutPins(pins)) }
    StoreSettings(context).pendingSchema = type.schemaId
    // 樂觀更新：使用者點下去到 IME 下一次醒來之間可能隔好幾秒，這段時間裡
    // 畫面必須已經反映他的選擇，否則他會以為沒點到而再點一次。
    CurrentKeyboard.publish(context, type.schemaId, type.layoutId)
}

/**
 * 直接從檔案讀「有哪些方案、它們叫什麼」。
 *
 * ⚠ 這是一條**唯讀旁路**，不是 `rs_schema_list()` 的替代品：部署一完成就以
 * 引擎的清單為準（見 [availableKeyboards] 的 `ifEmpty`）。它存在的唯一理由是
 * 首次啟動那十幾秒 —— 那段時間引擎還在編詞庫，什麼都答不出來，而那正是
 * 我們想拿來問使用者「要哪一種鍵盤」的時間。
 *
 * 兩個來源都是既有的事實，不新增任何格式：
 *   · 啟用了哪些 → `default.custom.yaml` 的 `schema_list` patch（[SchemaListPatch]）
 *   · 叫什麼名字 → 各方案 yaml 的 `schema: name:`
 *
 * 讀不到名字的方案**直接不列**：介面上寧可少一項，也不要出現 `luna_pinyin_tw`
 * 這種只有開發者看得懂的字串。
 */
internal fun schemasFromFiles(): List<RimeSchema> {
    val user = RimeRuntime.userDirOrNull ?: return emptyList()
    val dirs = listOfNotNull(user, RimeRuntime.sharedDirOrNull)
    val ids = runCatching { SchemaListPatch.read(user) }.getOrDefault(emptyList())
    return ids.mapNotNull { id ->
        val name = dirs.firstNotNullOfOrNull { dir ->
            val f = File(dir, "$id.schema.yaml")
            if (f.isFile) runCatching { schemaNameOf(f.readText(Charsets.UTF_8)) }.getOrNull() else null
        }
        if (name.isNullOrBlank()) null else RimeSchema(id, name)
    }
}

/**
 * 從一份 schema yaml 抽出 `schema: name:`。
 *
 * 刻意用最笨的逐行掃描而不是接上 MiniYaml：這裡只要一個字串，而 schema yaml
 * 用的是完整 YAML（含 `__include` / `__patch` 這些 librime 專屬指令），
 * 拿一個為主題格式寫的子集解析器去啃它只會多出一堆要處理的例外。
 * 抽不到就回 null，呼叫端會把那一項整個丟掉。
 */
internal fun schemaNameOf(yaml: String): String? {
    var inSchema = false
    for (raw in yaml.lineSequence()) {
        val line = raw.substringBefore('#').trimEnd()
        if (line.isBlank()) continue
        if (!line.first().isWhitespace()) {
            inSchema = line.trimEnd() == "schema:"
            continue
        }
        if (!inSchema) continue
        val t = line.trim()
        if (t.startsWith("name:")) {
            return t.removePrefix("name:").trim().trim('"', '\'').takeIf { it.isNotEmpty() }
        }
    }
    return null
}

/* ─────────────────────── 卡片 ─────────────────────── */

/**
 * 一張鍵盤卡：中文名 + 方案名 + 一個**形狀縮圖**。
 *
 * 形狀縮圖（三欄 = 九宮格、十欄 = 全鍵盤）比任何圖示都準確，也不必畫圖示 ——
 * 使用者一眼就分得出「鍵很多」和「鍵很少」，而那正是他真正在選的東西。
 * 卡片上不出現 `luna_pinyin_tw`、`t9_pinyin` 這類代號。
 */
@Composable
fun KeyboardCard(
    type: KeyboardType,
    selected: Boolean,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    /** false = 上面已經有方案名當小標了，卡片上不必再印一次。 */
    showSubtitle: Boolean = true,
) {
    val border = if (selected) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.outline
    Column(
        modifier = modifier
            .clip(RoundedCornerShape(14.dp))
            .background(
                if (selected) MaterialTheme.colorScheme.primaryContainer
                else MaterialTheme.colorScheme.surface
            )
            .clickable(onClick = onClick)
            .padding(14.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(
                text = type.title,
                fontSize = 15.5.sp,
                fontWeight = FontWeight.SemiBold,
                // 兩行：鍵盤名常常是「QWERTY・常駐數字列」這種長度，一行截斷之後
                // 兩張卡的開頭長得一模一樣，使用者只能亂猜。
                maxLines = 2,
                lineHeight = 19.sp,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier.weight(1f),
            )
            if (selected) Text("✓", fontSize = 15.sp, color = MaterialTheme.colorScheme.primary)
        }
        if (showSubtitle) {
            Text(
                text = type.subtitle,
                fontSize = 12.5.sp,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        }
        Spacer(Modifier.height(10.dp))
        ShapeThumb(columns = if (looksLikeGrid(type)) 3 else 10, tint = border)
    }
}

/**
 * 九宮格的判定只看佈局 id 裡有沒有 `t9`。
 *
 * ⚠ 這是**啟發式，不是資料**：佈局格式沒有「這是幾欄」的欄位，而縮圖只是
 * 給人看的提示，猜錯的後果是圖畫得不像，不是功能壞掉。真正要做對，得在
 * 佈局格式裡補一個欄位；已記在回報中。
 */
private fun looksLikeGrid(type: KeyboardType): Boolean =
    type.layoutId?.contains("t9") == true

@Composable
private fun ShapeThumb(columns: Int, tint: androidx.compose.ui.graphics.Color) {
    Column(verticalArrangement = Arrangement.spacedBy(3.dp)) {
        repeat(3) {
            Row(horizontalArrangement = Arrangement.spacedBy(3.dp)) {
                repeat(columns) {
                    Box(
                        Modifier
                            .weight(1f)
                            .height(if (columns <= 4) 8.dp else 6.dp)
                            .clip(RoundedCornerShape(1.5.dp))
                            .background(tint)
                    )
                }
                if (columns <= 4) Spacer(Modifier.width(1.dp))
            }
        }
    }
}

/** 兩欄卡片格。奇數項時右半格留白 —— 不要讓最後一項橫跨整列，那看起來像別的東西。 */
@Composable
fun KeyboardGrid(
    types: List<KeyboardType>,
    selectedKey: String?,
    onPick: (KeyboardType) -> Unit,
    modifier: Modifier = Modifier,
    showSubtitle: Boolean = true,
) {
    Column(modifier = modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(10.dp)) {
        for (pair in types.chunked(2)) {
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                for (t in pair) {
                    KeyboardCard(
                        type = t,
                        selected = t.key == selectedKey,
                        onClick = { onPick(t) },
                        modifier = Modifier.weight(1f),
                        showSubtitle = showSubtitle,
                    )
                }
                if (pair.size == 1) Spacer(Modifier.weight(1f).size(0.dp))
            }
        }
    }
}
