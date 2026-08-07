@file:OptIn(ExperimentalLayoutApi::class)

package org.rimequad.ime.prefs

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.rimequad.ime.core.RimeRuntime
import org.rimequad.ime.keyboard.ConfigRepository
import org.rimequad.ime.keyboard.LayoutRemap
import org.rimequad.ime.keyboard.LayoutRemapValidator
import org.rimequad.ime.keyboard.RemapOp
import org.rimequad.ime.keyboard.RemappingLayoutRepository
import org.rimequad.ime.keyboard.UserLayoutStore
import org.rimequad.ime.theme.KeyboardLayout
import org.rimequad.ime.theme.LayoutKey

/**
 * 「自訂鍵位」的最小可用入口。
 *
 * ── 這個檔案的定位 ──────────────────────────────────────────────────────
 * **刻意做成一個獨立、自給自足的 composable。** 設定頁與主畫面的整體重新設計
 * 正在另一條線上進行，這塊畫面之後會被搬到別的地方去。所以：
 *
 *  · 全部邏輯都在 `org.rimequad.ime.keyboard`（純函式 + 儲存層），
 *    本檔只負責「選兩顆鍵、按一下、看結果」，一行商業邏輯都沒有。
 *  · 不改 [SettingsScreen] 的結構，只在它的 LazyColumn 裡多一個 `item`。
 *  · 版面零件（Section 卡片）在本檔內自帶一份，不依賴 SettingsScreen 的
 *    private composable —— 搬家時整個檔案剪下貼上即可，不會拖出一串相依。
 *
 * ── 為什麼驗證要放到背景執行緒 ──────────────────────────────────────────
 * [LayoutRemapValidator] 會跑逃生不變式的圖走訪（[org.rimequad.ime.keyboard.LayoutEscape]），
 * 那是指數級的重放。雖然有 CachingLayoutRepository 壓著，仍然不該壓在主執行緒
 * 上 —— 一次幾百毫秒的卡頓在設定頁裡就是「按了沒反應」。
 */
@Composable
fun KeyRemapSection(modifier: Modifier = Modifier) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()

    val repo = remember { ConfigRepository(context) }
    val store = remember { UserLayoutStore.get(RimeRuntime.userDataDirOf(context)) }

    // ⚠ 用 rememberSaveable 而不是 remember：本區塊是 SettingsScreen 那個
    // LazyColumn 裡的一個 item，捲出畫面時 item 會被**銷毀**，普通的 remember
    // 會連同使用者已經選好的兩顆鍵一起消失。實測就是這樣：選好 a 與 s、捲下去
    // 找「交換」按鈕，按鈕卻是灰的 —— 而使用者完全看不出自己的選擇何時被吃掉。
    // LazyColumn 會替每個 item 保存 saveable 狀態，所以這一行就是修法。
    // 每次寫入就 +1，用來強迫下面所有 remember(...) 重算。
    var revision by rememberSaveable { mutableStateOf(0) }
    // busy 刻意**不**保存：行程若在驗證途中被收掉，存回一個 true 會讓按鈕
    // 永遠是灰的，而使用者不知道要怎麼救。重來一次即可。
    var busy by remember { mutableStateOf(false) }
    var message by rememberSaveable { mutableStateOf<String?>(null) }
    var failed by rememberSaveable { mutableStateOf(false) }

    val layoutIds = remember { repo.layoutIds().sorted() }
    var layoutId by rememberSaveable { mutableStateOf(layoutIds.firstOrNull { it == "qwerty" } ?: layoutIds.firstOrNull().orEmpty()) }

    // 顯示的是**套用自訂之後**的佈局：選單裡的先後順序必須跟鍵盤上看到的一樣，
    // 否則使用者換過一次之後就再也對不上哪顆是哪顆。
    val layout: KeyboardLayout? = remember(layoutId, revision) {
        if (layoutId.isEmpty()) {
            null
        } else {
            RemappingLayoutRepository(repo) { id -> store.remapFor(id) }
                .loadLayout(layoutId).value
        }
    }
    val layerIds = layout?.layers?.map { it.id } ?: emptyList()
    var layerId by rememberSaveable(layoutId, revision) { mutableStateOf(layout?.defaultLayer.orEmpty()) }

    val keys = remember(layout, layerId) { addressableKeys(layout, layerId) }
    var first by rememberSaveable(layoutId, layerId, revision) { mutableStateOf<String?>(null) }
    var second by rememberSaveable(layoutId, layerId, revision) { mutableStateOf<String?>(null) }

    val current = remember(layoutId, revision) { store.remapFor(layoutId) }

    fun finish(problems: List<String>, okText: String) {
        busy = false
        if (problems.isEmpty()) {
            failed = false
            message = okText
            revision++
            first = null
            second = null
        } else {
            failed = true
            message = problems.joinToString("\n")
        }
    }

    Section("自訂鍵位（引擎層）", modifier) {
        Text(
            text = "選同一層裡的兩顆鍵交換位置。交換的只有「位置」—— 標著 a 的鍵搬到 " +
                "s 的格子之後，按下去送出的仍然是 a。改變一顆鍵「送出什麼」會直接打壞方案" +
                "（九宮格那八顆鍵送的 A/D/G/J/M/P/T/W 是 t9_pinyin 的 speller 契約），" +
                "所以引擎層不提供那件事。",
            fontSize = 12.sp,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )

        Label("佈局")
        FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            layoutIds.forEach { id ->
                FilterChip(
                    selected = id == layoutId,
                    onClick = { layoutId = id; message = null },
                    label = { Text(id, fontSize = 12.sp) },
                )
            }
        }

        if (layout == null) {
            Text(
                text = "佈局「$layoutId」載不起來，沒有可以自訂的基礎。",
                fontSize = 12.sp,
                color = MaterialTheme.colorScheme.error,
            )
            return@Section
        }

        Label("層")
        FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            layerIds.forEach { id ->
                FilterChip(
                    selected = id == layerId,
                    onClick = { layerId = id; first = null; second = null; message = null },
                    label = { Text(id, fontSize = 12.sp) },
                )
            }
        }

        Label("第一顆鍵")
        KeyChips(keys, first) { first = it; message = null }

        Label("第二顆鍵")
        KeyChips(keys, second) { second = it; message = null }

        Spacer(Modifier.height(8.dp))
        Button(
            onClick = {
                val a = first ?: return@Button
                val b = second ?: return@Button
                busy = true
                message = null
                scope.launch {
                    val next = (current ?: LayoutRemap.empty(layoutId))
                        .plus(RemapOp.Swap(layerId, a, b))
                    val problems = withContext(Dispatchers.Default) {
                        LayoutRemapValidator.validate(repo, next)
                    }
                    if (problems.isEmpty()) store.put(next)
                    finish(problems, "已交換「$a」與「$b」。回到輸入框就會看到新鍵盤。")
                }
            },
            enabled = !busy && first != null && second != null && first != second,
            modifier = Modifier.fillMaxWidth(),
        ) {
            Text(if (busy) "驗證中…" else "交換這兩顆鍵")
        }

        message?.let {
            Spacer(Modifier.height(6.dp))
            Text(
                text = it,
                fontSize = 12.sp,
                color = if (failed) MaterialTheme.colorScheme.error
                else MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }

        Spacer(Modifier.height(10.dp))
        Text(
            text = if (current == null) "「$layoutId」目前沒有自訂。"
            else "「$layoutId」目前的自訂：\n" + current.ops.joinToString("\n") { "· " + it.describe() },
            fontSize = 12.sp,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )

        Spacer(Modifier.height(6.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedButton(
                onClick = {
                    store.reset(layoutId)
                    failed = false
                    // 重置回到的是**當前**基礎佈局的原樣，不是某個快照 ——
                    // 這正是儲存層存「操作」而不是存「改完的佈局」的兌現處。
                    message = "「$layoutId」已回到基礎佈局的原樣。"
                    revision++
                },
                enabled = current != null,
                modifier = Modifier.weight(1f),
            ) { Text("重置這份佈局", fontSize = 12.sp) }

            OutlinedButton(
                onClick = {
                    store.resetAll()
                    failed = false
                    message = "全部自訂鍵位已清除。"
                    revision++
                },
                enabled = store.hasAny(),
                modifier = Modifier.weight(1f),
            ) { Text("全部重置", fontSize = 12.sp) }
        }
    }
}

