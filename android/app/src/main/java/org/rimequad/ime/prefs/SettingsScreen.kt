@file:OptIn(ExperimentalLayoutApi::class)

package org.rimequad.ime.prefs

import android.content.res.Configuration
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.material3.Card
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.launch
import org.rimequad.ime.core.RimeCore
import org.rimequad.ime.core.RimeRuntime
import org.rimequad.ime.keyboard.ConfigRepository
import org.rimequad.ime.theme.HapticStrength
import org.rimequad.ime.theme.HintPosition
import org.rimequad.ime.theme.Theme
import org.rimequad.ime.update.UpdateController
import org.rimequad.ime.update.UpdateSection
import java.util.Locale
import kotlin.math.roundToInt

/**
 * 設定畫面。
 *
 * ── 每一列的共同約定 ────────────────────────────────────────────────────
 * 控制項旁邊一律標出**主題檔的值**，而且只要使用者設過就出現「回復預設」。
 * 這不是裝飾：三層覆寫的最上層若不把下層的值顯示出來，「回復預設」按下去
 * 會像什麼都沒發生，使用者無從判斷自己現在是不是在覆寫狀態。
 *
 * ── 為什麼多半是倍率 ────────────────────────────────────────────────────
 * 見 [UserPrefs] 的檔頭：存絕對值會讓使用者被釘在某一版主題的數字上。
 * 畫面上顯示成百分比，並把主題原值寫在副標題裡，使用者才知道自己在動什麼。
 */
