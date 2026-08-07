package org.rimequad.ime.store

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
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

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

    LaunchedEffect(Unit) {
        controller.refreshLocalState()
        if (controller.index == null && !controller.loading) controller.loadIndex()
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
            verticalArrangement = Arrangement.spacedBy(10.dp),
            contentPadding = androidx.compose.foundation.layout.PaddingValues(bottom = 32.dp),
        ) {
            item { SourceCard(controller) }

            controller.indexError?.let { err ->
                item {
                    Card(
                        modifier = Modifier.fillMaxWidth(),
                        colors = CardDefaults.cardColors(
                            containerColor = MaterialTheme.colorScheme.errorContainer
                        ),
                    ) {
                        Column(Modifier.padding(14.dp)) {
                            Text("索引載入失敗", fontWeight = FontWeight.SemiBold)
                            Text(err, fontSize = 13.sp, modifier = Modifier.padding(top = 6.dp))
                        }
                    }
                }
            }

            if (controller.indexWarnings.isNotEmpty()) {
                item {
                    Card(Modifier.fillMaxWidth()) {
                        Column(Modifier.padding(14.dp)) {
                            Text("索引警告（${controller.indexWarnings.size}）", fontWeight = FontWeight.SemiBold)
                            controller.indexWarnings.take(8).forEach {
                                Text("· $it", fontSize = 12.sp)
                            }
                        }
                    }
                }
            }

            item { LocalImportCard(onPick = { picker.launch(arrayOf("*/*")) }) }

            val idx = controller.index
            if (idx != null) {
                for (cat in idx.visibleCategories()) {
                    val pkgs = idx.packagesIn(cat.id)
                    if (pkgs.isEmpty()) continue
                    item(key = "cat-${cat.id}") {
                        Text(
                            cat.name,
                            style = MaterialTheme.typography.titleMedium,
                            modifier = Modifier.padding(top = 8.dp),
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
                        "自行匯入",
                        style = MaterialTheme.typography.titleMedium,
                        modifier = Modifier.padding(top = 8.dp),
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
    // 只剩失敗才會走到這裡（見 StoreController.ResultUi 的註解）。
    controller.result?.let { r -> ResultDialog(controller, r) }
    controller.toast?.let { t -> ToastBar(t) { controller.dismissToast() } }
}

/* ─────────────────────────── 來源設定 ─────────────────────────── */

@Composable
private fun SourceCard(controller: StoreController) {
    var editing by remember { mutableStateOf(false) }
    var draft by remember(controller.indexUrl) { mutableStateOf(controller.indexUrl) }

    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(14.dp)) {
            Text("索引來源", fontWeight = FontWeight.SemiBold)
            if (editing) {
                OutlinedTextField(
                    value = draft,
                    onValueChange = { draft = it },
                    label = { Text("index.json 的網址") },
                    singleLine = true,
                    modifier = Modifier.fillMaxWidth().padding(top = 6.dp),
                )
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    TextButton(onClick = {
                        controller.applyIndexUrl(draft)
                        editing = false
                        controller.loadIndex()
                    }) { Text("套用並重新載入") }
                    TextButton(onClick = {
                        controller.resetIndexUrl()
                        draft = controller.indexUrl
                        editing = false
                    }) { Text("回復預設") }
                }
            } else {
                Text(
                    controller.indexUrl,
                    fontSize = 12.sp,
                    fontFamily = FontFamily.Monospace,
                    modifier = Modifier.padding(top = 4.dp),
                )
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    TextButton(onClick = { controller.loadIndex() }, enabled = !controller.loading) {
                        Text(if (controller.loading) "載入中…" else "重新整理")
                    }
                    TextButton(onClick = { editing = true }) { Text("更改來源") }
                }
            }
        }
    }
}

@Composable
private fun LocalImportCard(onPick: () -> Unit) {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(14.dp)) {
            Text("匯入自己的方案", fontWeight = FontWeight.SemiBold)
            Text(
                "接受 .zip 與單一 .yaml。安全檢查與市集完全相同：路徑穿越、符號連結、" +
                    "解壓炸彈、副檔名白名單，任一不過就整包拒絕。",
                fontSize = 12.sp,
                modifier = Modifier.padding(vertical = 4.dp),
            )
            OutlinedButton(onClick = onPick, modifier = Modifier.fillMaxWidth()) {
                Text("選擇檔案…")
            }
        }
    }
}

/* ─────────────────────────── 套件卡片 ─────────────────────────── */

