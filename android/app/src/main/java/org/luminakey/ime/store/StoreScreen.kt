package org.luminakey.ime.store

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Snackbar
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import org.luminakey.ime.R
import org.luminakey.ime.core.DeployEstimate
import org.luminakey.ime.home.Radius
import org.luminakey.ime.home.Space
import org.luminakey.ime.home.TypeScale
import org.luminakey.ime.net.NetworkRequiredCard
import org.luminakey.ime.net.rememberNetworkEnabled

/**
 * 方案市集畫面。
 *
 * 三種狀態必須看得出來（規範外的實作要求，但理由很硬）：
 *   未安裝 / 已安裝但停用 / 已啟用
 * 因為 librime 每次部署會編譯 schema_list 上的**所有**方案，停用一個
 * 用不到的方案是實際會省下秒數的操作，不能藏起來。
 */
@Composable
fun StoreScreen(controller: StoreController, modifier: Modifier = Modifier) {

    // 連網開關。關閉時這一頁**完全不連網**：不取索引、不顯示套件清單，
    // 只留下「為什麼」與一顆開啟按鈕。匯入本機檔案不受影響 —— 那件事本來
    // 就不需要網路，把它一起擋掉只會讓離線使用者無事可做。
    val networkOn = rememberNetworkEnabled()

    LaunchedEffect(networkOn) {
        controller.refreshLocalState()
        if (networkOn && controller.index == null && !controller.loading) controller.loadIndex()
    }

    val picker = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenDocument()
    ) { uri ->
        // 檔名交給 controller 從 SAF 問（見 displayNameOf 的註解）：
        // lastPathSegment 拿到的是不透明的 document id，不是檔名。
        if (uri != null) controller.importLocal(uri)
    }

    Box(modifier = modifier.fillMaxSize()) {
        LazyColumn(
            modifier = Modifier.fillMaxSize(),
            verticalArrangement = Arrangement.spacedBy(Space.s5),
            contentPadding = androidx.compose.foundation.layout.PaddingValues(bottom = Space.s8),
        ) {
            item {
                if (networkOn) SourceCard(controller)
                else NetworkRequiredCard(stringResource(R.string.network_what_browse))
            }

            controller.indexError?.let { err ->
                item {
                    Card(
                        modifier = Modifier.fillMaxWidth(),
                        colors = CardDefaults.cardColors(
                            containerColor = MaterialTheme.colorScheme.errorContainer
                        ),
                    ) {
                        Column(Modifier.padding(Space.s5)) {
                            Text(
                                stringResource(R.string.store_index_error_title),
                                fontWeight = FontWeight.SemiBold,
                            )
                            Text(err, fontSize = TypeScale.t4, modifier = Modifier.padding(top = Space.s3))
                        }
                    }
                }
            }

            if (controller.indexWarnings.isNotEmpty()) {
                item {
                    Card(Modifier.fillMaxWidth()) {
                        Column(Modifier.padding(Space.s5)) {
                            Text(
                                pluralStringResource(
                                    R.plurals.store_index_warnings,
                                    controller.indexWarnings.size,
                                    controller.indexWarnings.size,
                                ),
                                fontWeight = FontWeight.SemiBold,
                            )
                            controller.indexWarnings.take(8).forEach {
                                Text("· $it", fontSize = TypeScale.t5)
                            }
                        }
                    }
                }
            }

            item { LocalImportCard(onPick = { picker.launch(arrayOf("*/*")) }) }

            // 開關關掉之後，上一次載到的索引就不該再畫出來 —— 上面還有
            // 「安裝」按鈕，按下去只會撞上 NetworkGate 的拒絕。
            val idx = if (networkOn) controller.index else null
            if (idx != null) {
                for (cat in idx.visibleCategories()) {
                    val pkgs = idx.packagesIn(cat.id)
                    if (pkgs.isEmpty()) continue
                    item(key = "cat-${cat.id}") {
                        Text(
                            cat.name,
                            style = MaterialTheme.typography.titleMedium,
                            modifier = Modifier.padding(top = Space.s3),
                        )
                    }
                    items(pkgs, key = { it.id }) { pkg ->
                        PackageCard(controller, pkg)
                    }
                }
            }

            val localOnly = controller.installedPackages.filter { it.source == "local" }
            if (localOnly.isNotEmpty()) {
                item {
                    Text(
                        stringResource(R.string.store_local_section),
                        style = MaterialTheme.typography.titleMedium,
                        modifier = Modifier.padding(top = Space.s3),
                    )
                }
                items(localOnly, key = { it.id }) { p -> LocalPackageCard(controller, p) }
            }
        }

    }
}