@Composable
fun SettingsScreen(
    modifier: Modifier = Modifier,
    onOpenSchemaStore: (() -> Unit)? = null,
) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val store = remember { PrefsStore.get(context) }
    val prefs by store.flow.collectAsState(initial = store.current)

    val repo = remember { ConfigRepository(context) }
    val themeIds = remember { repo.themeIds() }
    val systemDark = (LocalConfiguration.current.uiMode and Configuration.UI_MODE_NIGHT_MASK) ==
        Configuration.UI_MODE_NIGHT_YES
    // 主題檔的原值（未套任何使用者覆寫）—— 每一列的「預設」都從這裡來。
    val base: Theme = remember(prefs.themeId, prefs.appearanceMode, systemDark) {
        resolveBaseTheme(repo, prefs, systemDark)
    }

    fun edit(block: (UserPrefs) -> UserPrefs) {
        scope.launch { store.update(block) }
    }

    val updates = remember { UpdateController.get(context) }

    LazyColumn(
        modifier = modifier.fillMaxWidth(),
        verticalArrangement = Arrangement.spacedBy(10.dp),
        contentPadding = PaddingValues(bottom = 32.dp),
    ) {

        /* ─────────────────────────── 更新 ─────────────────────────── */

        // 排在最前面：有新版本時那顆小紅點必須第一眼看得到，往下捲三頁才
        // 發現的提示等於沒有提示。沒有新版本時它也只是一張矮卡片。
        item {
            UpdateSection(
                controller = updates,
                autoCheck = prefs.autoCheckUpdate,
                onAutoCheckChange = { v -> edit { p -> p.copy(autoCheckUpdate = v) } },
            )
        }

        /* ─────────────────────────── 鍵盤 ─────────────────────────── */

        item {
            Section("鍵盤") {
                // §8.8.0：鍵盤總高是算出來的，可調的是鍵高。副標題因此標出
                // key_aspect 與鍵高夾制，而不是一個不存在的「鍵盤高度」欄位。
                val g = base.keyboard.geometry
                ScaleRow(
                    title = "鍵盤高度",
                    subtitle = "主題 key_aspect ${fmt(g.aspect)}，鍵高夾制 " +
                        "${fmt(g.keyHeightMin)}–${fmt(g.keyHeightMax)}dp",
                    value = prefs.keyboardHeightScale,
                    range = HEIGHT_SCALE_MIN..HEIGHT_SCALE_MAX,
                    steps = 17,
                    onChange = { edit { p -> p.copy(keyboardHeightScale = it) } },
                )

                Gap()

                BoolRow(
                    title = "按鍵音",
                    themeValue = base.feedback.sound,
                    value = prefs.soundEnabled,
                    onChange = { edit { p -> p.copy(soundEnabled = it) } },
                )
                if (prefs.soundEnabled ?: base.feedback.sound) {
                    RatioRow(
                        title = "音量",
                        themeValue = base.feedback.soundVolume,
                        value = prefs.soundVolume,
                        onChange = { edit { p -> p.copy(soundVolume = it) } },
                    )
                }

                Gap()

                BoolRow(
                    title = "震動",
                    themeValue = base.feedback.haptic,
                    value = prefs.hapticEnabled,
                    onChange = { edit { p -> p.copy(hapticEnabled = it) } },
                )
                if (prefs.hapticEnabled ?: base.feedback.haptic) {
                    ChoiceRow(
                        title = "震動強度",
                        subtitle = "主題值：${zhStrength(base.feedback.hapticStrength)}",
                        options = HapticStrength.entries.map { it to zhStrength(it) },
                        selected = prefs.hapticStrength,
                        onChange = { edit { p -> p.copy(hapticStrength = it) } },
                    )
                }

                Gap()

                MillisRow(
                    title = "長按延遲",
                    subtitle = "⚠ 主題格式無對應欄位；未設定時用 ${KeyBehavior.DEFAULT_LONG_PRESS_MS} ms",
                    value = prefs.longPressMs,
                    fallback = KeyBehavior.DEFAULT_LONG_PRESS_MS,
                    range = 150f..1000f,
                    onChange = { edit { p -> p.copy(longPressMs = it) } },
                )
                MillisRow(
                    title = "按鍵重複：起始延遲",
                    subtitle = "⚠ 主題格式無對應欄位；未設定時用 ${KeyBehavior.DEFAULT_REPEAT_DELAY_MS} ms",
                    value = prefs.repeatDelayMs,
                    fallback = KeyBehavior.DEFAULT_REPEAT_DELAY_MS,
                    range = 150f..1000f,
                    onChange = { edit { p -> p.copy(repeatDelayMs = it) } },
                )
                MillisRow(
                    title = "按鍵重複：間隔",
                    subtitle = "⚠ 主題格式無對應欄位；未設定時用 ${KeyBehavior.DEFAULT_REPEAT_INTERVAL_MS} ms",
                    value = prefs.repeatIntervalMs,
                    fallback = KeyBehavior.DEFAULT_REPEAT_INTERVAL_MS,
                    range = 20f..300f,
                    onChange = { edit { p -> p.copy(repeatIntervalMs = it) } },
                )

                Gap()

                ChoiceRow(
                    title = "鍵面提示（hint）",
                    subtitle = "拼音佈局上的數字提示。主題的 default 鍵位置：" +
                        zhHint(base.keyboard.keyStyle("default").hintPosition),
                    // 不列 HintVisibility.THEME:它與「未設定」是同一件事,
                    // 列出來會變成兩顆一模一樣的「跟隨主題」。
                    options = listOf(
                        HintVisibility.SHOWN to "顯示",
                        HintVisibility.HIDDEN to "隱藏",
                    ),
                    selected = prefs.hints,
                    onChange = { edit { p -> p.copy(hints = it) } },
                )
            }
        }

        /* ─────────────────────────── 外觀 ─────────────────────────── */

        item {
            Section("外觀") {
                ChoiceRow(
                    title = "主題",
                    subtitle = "目前生效：${base.id}（${base.name.get("zh-Hant")}）",
                    options = themeIds.map { it to it },
                    selected = prefs.themeId,
                    unsetLabel = "預設",
                    onChange = { edit { p -> p.copy(themeId = it) } },
                )
                ChoiceRow(
                    title = "深淺色",
                    subtitle = null,
                    options = listOf(
                        AppearanceMode.FOLLOW_SYSTEM to "跟隨系統",
                        AppearanceMode.LIGHT to "固定淺色",
                        AppearanceMode.DARK to "固定深色",
                    ),
                    selected = prefs.appearanceMode,
                    unsetLabel = "預設（跟隨系統）",
                    onChange = { edit { p -> p.copy(appearanceMode = it) } },
                )

                Gap()

                ScaleRow(
                    title = "候選列字級",
                    subtitle = "主題值 ${fmt(base.candidates.bar.style.text.size)}sp，" +
                        "候選列高 ${fmt(base.candidates.bar.height)}dp",
                    value = prefs.candidateSizeScale,
                    range = CANDIDATE_SIZE_SCALE_MIN..CANDIDATE_SIZE_SCALE_MAX,
                    steps = 16,
                    onChange = { edit { p -> p.copy(candidateSizeScale = it) } },
                )
            }
        }

        /* ─────────────────────────── 輸入 ─────────────────────────── */

        item {
            Section("輸入") {
                CountRow(
                    title = "候選字數量",
                    subtitle = "候選列最多顯示幾個（主題 max_visible = " +
                        (if (base.candidates.bar.maxVisible == 0) "不限"
                        else "${base.candidates.bar.maxVisible}") +
                        "）。⚠ 這只限制顯示；librime 每頁真正產生幾個由方案的 " +
                        "menu/page_size 決定，rime_shell 的 C ABI 目前沒有暴露它。",
                    value = prefs.candidateCount,
                    onChange = { edit { p -> p.copy(candidateCount = it) } },
                )

                Gap()

                TriStateRow(
                    title = "簡繁",
                    subtitle = "rs_set_option。⚠ 隨附的 luna_pinyin 系列沒有 " +
                        "simplification 開關，用的是 zh_hant/zh_hans 互斥選項組；" +
                        "兩種都會送，切回繁體時還原原本的字形。",
                    value = prefs.simplification,
                    onLabel = "簡體",
                    offLabel = "繁體",
                    onChange = { edit { p -> p.copy(simplification = it) } },
                )
                TriStateRow(
                    title = "標點",
                    subtitle = "librime 的 ascii_punct 開關（rs_set_option）",
                    value = prefs.asciiPunct,
                    onLabel = "半形",
                    offLabel = "全形",
                    onChange = { edit { p -> p.copy(asciiPunct = it) } },
                )

                Gap()

                ChoiceRow(
                    title = "空白鍵",
                    subtitle = "⚠ 主題與佈局格式都沒有描述組字中的空白鍵行為，這是實作層擴充。",
                    // 同上:SCHEMA_DEFAULT 就是「未設定」,不再列一次。
                    options = listOf(
                        SpaceBehavior.ALWAYS_SPACE to "一律送空白",
                    ),
                    selected = prefs.spaceBehavior,
                    unsetLabel = "交給方案（多為選字）",
                    onChange = { edit { p -> p.copy(spaceBehavior = it) } },
                )
            }
        }

        /* ─────────────────────────── 方案 ─────────────────────────── */

        item {
            Section("方案") {
                val schemas = remember(RimeRuntime.phase) { RimeCore.schemaList() }
                Text(
                    text = if (schemas.isEmpty()) "尚無已啟用的方案"
                    else schemas.joinToString("、") { it.name.ifEmpty { it.id } },
                    fontSize = 13.sp,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(vertical = 4.dp),
                )
                OutlinedButton(
                    onClick = { onOpenSchemaStore?.invoke() },
                    enabled = onOpenSchemaStore != null,
                    modifier = Modifier.fillMaxWidth(),
                ) {
                    Text(
                        if (onOpenSchemaStore != null) "開啟方案管理／市集"
                        else "方案管理（此入口尚未接上）"
                    )
                }
            }
        }

        /* ─────────────────────────── 診斷 ─────────────────────────── */

        item {
            Section("診斷") {
                // ⚠ 與 MainActivity 的診斷分頁內容重複，是刻意的：依協調，
                // 診斷資訊先在設定頁**複製**一份，等方案市集那一側完工後
                // 再由主控決定何時移除 MainActivity 的版本。兩邊都只讀不寫。
                Mono(
                    buildString {
                        appendLine("實作: ${if (RimeCore.isStub()) "stub 假實作" else "真 librime"}")
                        appendLine("so 載入: ${if (RimeCore.libraryLoaded) "成功" else "失敗"}")
                        appendLine(
                            "ABI 執行期/編譯期/要求: ${RimeCore.abiVersion()} / " +
                                "${RimeCore.compiledAbiVersion()} / ${RimeCore.EXPECTED_ABI_VERSION}"
                        )
                        appendLine("初始化階段: ${RimeRuntime.phase}")
                        RimeRuntime.initError?.let { appendLine("錯誤: $it") }
                        appendLine("部署狀態: ${RimeCore.lastDeployStatus}")
                        appendLine(
                            "解壓耗時: " + if (RimeRuntime.extractMillis >= 0)
                                "${RimeRuntime.extractMillis} ms" else "本次未解壓"
                        )
                        appendLine(
                            "首次部署耗時: " + if (RimeRuntime.deployMillis >= 0)
                                "${RimeRuntime.deployMillis} ms" else "進行中／未完成"
                        )
                        appendLine()
                        appendLine(RimeRuntime.describeDataDirs())
                        appendLine()
                        appendLine("生效主題: ${base.id} rev${base.revision}")
                        append("可選主題: ${themeIds.joinToString(", ")}")
                    }
                )
            }
        }

        /* ─────────────────────── 全部回復預設 ─────────────────────── */

        item {
            Section("回復預設") {
                Text(
                    text = "把所有設定刪掉，全部回到主題檔（core/themes）與方案的值。" +
                        "偏好儲存的是「使用者設過什麼」而不是預設值的副本，" +
                        "所以主題日後更新了也會跟著更新。",
                    fontSize = 13.sp,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Spacer(Modifier.height(8.dp))
                OutlinedButton(
                    onClick = { scope.launch { store.resetAll() } },
                    enabled = !prefs.isPristine,
                    modifier = Modifier.fillMaxWidth(),
                ) {
                    Text(if (prefs.isPristine) "目前全部都是預設值" else "全部回復預設")
                }
            }
        }
    }
}