@Composable
private fun PackageCard(controller: StoreController, pkg: StorePackage) {
    val state = controller.stateOf(pkg)
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(14.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(pkg.name, fontWeight = FontWeight.SemiBold, modifier = Modifier.weight(1f))
                if (pkg.recommended) Badge("推薦", MaterialTheme.colorScheme.tertiaryContainer)
                Box(Modifier.width(6.dp))
                StateBadge(state)
            }
            if (pkg.description.isNotEmpty()) {
                Text(pkg.description, fontSize = 13.sp, modifier = Modifier.padding(top = 4.dp))
            }
            Text(
                buildString {
                    append(formatBytes(pkg.size))
                    pkg.license?.let { append("　·　$it") }
                    if (pkg.schemas.isNotEmpty()) {
                        append("　·　方案：")
                        append(pkg.schemas.joinToString("、") { it.name })
                    }
                    if (pkg.requires.isNotEmpty()) append("　·　相依：${pkg.requires.joinToString("、")}")
                },
                fontSize = 11.sp,
                modifier = Modifier.padding(top = 4.dp),
            )
            if (!pkg.verifiedDeployed) {
                Text(
                    "⚠ 索引未標示 verified.deployed —— 這個套件沒有經過打包端的部署驗證，" +
                        "導入後有較高機率部署失敗（失敗會自動回滾）。",
                    fontSize = 11.sp,
                    color = MaterialTheme.colorScheme.error,
                    modifier = Modifier.padding(top = 4.dp),
                )
            }
            pkg.layoutNote?.let {
                Text("鍵盤：$it", fontSize = 11.sp, modifier = Modifier.padding(top = 4.dp))
            }

            Row(
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                modifier = Modifier.padding(top = 8.dp),
            ) {
                when (state) {
                    StoreController.PackageState.NOT_INSTALLED ->
                        Button(
                            onClick = { controller.prepareInstall(pkg) },
                            enabled = !controller.busy,
                        ) { Text("安裝") }

                    StoreController.PackageState.INSTALLED_DISABLED -> {
                        if (pkg.schemaIds.isNotEmpty()) {
                            Button(
                                onClick = { controller.setEnabled(pkg.schemaIds, true) },
                                enabled = !controller.busy,
                            ) { Text("啟用") }
                        }
                        OutlinedButton(
                            onClick = { controller.uninstall(pkg.id) },
                            enabled = !controller.busy,
                        ) { Text("解除安裝") }
                    }

                    StoreController.PackageState.ENABLED -> {
                        OutlinedButton(
                            onClick = { controller.setEnabled(pkg.schemaIds, false) },
                            enabled = !controller.busy,
                        ) { Text("停用") }
                        OutlinedButton(
                            onClick = { controller.uninstall(pkg.id) },
                            enabled = !controller.busy,
                        ) { Text("解除安裝") }
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
        Column(Modifier.padding(14.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(pkg.name, fontWeight = FontWeight.SemiBold, modifier = Modifier.weight(1f))
                StateBadge(
                    if (enabled) StoreController.PackageState.ENABLED
                    else StoreController.PackageState.INSTALLED_DISABLED
                )
            }
            Text(
                "${pkg.files.size} 個檔案" +
                    if (pkg.schemaIds.isEmpty()) "（沒有 *.schema.yaml）"
                    else "　·　方案：${pkg.schemaIds.joinToString("、")}",
                fontSize = 11.sp,
            )
            Row(
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                modifier = Modifier.padding(top = 8.dp),
            ) {
                if (pkg.schemaIds.isNotEmpty()) {
                    if (enabled) {
                        OutlinedButton(
                            onClick = { controller.setEnabled(pkg.schemaIds, false) },
                            enabled = !controller.busy,
                        ) { Text("停用") }
                    } else {
                        Button(
                            onClick = { controller.setEnabled(pkg.schemaIds, true) },
                            enabled = !controller.busy,
                        ) { Text("啟用") }
                    }
                }
                OutlinedButton(
                    onClick = { controller.uninstall(pkg.id) },
                    enabled = !controller.busy,
                ) { Text("解除安裝") }
            }
        }
    }
}

@Composable
private fun StateBadge(state: StoreController.PackageState) = when (state) {
    StoreController.PackageState.NOT_INSTALLED ->
        Badge("未安裝", MaterialTheme.colorScheme.surfaceVariant)

    StoreController.PackageState.INSTALLED_DISABLED ->
        Badge("已安裝·停用", MaterialTheme.colorScheme.secondaryContainer)

    StoreController.PackageState.ENABLED ->
        Badge("已啟用", MaterialTheme.colorScheme.primaryContainer)
}

@Composable
private fun Badge(text: String, bg: Color) {
    Box(
        modifier = Modifier
            .background(bg, RoundedCornerShape(6.dp))
            .padding(horizontal = 8.dp, vertical = 2.dp)
    ) { Text(text, fontSize = 11.sp) }
}

/* ─────────────────────────── 對話框與進度 ─────────────────────────── */

@Composable
private fun ConfirmDialog(controller: StoreController, plan: StoreController.ConfirmPlan) {
    AlertDialog(
        onDismissRequest = { controller.dismissConfirm() },
        title = { Text("將下載 ${plan.plan.count} 個套件，共 ${formatBytes(plan.plan.totalBytes)}") },
        text = {
            Column {
                plan.plan.toDownload.forEach {
                    Text("· ${it.name}　${formatBytes(it.size)}", fontSize = 13.sp)
                }
                // 索引的 size 是 **zip** 大小。實測 luna-pinyin：zip 0.4MB →
                // 解壓 0.9MB → librime 部署產生的 build 產物 13MB。只講下載量
                // 會讓使用者以為只花幾百 KB。這裡把兩個數字分開講，
                // 並明講後者是概估（索引還沒有對應欄位）。
                Text(
                    "安裝後預計佔用約 ${formatBytes(plan.plan.estimatedInstalledBytes)}（概估）—— " +
                        "librime 部署會把詞庫編成索引檔，體積遠大於下載的壓縮檔。",
                    fontSize = 12.sp,
                    modifier = Modifier.padding(top = 8.dp),
                )
                if (plan.plan.cycles.isNotEmpty()) {
                    Text(
                        "註：" + plan.plan.cycles.joinToString("；") { it.joinToString(" ↔ ") } +
                            " 互為相依（互相提供反查詞庫），本來就要一起安裝。",
                        fontSize = 12.sp,
                        modifier = Modifier.padding(top = 6.dp),
                    )
                }
                if (plan.plan.alreadyInstalled.isNotEmpty()) {
                    Text(
                        "另有 ${plan.plan.alreadyInstalled.size} 個相依已安裝，不重複下載：" +
                            plan.plan.alreadyInstalled.joinToString("、"),
                        fontSize = 12.sp,
                        modifier = Modifier.padding(top = 6.dp),
                    )
                }
                Text(
                    "下載後會逐一驗證 sha256，通過才解壓；接著加入 schema_list 並由 librime " +
                        "重新編譯詞庫（可能要數秒到數十秒）。部署失敗會自動回滾。",
                    fontSize = 12.sp,
                    modifier = Modifier.padding(top = 8.dp),
                )
            }
        },
        confirmButton = {
            TextButton(onClick = { controller.confirmInstall(alsoEnable = true) }) {
                Text("下載並啟用")
            }
        },
        dismissButton = {
            Row {
                TextButton(onClick = { controller.confirmInstall(alsoEnable = false) }) {
                    Text("只下載")
                }
                TextButton(onClick = { controller.dismissConfirm() }) { Text("取消") }
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
        modifier = Modifier.fillMaxSize().padding(12.dp),
        contentAlignment = Alignment.BottomCenter,
    ) {
        Snackbar(
            action = { TextButton(onClick = onDismiss) { Text("關閉") } },
        ) { Text(text, fontSize = 13.sp) }
    }
}

@Composable
private fun ResultDialog(controller: StoreController, r: StoreController.ResultUi) {
    AlertDialog(
        onDismissRequest = { controller.dismissResult() },
        title = { Text(if (r.ok) "完成" else "沒有成功") },
        text = {
            Column {
                Text(r.message, fontSize = 14.sp)
                r.details.forEach {
                    Text("· $it", fontSize = 12.sp, modifier = Modifier.padding(top = 4.dp))
                }
            }
        },
        confirmButton = { TextButton(onClick = { controller.dismissResult() }) { Text("知道了") } },
    )
}

/**
 * 進度覆蓋層。
 *
 * 規範 §3 要求部署期間「顯示進度而非停在空白或假死狀態」。librime 不提供
 * 百分比，所以部署階段用不確定進度條 + **已耗時秒數**，並直說預期量級；
 * 下載階段有 content-length 才用確定進度條。寧可誠實地說「不知道還要多久，
 * 但已經跑了 12 秒」，也不要畫一條假的百分比。
 */
@Composable
private fun JobOverlay(job: StoreController.JobUi) {
    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(Color(0x99000000)),
        contentAlignment = Alignment.Center,
    ) {
        Card(modifier = Modifier.fillMaxWidth().padding(24.dp)) {
            Column(Modifier.padding(18.dp)) {
                Text(job.title, fontWeight = FontWeight.SemiBold)
                Text(job.detail, fontSize = 13.sp, modifier = Modifier.padding(top = 6.dp))
                if (job.fraction >= 0f) {
                    LinearProgressIndicator(
                        progress = { job.fraction },
                        modifier = Modifier.fillMaxWidth().padding(top = 12.dp),
                    )
                } else {
                    LinearProgressIndicator(
                        modifier = Modifier.fillMaxWidth().padding(top = 12.dp),
                    )
                }
            }
        }
    }
}