/**
 * 進度覆蓋層與對話框。
 *
 * 刻意與 [StoreScreen] 分開，由 Activity 畫在**分頁之外**：部署進度不是
 * 市集分頁專屬的東西 —— 診斷分頁的「重新部署」按的是同一個 `rs_deploy()`，
 * 該有的進度、成功／失敗提示一模一樣。寫兩份只會有一份被維護。
 */
@Composable
fun StoreOverlays(controller: StoreController) {
    controller.job?.let { JobOverlay(it) }
    controller.confirm?.let { plan -> ConfirmDialog(controller, plan) }
    // 失敗，**以及「成功但有話要說」**，都走這裡（見 [finishUi] 的註解：
    // 預檢的 warning 就是成功但有話要說）。這一行的註解上一版寫的是
    // 「只剩失敗才會走到這裡」，而同一版的重點正好是讓帶警告的成功也走 result
    // —— 程式是對的，註解變成反話。這個專案靠註解傳遞為什麼，留著會誤導。
    controller.result?.let { r -> ResultDialog(controller, r) }
    controller.toast?.let { t -> ToastBar(t) { controller.dismissToast() } }
}

/* ─────────────────────────── 來源設定 ─────────────────────────── */

@Composable
private fun SourceCard(controller: StoreController) {
    var editing by remember { mutableStateOf(false) }
    var draft by remember(controller.indexUrl) { mutableStateOf(controller.indexUrl) }

    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(Space.s5)) {
            Text(stringResource(R.string.store_source_title), fontWeight = FontWeight.SemiBold)
            if (editing) {
                OutlinedTextField(
                    value = draft,
                    onValueChange = { draft = it },
                    label = { Text(stringResource(R.string.store_source_field_label)) },
                    singleLine = true,
                    modifier = Modifier.fillMaxWidth().padding(top = Space.s3),
                )
                Row(horizontalArrangement = Arrangement.spacedBy(Space.s3)) {
                    TextButton(onClick = {
                        controller.applyIndexUrl(draft)
                        editing = false
                        controller.loadIndex()
                    }) { Text(stringResource(R.string.store_source_apply)) }
                    TextButton(onClick = {
                        controller.resetIndexUrl()
                        draft = controller.indexUrl
                        editing = false
                    }) { Text(stringResource(R.string.reset_to_default)) }
                }
            } else {
                Text(
                    controller.indexUrl,
                    fontSize = TypeScale.t5,
                    fontFamily = FontFamily.Monospace,
                    modifier = Modifier.padding(top = Space.s1),
                )
                Row(horizontalArrangement = Arrangement.spacedBy(Space.s3)) {
                    TextButton(onClick = { controller.loadIndex() }, enabled = !controller.loading) {
                        Text(
                            stringResource(
                                if (controller.loading) R.string.store_source_loading
                                else R.string.store_source_reload
                            )
                        )
                    }
                    TextButton(onClick = { editing = true }) {
                        Text(stringResource(R.string.store_source_change))
                    }
                }
            }
        }
    }
}

@Composable
private fun LocalImportCard(onPick: () -> Unit) {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(Space.s5)) {
            Text(stringResource(R.string.store_import_title), fontWeight = FontWeight.SemiBold)
            Text(
                stringResource(R.string.store_import_body),
                fontSize = TypeScale.t5,
                modifier = Modifier.padding(vertical = Space.s2),
            )
            OutlinedButton(onClick = onPick, modifier = Modifier.fillMaxWidth()) {
                Text(stringResource(R.string.store_import_pick))
            }
        }
    }
}

/* ─────────────────────────── 套件卡片 ─────────────────────────── */

