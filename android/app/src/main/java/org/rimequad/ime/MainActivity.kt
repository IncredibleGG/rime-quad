package org.rimequad.ime

import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.provider.Settings
import android.view.inputmethod.InputMethodManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Tab
import androidx.compose.material3.TabRow
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import org.rimequad.ime.core.RimeCore
import org.rimequad.ime.core.RimeDeployStatus
import org.rimequad.ime.core.RimeRuntime
import org.rimequad.ime.prefs.SettingsActivity
import org.rimequad.ime.prefs.SettingsScreen
import org.rimequad.ime.store.StoreController
import org.rimequad.ime.store.StoreOverlays
import org.rimequad.ime.store.StoreScreen
import org.rimequad.ime.ui.RimeTheme

/**
 * 設定／診斷畫面，同時也是 method.xml 指定的 settingsActivity。
 *
 * 現階段的職責很窄：告訴人「輸入法有沒有啟用」「native 那條線通了沒」。
 */
class MainActivity : ComponentActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        RimeRuntime.start(applicationContext)
        val store = StoreController(applicationContext)
        setContent {
            RimeTheme {
                // SettingsActivity 的「方案管理」按鈕會帶著這個 extra 進來,
                // 讓使用者一步落在市集分頁而不是預設分頁。沒有 extra 時照舊。
                var tab by remember {
                    mutableStateOf(intent?.getIntExtra(SettingsActivity.EXTRA_TAB, 0) ?: 0)
                }
                Scaffold { padding ->
                    Column(
                        modifier = Modifier
                            .fillMaxSize()
                            .padding(padding)
                            .padding(horizontal = 16.dp)
                            .padding(top = 8.dp)
                    ) {
                        TabRow(selectedTabIndex = tab) {
                            Tab(
                                selected = tab == 0,
                                onClick = { tab = 0 },
                                text = { Text("診斷") },
                            )
                            Tab(
                                selected = tab == 1,
                                onClick = { tab = 1 },
                                text = { Text("方案市集") },
                            )
                            // 設定分頁。內容全部在 org.rimequad.ime.prefs 底下,
                            // 這裡只是掛上去的入口。
                            Tab(
                                selected = tab == 2,
                                onClick = { tab = 2 },
                                text = { Text("設定") },
                            )
                        }
                        Box(modifier = Modifier.padding(top = 12.dp)) {
                            when (tab) {
                                0 -> SetupScreen(
                                    controller = store,
                                    modifier = Modifier.fillMaxSize(),
                                )

                                2 -> SettingsScreen(
                                    modifier = Modifier.fillMaxSize(),
                                    onOpenSchemaStore = { tab = 1 },
                                )

                                else -> StoreScreen(
                                    controller = store,
                                    modifier = Modifier.fillMaxSize(),
                                )
                            }
                            // 部署進度／結果畫在分頁之外：兩個分頁按的是同一個
                            // rs_deploy()，提示不該有兩套。
                            StoreOverlays(store)
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun SetupScreen(controller: StoreController, modifier: Modifier = Modifier) {
    val context = LocalContext.current
    var deployStatus by remember { mutableStateOf(RimeCore.lastDeployStatus) }
    var phase by remember { mutableStateOf(RimeRuntime.phase) }

    DisposableEffect(Unit) {
        val deployListener: (RimeDeployStatus) -> Unit = { deployStatus = it }
        val phaseListener: (RimeRuntime.Phase) -> Unit = { phase = it }
        RimeCore.addDeployListener(deployListener)
        RimeRuntime.addListener(phaseListener)
        onDispose {
            RimeCore.removeDeployListener(deployListener)
            RimeRuntime.removeListener(phaseListener)
        }
    }

    // 進到診斷頁就把 schema_list / 已安裝清單讀進來，否則要切到市集頁才會有值。
    LaunchedEffect(phase, controller.refreshTick) { controller.refreshLocalState() }

    Column(
        modifier = modifier.verticalScroll(rememberScrollState()),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Text(
            text = "RIME 四端輸入法（Android）",
            style = MaterialTheme.typography.headlineSmall,
        )

        if (RimeCore.isStub()) {
            InfoCard(
                title = "⟦STUB⟧ 目前跑的是假實作",
                body = "librime 尚未接上，候選字全是佔位資料。\n" +
                    "third_party/prebuilt/<abi>/lib/librime.a 與 core/src/rime_shell.cc " +
                    "齊備後重新建置，會自動切換成真的。",
            )
        }

        InfoCard(
            title = "原生層狀態",
            body = buildString {
                appendLine("so 載入: ${if (RimeCore.libraryLoaded) "成功" else "失敗 — ${RimeCore.libraryLoadError}"}")
                appendLine("rime_shell ABI（執行期）: ${RimeCore.abiVersion()}")
                appendLine("rime_shell ABI（so 編譯期）: ${RimeCore.compiledAbiVersion()}")
                appendLine("上層要求 ABI: ${RimeCore.EXPECTED_ABI_VERSION}")
                appendLine("ABI 相容: ${if (RimeCore.abiCompatible()) "是" else "否"}")
                appendLine("實作: ${if (RimeCore.isStub()) "stub 假實作" else "真 librime"}")
                appendLine("初始化階段: $phase")
                RimeRuntime.initError?.let { appendLine("錯誤: $it") }
                appendLine(
                    "解壓耗時: " +
                        if (RimeRuntime.extractMillis >= 0) "${RimeRuntime.extractMillis} ms" else "本次未解壓"
                )
                appendLine(
                    "首次部署耗時: " +
                        if (RimeRuntime.deployMillis >= 0) "${RimeRuntime.deployMillis} ms" else "進行中／未完成"
                )
                append("部署狀態: $deployStatus")
            },
        )

        InfoCard(
            title = "資料目錄",
            body = RimeRuntime.describeDataDirs(),
        )

        // refreshTick：市集或「重新部署」跑完之後強制重讀，否則畫面上的
        // schema 清單會停在部署前的樣子。
        val schemas = remember(phase, controller.refreshTick) { RimeCore.schemaList() }
        InfoCard(
            title = "Schema（${schemas.size}）",
            body = if (schemas.isEmpty()) {
                "尚無可用 schema"
            } else {
                schemas.joinToString("\n") { "${it.id}  —  ${it.name}" }
            },
        )

        val enabled = remember(controller.refreshTick, phase) { controller.enabledSchemas }
        InfoCard(
            title = "schema_list patch（已啟用 ${enabled.size}）",
            body = if (enabled.isEmpty()) "default.custom.yaml 沒有 schema_list patch"
            else enabled.joinToString("\n"),
        )

        Button(
            onClick = {
                context.startActivity(
                    Intent(Settings.ACTION_INPUT_METHOD_SETTINGS)
                        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                )
            },
            modifier = Modifier.fillMaxWidth(),
        ) {
            Text("① 到系統設定啟用本輸入法")
        }

        Button(
            onClick = {
                val imm = context.getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
                imm.showInputMethodPicker()
            },
            modifier = Modifier.fillMaxWidth(),
        ) {
            Text("② 切換到本輸入法")
        }

        // ⚠ 這裡刻意**不**直接呼叫 RimeCore.deploy()。
        // rs_deploy() 是非同步的：直接呼叫會立刻返回，畫面上什麼都不會發生，
        // 而背景其實跑了十幾秒（實測 模擬器 7.2s / 三星 S24U 首次 12.5s）。
        // 使用者按下去只會看到「毫無反應」——這是真機回報的 bug。
        // 走 controller.redeploy() 才有進行中狀態、耗時、成功／失敗提示，
        // 以及部署後的 session 重建。
        OutlinedButton(
            onClick = { controller.redeploy() },
            enabled = RimeRuntime.isReady && !controller.busy,
            modifier = Modifier.fillMaxWidth(),
        ) {
            Text(if (controller.busy) "部署中…" else "重新部署（rs_deploy）")
        }
    }
}

@Composable
private fun InfoCard(title: String, body: String) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(modifier = Modifier.padding(14.dp)) {
            Text(text = title, fontWeight = FontWeight.SemiBold)
            Text(
                text = body,
                fontSize = 13.sp,
                fontFamily = FontFamily.Monospace,
                modifier = Modifier.padding(top = 6.dp),
            )
        }
    }
}