/* ══════════════════════════ 版面零件（本檔自帶） ══════════════════════════ */

@Composable
private fun KeyChips(keys: List<KeyChoice>, selected: String?, onPick: (String) -> Unit) {
    FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
        keys.forEach { k ->
            FilterChip(
                selected = k.id == selected,
                onClick = { onPick(k.id) },
                label = { Text(k.face, fontSize = 12.sp) },
            )
        }
    }
}

@Composable
private fun Label(text: String) {
    Text(
        text = text,
        fontWeight = FontWeight.Medium,
        fontSize = 13.sp,
        modifier = Modifier.padding(top = 10.dp, bottom = 2.dp),
    )
}

@Composable
private fun Section(
    title: String,
    modifier: Modifier = Modifier,
    content: @Composable ColumnScope.() -> Unit,
) {
    Card(modifier = modifier.fillMaxWidth()) {
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

/* ══════════════════════════ 選單資料 ══════════════════════════ */

/** 選單上的一顆鍵。[face] 是「id（鍵面）」，兩者都要有：鍵面會重複，id 不會。 */
internal data class KeyChoice(val id: String, val face: String)

/**
 * 這一層裡「可以被自訂定位」的鍵：有 id、不是 spacer、且 id 在本層唯一。
 *
 * 排除沒有 id 的鍵不是偷懶 —— 沒有 id 就沒有穩定的定位方式，只能用索引，
 * 而索引正是上游改版時會靜靜指錯的東西（見 KeyRemap 的檔頭）。
 * 排除重複 id 同理：那時「用 id 定位」這個前提本身不成立。
 */
internal fun addressableKeys(layout: KeyboardLayout?, layerId: String): List<KeyChoice> {
    val layer = layout?.layer(layerId) ?: return emptyList()
    val all = layer.rows.flatMap { it.keys }
    val counts = HashMap<String, Int>()
    for (k in all) k.id?.let { counts[it] = (counts[it] ?: 0) + 1 }
    return all.mapNotNull { k ->
        val id = k.id ?: return@mapNotNull null
        if (k.spacer || counts[id] != 1) return@mapNotNull null
        KeyChoice(id, "$id（${faceOf(k)}）")
    }
}

private fun faceOf(k: LayoutKey): String = when {
    k.label.isNotEmpty() -> k.label
    k.icon != null -> k.icon
    else -> "—"
}