@Composable
private fun PackageCard(controller: StoreController, pkg: StorePackage) {
    val state = controller.stateOf(pkg)
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(Space.s5)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(pkg.name, fontWeight = FontWeight.SemiBold, modifier = Modifier.weight(1f))
                if (pkg.recommended) {
                    Badge(
                        stringResource(R.string.store_badge_recommended),
                        MaterialTheme.colorScheme.tertiaryContainer,
                    )
                }
                Box(Modifier.width(Space.s2))
                StateBadge(state)
            }
            if (pkg.description.isNotEmpty()) {
                Text(pkg.description, fontSize = TypeScale.t4, modifier = Modifier.padding(top = Space.s1))
            }
            val sep = stringResource(R.string.store_meta_separator)
            val listSep = stringResource(R.string.store_meta_list_separator)
            Text(
                buildString {
                    append(formatBytes(pkg.size))
                    pkg.license?.let { append(sep); append(it) }
                    if (pkg.schemas.isNotEmpty()) {
                        append(sep)
                        append(
                            stringResource(
                                R.string.store_meta_schemas,
                                pkg.schemas.joinToString(listSep) { it.name },
                            )
                        )
                    }
                    if (pkg.requires.isNotEmpty()) {
                        append(sep)
                        append(
                            stringResource(
                                R.string.store_meta_requires,
                                pkg.requires.joinToString(listSep),
                            )
                        )
                    }
                },
                fontSize = TypeScale.t5,
                modifier = Modifier.padding(top = Space.s1),
            )
            if (!pkg.verifiedDeployed) {
                Text(
                    stringResource(R.string.store_unverified),
                    fontSize = TypeScale.t5,
                    color = MaterialTheme.colorScheme.error,
                    modifier = Modifier.padding(top = Space.s1),
                )
            }
            pkg.layoutNote?.let {
                Text(
                    stringResource(R.string.store_layout_note, it),
                    fontSize = TypeScale.t5,
                    modifier = Modifier.padding(top = Space.s1),
                )
            }

            Row(
                horizontalArrangement = Arrangement.spacedBy(Space.s3),
                modifier = Modifier.padding(top = Space.s3),
            ) {
                when (state) {
                    StoreController.PackageState.NOT_INSTALLED ->
                        Button(
                            onClick = { controller.prepareInstall(pkg) },
                            enabled = !controller.busy,
                        ) { Text(stringResource(R.string.store_action_install)) }

                    StoreController.PackageState.INSTALLED_DISABLED -> {
                        if (pkg.schemaIds.isNotEmpty()) {
                            Button(
                                onClick = { controller.setEnabled(pkg.schemaIds, true) },
                                enabled = !controller.busy,
                            ) { Text(stringResource(R.string.store_action_enable)) }
                        }
                        OutlinedButton(
                            onClick = { controller.uninstall(pkg.id) },
                            enabled = !controller.busy,
                        ) { Text(stringResource(R.string.store_action_uninstall)) }
                    }

                    StoreController.PackageState.ENABLED -> {
                        OutlinedButton(
                            onClick = { controller.setEnabled(pkg.schemaIds, false) },
                            enabled = !controller.busy,
                        ) { Text(stringResource(R.string.store_action_disable)) }
                        OutlinedButton(
                            onClick = { controller.uninstall(pkg.id) },
                            enabled = !controller.busy,
                        ) { Text(stringResource(R.string.store_action_uninstall)) }
                    }
                }
            }
        }
    }
}