/* ══════════════════════════ 版面零件 ══════════════════════════ */

@Composable
private fun Section(title: String, content: @Composable ColumnScope.() -> Unit) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(modifier = Modifier.padding(14.dp)) {
            Text(
                text = title,
                style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.SemiBold,
                modifier = Modifier.padding(bottom = 4.dp),
            )
            content()
        }
    }
}

@Composable
private fun ColumnScope.Gap() {
    Spacer(Modifier.height(10.dp))
}

@Composable
private fun ColumnScope.Mono(text: String) {
    Text(text = text, fontSize = 12.sp, fontFamily = FontFamily.Monospace)
}

@Composable
private fun ColumnScope.RowHeader(title: String, subtitle: String?, onReset: (() -> Unit)?) {
    Row(
        modifier = Modifier.fillMaxWidth().padding(top = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Text(text = title, fontWeight = FontWeight.Medium)
            if (subtitle != null) {
                Text(
                    text = subtitle,
                    fontSize = 12.sp,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
        if (onReset != null) {
            TextButton(onClick = onReset) { Text("回復預設", fontSize = 12.sp) }
        }
    }
}

/** 倍率滑桿（鍵盤高度、候選字級）。未設定時滑桿停在 100%。 */
@Composable
private fun ColumnScope.ScaleRow(
    title: String,
    subtitle: String?,
    value: Float?,
    range: ClosedFloatingPointRange<Float>,
    steps: Int,
    onChange: (Float?) -> Unit,
) {
    val shown = value ?: 1.0f
    RowHeader(
        title = "$title　${(shown * 100).roundToInt()}%" + if (value == null) "（預設）" else "",
        subtitle = subtitle,
        onReset = if (value == null) null else ({ onChange(null) }),
    )
    Slider(
        value = shown.coerceIn(range),
        onValueChange = { onChange(step05(it)) },
        valueRange = range,
        steps = steps,
    )
}

/** 0–1 比例滑桿（音量）。 */
@Composable
private fun ColumnScope.RatioRow(
    title: String,
    themeValue: Float,
    value: Float?,
    onChange: (Float?) -> Unit,
) {
    val shown = value ?: themeValue
    RowHeader(
        title = "$title　${(shown * 100).roundToInt()}%" + if (value == null) "（主題值）" else "",
        subtitle = null,
        onReset = if (value == null) null else ({ onChange(null) }),
    )
    Slider(
        value = shown.coerceIn(0f, 1f),
        onValueChange = { onChange(step05(it)) },
        valueRange = 0f..1f,
        steps = 19,
    )
}

/** 毫秒滑桿。主題格式沒有對應欄位，fallback 是實作常數而非主題值。 */
@Composable
private fun ColumnScope.MillisRow(
    title: String,
    subtitle: String,
    value: Int?,
    fallback: Int,
    range: ClosedFloatingPointRange<Float>,
    onChange: (Int?) -> Unit,
) {
    val shown = value ?: fallback
    RowHeader(
        title = "$title　$shown ms" + if (value == null) "（預設）" else "",
        subtitle = subtitle,
        onReset = if (value == null) null else ({ onChange(null) }),
    )
    Slider(
        value = shown.toFloat().coerceIn(range),
        onValueChange = { onChange((it / 10f).roundToInt() * 10) },
        valueRange = range,
    )
}

/** 候選字數量。0 = 不限（與主題 `max_visible` 的語義一致）。 */
@Composable
private fun ColumnScope.CountRow(
    title: String,
    subtitle: String,
    value: Int?,
    onChange: (Int?) -> Unit,
) {
    val shown = value ?: 0
    RowHeader(
        title = "$title　" + (if (shown == 0) "不限" else "$shown") +
            if (value == null) "（主題值）" else "",
        subtitle = subtitle,
        onReset = if (value == null) null else ({ onChange(null) }),
    )
    Slider(
        value = shown.toFloat(),
        onValueChange = { onChange(it.roundToInt()) },
        valueRange = 0f..20f,
        steps = 19,
    )
}

/** 主題有對應欄位的布林（按鍵音、震動）。未設定時顯示主題值。 */
@Composable
private fun ColumnScope.BoolRow(
    title: String,
    themeValue: Boolean,
    value: Boolean?,
    onChange: (Boolean?) -> Unit,
) {
    val shown = value ?: themeValue
    Row(
        modifier = Modifier.fillMaxWidth().padding(top = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Text(text = title, fontWeight = FontWeight.Medium)
            Text(
                text = if (value == null) "主題值：${onOff(themeValue)}"
                else "已覆寫（主題值 ${onOff(themeValue)}）",
                fontSize = 12.sp,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        if (value != null) {
            TextButton(onClick = { onChange(null) }) { Text("回復預設", fontSize = 12.sp) }
        }
        Switch(checked = shown, onCheckedChange = { onChange(it) })
    }
}

/**
 * 三態布林（librime 的 simplification / ascii_punct）。
 *
 * 為什麼不用 Switch：這兩項的「未設定」不是 false，而是「不干預，讓方案
 * 自己的預設生效」。兩態開關表達不了，也回不去不干預。
 */
@Composable
private fun ColumnScope.TriStateRow(
    title: String,
    subtitle: String,
    value: Boolean?,
    onLabel: String,
    offLabel: String,
    onChange: (Boolean?) -> Unit,
) {
    RowHeader(title = title, subtitle = subtitle, onReset = null)
    FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        FilterChip(
            selected = value == null,
            onClick = { onChange(null) },
            label = { Text("跟隨方案") },
        )
        FilterChip(
            selected = value == false,
            onClick = { onChange(false) },
            label = { Text(offLabel) },
        )
        FilterChip(
            selected = value == true,
            onClick = { onChange(true) },
            label = { Text(onLabel) },
        )
    }
}

/** 列舉／字串選擇。第一顆 chip 永遠是「未設定」。 */
@Composable
private fun <T : Any> ColumnScope.ChoiceRow(
    title: String,
    subtitle: String?,
    options: List<Pair<T, String>>,
    selected: T?,
    unsetLabel: String = "跟隨主題",
    onChange: (T?) -> Unit,
) {
    RowHeader(title = title, subtitle = subtitle, onReset = null)
    FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        FilterChip(
            selected = selected == null,
            onClick = { onChange(null) },
            label = { Text(unsetLabel) },
        )
        options.forEach { (v, label) ->
            FilterChip(
                selected = selected == v,
                onClick = { onChange(v) },
                label = { Text(label) },
            )
        }
    }
}

/* ══════════════════════════ 小工具 ══════════════════════════ */

/** 滑桿一律吸附到 0.05 的倍數，免得存進偏好的是 1.1739998 這種值。 */
private fun step05(v: Float): Float = (v * 20f).roundToInt() / 20f

private fun fmt(v: Float): String =
    if (v == v.toInt().toFloat()) v.toInt().toString()
    else String.format(Locale.US, "%.2f", v)

private fun onOff(v: Boolean): String = if (v) "開" else "關"

private fun zhStrength(s: HapticStrength): String = when (s) {
    HapticStrength.NONE -> "無"
    HapticStrength.LIGHT -> "輕"
    HapticStrength.MEDIUM -> "中"
    HapticStrength.HEAVY -> "強"
}

private fun zhHint(p: HintPosition): String = when (p) {
    HintPosition.NONE -> "不顯示"
    HintPosition.TOP_RIGHT -> "右上"
    HintPosition.TOP_CENTER -> "上中"
    HintPosition.TOP_LEFT -> "左上"
    HintPosition.BOTTOM_RIGHT -> "右下"
}