@Composable
private fun LocalPackageCard(controller: StoreController, pkg: InstalledPackage) {
    val enabled = pkg.schemaIds.any { it in controller.enabledSchemas }
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(Space.s5)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(pkg.name, fontWeight = FontWeight.SemiBold, modifier = Modifier.weight(1f))
                StateBadge(
                    if (enabled) StoreController.PackageState.ENABLED
                    else StoreController.PackageState.INSTALLED_DISABLED
                )
            }
            Text(
                pluralStringResource(R.plurals.store_local_files, pkg.files.size, pkg.files.size) +
                    if (pkg.schemaIds.isEmpty()) stringResource(R.string.store_local_no_schema)
                    else stringResource(R.string.store_meta_separator) + stringResource(
                        R.string.store_meta_schemas,
                        pkg.schemaIds.joinToString(
                            stringResource(R.string.store_meta_list_separator)
                        ),
                    ),
                fontSize = TypeScale.t5,
            )
            Row(
                horizontalArrangement = Arrangement.spacedBy(Space.s3),
                modifier = Modifier.padding(top = Space.s3),
            ) {
                if (pkg.schemaIds.isNotEmpty()) {
                    if (enabled) {
                        OutlinedButton(
                            onClick = { controller.setEnabled(pkg.schemaIds, false) },
                            enabled = !controller.busy,
                        ) { Text(stringResource(R.string.store_action_disable)) }
                    } else {
                        Button(
                            onClick = { controller.setEnabled(pkg.schemaIds, true) },
                            enabled = !controller.busy,
                        ) { Text(stringResource(R.string.store_action_enable)) }
                    }
                }
                OutlinedButton(
                    onClick = { controller.uninstall(pkg.id) },
                    enabled = !controller.busy,
                ) { Text(stringResource(R.string.store_action_uninstall)) }
            }
        }
    }
}

@Composable
private fun StateBadge(state: StoreController.PackageState) = when (state) {
    StoreController.PackageState.NOT_INSTALLED ->
        Badge(
            stringResource(R.string.store_badge_not_installed),
            MaterialTheme.colorScheme.surfaceVariant,
        )

    StoreController.PackageState.INSTALLED_DISABLED ->
        Badge(
            stringResource(R.string.store_badge_installed_off),
            MaterialTheme.colorScheme.secondaryContainer,
        )

    StoreController.PackageState.ENABLED ->
        Badge(
            stringResource(R.string.store_badge_on),
            MaterialTheme.colorScheme.primaryContainer,
        )
}

@Composable
private fun Badge(text: String, bg: Color) {
    Box(
        modifier = Modifier
            .background(bg, RoundedCornerShape(Radius.small))
            .padding(horizontal = Space.s3, vertical = Space.s1)
    ) { Text(text, fontSize = TypeScale.t5) }
}

/* ─────────────────────────── 對話框與進度 ─────────────────────────── */

@Composable
private fun ConfirmDialog(controller: StoreController, plan: StoreController.ConfirmPlan) {
    AlertDialog(
        onDismissRequest = { controller.dismissConfirm() },
        title = {
            Text(
                pluralStringResource(
                    R.plurals.store_confirm_title,
                    plan.plan.count,
                    plan.plan.count,
                    formatBytes(plan.plan.totalBytes),
                )
            )
        },
        text = {
            Column {
                plan.plan.toDownload.forEach {
                    Text(
                        stringResource(
                            R.string.store_confirm_line, it.name, formatBytes(it.size)
                        ),
                        fontSize = TypeScale.t4,
                    )
                }
                // 索引的 size 是 **zip** 大小。實測 luna-pinyin：zip 0.4MB →
                // 解壓 0.9MB → librime 部署產生的 build 產物 13MB。只講下載量
                // 會讓使用者以為只花幾百 KB。這裡把兩個數字分開講，
                // 並明講後者是概估（索引還沒有對應欄位）。
                Text(
                    stringResource(
                        R.string.store_confirm_installed_size,
                        formatBytes(plan.plan.estimatedInstalledBytes),
                    ),
                    fontSize = TypeScale.t5,
                    modifier = Modifier.padding(top = Space.s3),
                )
                if (plan.plan.cycles.isNotEmpty()) {
                    Text(
                        stringResource(
                            R.string.store_confirm_cycles,
                            plan.plan.cycles.joinToString("；") { it.joinToString(" ↔ ") },
                        ),
                        fontSize = TypeScale.t5,
                        modifier = Modifier.padding(top = Space.s3),
                    )
                }
                if (plan.plan.alreadyInstalled.isNotEmpty()) {
                    Text(
                        pluralStringResource(
                            R.plurals.store_confirm_already,
                            plan.plan.alreadyInstalled.size,
                            plan.plan.alreadyInstalled.size,
                            plan.plan.alreadyInstalled.joinToString(
                                stringResource(R.string.store_meta_list_separator)
                            ),
                        ),
                        fontSize = TypeScale.t5,
                        modifier = Modifier.padding(top = Space.s3),
                    )
                }
                Text(
                    stringResource(
                        R.string.store_confirm_footnote,
                        DeployEstimate.TYPICAL_SECONDS,
                    ),
                    fontSize = TypeScale.t5,
                    modifier = Modifier.padding(top = Space.s3),
                )
            }
        },
        confirmButton = {
            TextButton(onClick = { controller.confirmInstall(alsoEnable = true) }) {
                Text(stringResource(R.string.store_confirm_install_enable))
            }
        },
        dismissButton = {
            Row {
                TextButton(onClick = { controller.confirmInstall(alsoEnable = false) }) {
                    Text(stringResource(R.string.store_confirm_download_only))
                }
                TextButton(onClick = { controller.dismissConfirm() }) {
                    Text(stringResource(R.string.cancel))
                }
            }
        },
    )
}

/**
 * 成功時的短暫提示。
 *
 * 真機回報：「診斷的重新部署。部署完就要退出界面對不。」—— 對。使用者按
 * 那顆按鈕是為了完成部署，成功之後再攔他一次要他按「知道了」，只是在他
 * 已經達成目的之後多收一次過路費。所以成功走這條：訊息照講，但不擋路，
 * 幾秒後自己消失，也可以點掉。
 *
 * 失敗**不**走這裡，仍然停在 [ResultDialog] 上 —— 那些訊息裡有
 * `rs_last_error()`、有回滾結果、有需要使用者採取行動的指示。
 *
 * 外層 Box 沒有掛任何 pointer 修飾子，所以不會攔截底下畫面的觸控。
 */
@Composable
private fun ToastBar(text: String, onDismiss: () -> Unit) {
    Box(
        modifier = Modifier.fillMaxSize().padding(Space.s4),
        contentAlignment = Alignment.BottomCenter,
    ) {
        Snackbar(
            action = {
                TextButton(onClick = onDismiss) { Text(stringResource(R.string.close)) }
            },
        ) { Text(text, fontSize = TypeScale.t4) }
    }
}

@Composable
private fun ResultDialog(controller: StoreController, r: StoreController.ResultUi) {
    AlertDialog(
        onDismissRequest = { controller.dismissResult() },
        title = {
            Text(
                stringResource(
                    if (r.ok) R.string.store_result_ok else R.string.store_result_failed
                )
            )
        },
        text = {
            Column {
                Text(r.message, fontSize = TypeScale.t4)
                r.details.forEach {
                    Text("· $it", fontSize = TypeScale.t5, modifier = Modifier.padding(top = Space.s1))
                }
            }
        },
        confirmButton = {
            TextButton(onClick = { controller.dismissResult() }) {
                Text(stringResource(R.string.got_it))
            }
        },
    )
}

/**
 * 進度覆蓋層。
 *
 * 規範 §3 要求部署期間「顯示進度而非停在空白或假死狀態」。librime 不提供
 * 百分比，所以部署階段用不確定進度條 + **已耗時秒數**，並直說預期量級
 * （那個量級只有一個來源，見 [org.luminakey.ime.core.DeployEstimate]）；
 * 下載階段有 content-length 才用確定進度條。寧可誠實地說「不知道還要多久，
 * 但已經跑了這麼久」，也不要畫一條假的百分比。
 */
@Composable
private fun JobOverlay(job: StoreController.JobUi) {
    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(Color(0x99000000)),
        contentAlignment = Alignment.Center,
    ) {
        Card(modifier = Modifier.fillMaxWidth().padding(Space.s7)) {
            Column(Modifier.padding(Space.s5)) {
                Text(job.title, fontWeight = FontWeight.SemiBold)
                Text(job.detail, fontSize = TypeScale.t4, modifier = Modifier.padding(top = Space.s3))
                if (job.fraction >= 0f) {
                    LinearProgressIndicator(
                        progress = { job.fraction },
                        modifier = Modifier.fillMaxWidth().padding(top = Space.s4),
                    )
                } else {
                    LinearProgressIndicator(
                        modifier = Modifier.fillMaxWidth().padding(top = Space.s4),
                    )
                }
            }
        }
    }
}
