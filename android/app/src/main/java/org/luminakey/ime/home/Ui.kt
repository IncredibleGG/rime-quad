package org.luminakey.ime.home

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import org.luminakey.ime.R

/*
 * App 這一側的共用零件。
 *
 * 三條規矩，整個 App 都照著走：
 *   · **每屏最多一顆實心按鈕**，其餘一律外框或純文字 —— 不必讀字就知道該點哪裡。
 *   · **一張卡，不是一堆卡**：同一組東西放進同一張卡用 1px 分隔線切開；
 *     不同組之間用留白與小標題分開，不用第二張卡。
 *   · 第二層一定是終點：那一層裡不再有任何 `›`。
 *
 * ── 無障礙：為什麼寫在這一層而不是每個畫面各自補 ────────────────────────
 * `contentDescription` 補在畫面上，就等於**每加一個畫面都要記得補一次**，
 * 而漏掉的那一次不會有任何徵兆：畫面完全正常、自動化全過，只有開著 TalkBack
 * 的人摸到一格沉默的方塊。這與本專案抓過的五顆死鍵是同一種缺陷。
 *
 * 所以互動語意收在這幾個元件裡，畫面只負責給字：
 *   · [NavRow] / [QuietRow] 一定帶 `onClickLabel` —— TalkBack 會念
 *     「輕點兩下以**打開外觀**」，而不是「輕點兩下以啟動」。
 *   · [SwitchRow] 把 `toggleable` 放在**整列**上（不是只有那顆 Switch），
 *     並給 `stateDescription` —— 否則 TalkBack 只念得到「開關」，念不出開還關。
 *   · [Segmented] / [Chip] 用 `selectable` + [Role.RadioButton]，
 *     而且外面包 `selectableGroup()` —— 少了它，TalkBack 念不出「三之二」。
 *   · 裝飾用的字元（`›`、`✓`）一律 `clearAndSetSemantics {}` 清掉。
 *     不清的話 TalkBack 會逐字念出「大於符號」。
 *
 * `UiA11yTest` 盯著這幾條的**接線**（見該檔的「它抓不到什麼」）。
 */

/** 小標題（灰、小、字距略開）。 */
@Composable
fun SectionLabel(text: String, modifier: Modifier = Modifier) {
    Text(
        text = text,
        fontSize = TypeScale.t5,
        fontWeight = FontWeight.Medium,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        // 小標題是**標題**，不是一句敘述。標記成 heading 之後，TalkBack 使用者
        // 可以用「下一個標題」直接跳著找，不必逐項聽完整頁。
        modifier = modifier
            .padding(start = Space.s1, bottom = Space.s3)
            .semantics { heading() },
    )
}

@Composable
fun Hairline(modifier: Modifier = Modifier) {
    Box(
        modifier
            .fillMaxWidth()
            .height(Dimens.hairline)
            .background(MaterialTheme.colorScheme.outline)
    )
}

/** 一張卡；裡面的列由呼叫端自己用 [RowDivider] 切開。 */
@Composable
fun PlainCard(
    modifier: Modifier = Modifier,
    content: @Composable ColumnScope.() -> Unit,
) {
    Card(
        modifier = modifier.fillMaxWidth(),
        shape = RoundedCornerShape(Radius.large),
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
        // 深色時卡片與底只差一階，靠分隔線切開，**不靠陰影**（§3.5 規則 3）。
        // `0.dp` 表達的是「不要陰影」，不是一個間距，所以不歸 [Space] 管（§3.1）。
        elevation = CardDefaults.cardElevation(defaultElevation = 0.dp),
        content = { Column(content = content) },
    )
}

@Composable
fun RowDivider() {
    Box(
        Modifier
            .fillMaxWidth()
            .padding(start = Space.s5)
            .height(Dimens.hairline)
            .background(MaterialTheme.colorScheme.outline)
    )
}

/**
 * 第一層的一列：**只有名字，和現在的值**。
 *
 * 刻意沒有說明文字 —— 說明文字是給不確定的人看的，而「注音 · 大千」
 * 「淺色 · 標準」這一行灰字本身就是最好的說明。大多數人來設定只是想
 * 確認一件事，看一眼就走，不必點進去。
 */
@Composable
fun NavRow(
    title: String,
    value: String? = null,
    subtitle: String? = null,
    onClick: () -> Unit,
) {
    // TalkBack 念完名字之後會念「輕點兩下以 <這一句>」。給它一個**動詞 + 受詞**，
    // 使用者才知道點下去會發生什麼，而不是只知道「這裡可以點」。
    val openLabel = stringResource(R.string.a11y_open_page, title)
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClickLabel = openLabel, onClick = onClick)
            .heightIn(min = Dimens.row)
            .padding(horizontal = Space.s5, vertical = Space.s4),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Text(
                text = title,
                fontSize = TypeScale.t3,
                fontWeight = FontWeight.Medium,
                // ⚠ 這兩行是在地化之後補的，而且是實測撞出來的。
                //   右邊那行現值以前沒有寬度上限，中文短所以看不出來；換成英文
                //   （“Characters and punctuation follow the keyboard”）之後它把
                //   整列吃光，左邊的「Text」被擠成兩行的「Te / xt」—— 一個標題
                //   從中間斷開，看起來像畫面壞了。名字永遠一行，長了就截。
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            if (subtitle != null) {
                Text(
                    text = subtitle,
                    fontSize = TypeScale.t5,
                    lineHeight = TypeScale.t5Line,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
        if (value != null) {
            Text(
                text = value,
                fontSize = TypeScale.t4,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                // 現值可以折行，但不能超過一半的寬度 —— 它是補充，名字才是主體。
                textAlign = TextAlign.End,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier
                    .weight(1f, fill = false)
                    .padding(start = Space.s4, end = Space.s3),
            )
        }
        Chevron()
    }
}

/**
 * 「還有下一層」的箭頭。
 *
 * 這個字元走資源，不寫在程式裡：它是**有方向的**字元，RTL 語系要換成反向的
 * 那一個。留在 Kotlin 裡就等於把「往前是往右」寫死。
 *
 * 對 TalkBack 它是**噪音**：整列已經念過名字、現值與「輕點兩下以打開○○」，
 * 再念一次「大於符號」只是打斷。所以語意整個清掉。
 */
@Composable
private fun Chevron(fontSize: androidx.compose.ui.unit.TextUnit = TypeScale.t2) {
    Text(
        text = stringResource(R.string.nav_chevron),
        fontSize = fontSize,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        modifier = Modifier.clearAndSetSemantics { },
    )
}

/**
 * 開關列。用在「鍵上顯示小提示」這種純二選一。
 *
 * ⚠ `toggleable` 掛在**整列**上，那顆 `Switch` 的 `onCheckedChange` 因此傳 null。
 * 兩邊都接的話 TalkBack 會看到兩個可點的東西（列一個、開關一個），
 * 而且點列與點開關會各切一次 —— 使用者看到的是「切了等於沒切」。
 */
@Composable
fun SwitchRow(
    title: String,
    subtitle: String? = null,
    checked: Boolean,
    onCheckedChange: (Boolean) -> Unit,
) {
    val on = stringResource(R.string.a11y_state_on)
    val off = stringResource(R.string.a11y_state_off)
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .toggleable(
                value = checked,
                role = Role.Switch,
                onValueChange = onCheckedChange,
            )
            // 沒有這一句，TalkBack 只念得出「開關」，念不出現在是開還是關 ——
            // 一個看不見畫面的人因此永遠不知道自己按下去之後有沒有生效。
            .semantics { stateDescription = if (checked) on else off }
            .heightIn(min = Dimens.row)
            .padding(horizontal = Space.s5, vertical = Space.s4),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Text(text = title, fontSize = TypeScale.t3, fontWeight = FontWeight.Medium)
            if (subtitle != null) {
                Text(
                    text = subtitle,
                    fontSize = TypeScale.t5,
                    lineHeight = TypeScale.t5Line,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
        Switch(checked = checked, onCheckedChange = null)
    }
}

/**
 * 分段控制：**看得到全部選項、當場改完**。
 *
 * 第二層一律用它而不是滑桿或 `›`。滑桿要在數值與預覽之間來回猜；
 * 而使用者要的是「再大一點」，不是 `1.15`。所以這一版的 App 設定畫面上
 * 不出現任何百分比、毫秒或倍數。
 *
 * ⚠ 外層的 `selectableGroup()` 不是裝飾：少了它，TalkBack 念得出每一格的名字，
 * 卻**念不出「第 2 個，共 3 個」**，使用者不知道自己聽完了沒有。
 */
@Composable
fun <T> Segmented(
    options: List<Pair<T, String>>,
    selected: T,
    onSelect: (T) -> Unit,
    modifier: Modifier = Modifier,
) {
    Row(
        modifier = modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(Radius.medium))
            .background(MaterialTheme.colorScheme.surfaceVariant)
            .selectableGroup()
            .padding(Space.s1),
        horizontalArrangement = Arrangement.spacedBy(Space.s1),
    ) {
        for ((value, label) in options) {
            val on = value == selected
            Box(
                modifier = Modifier
                    .weight(1f)
                    .clip(RoundedCornerShape(Radius.mediumInner))
                    .background(
                        if (on) MaterialTheme.colorScheme.surface
                        else MaterialTheme.colorScheme.surfaceVariant
                    )
                    .selectable(
                        selected = on,
                        role = Role.RadioButton,
                        onClick = { onSelect(value) },
                    )
                    // 觸控目標下限。分段控制的格子在窄螢幕上會被擠矮，
                    // 而「要按兩三次才中」在模擬器上用滑鼠點永遠測不出來。
                    .heightIn(min = Dimens.touchTarget)
                    .padding(vertical = Space.s3),
                contentAlignment = Alignment.Center,
            ) {
                Text(
                    text = label,
                    fontSize = TypeScale.t4,
                    maxLines = 1,
                    // 被選中的那一格有**兩個訊號**：底色 + 字重。只有顏色的話，
                    // 色覺障礙、低亮度螢幕、截圖轉灰階三種情況下它就消失了。
                    fontWeight = if (on) FontWeight.SemiBold else FontWeight.Normal,
                    color = if (on) MaterialTheme.colorScheme.onSurface
                    else MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
    }
}

/** 一組「小標題 + 分段控制」，第二層的基本單位。 */
@Composable
fun <T> SettingGroup(
    label: String,
    options: List<Pair<T, String>>,
    selected: T,
    onSelect: (T) -> Unit,
    /** 改了要重新整理字詞才生效的那幾項，把話當場說出來（§4.1）。 */
    footnote: String? = null,
) {
    Column(Modifier.fillMaxWidth().padding(vertical = Space.s3)) {
        SectionLabel(label)
        Segmented(options = options, selected = selected, onSelect = onSelect)
        if (footnote != null) {
            Text(
                text = footnote,
                fontSize = TypeScale.t5,
                lineHeight = TypeScale.t5Line,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(top = Space.s3, start = Space.s1),
            )
        }
    }
}

/**
 * 「隨手試一下」。
 *
 * 「打出第一個字」不該需要離開 App、去找一個聊天室、再點輸入框。
 * 這一格就是能打字的地方，而且**打壞也沒關係**。
 */
@Composable
fun TryField(
    placeholder: String = stringResource(R.string.try_field_placeholder),
    modifier: Modifier = Modifier,
    focusRequester: FocusRequester? = null,
) {
    var text by remember { mutableStateOf("") }
    // 空的輸入框對 TalkBack 是一格沉默的方塊 —— placeholder 不會被念出來。
    val label = stringResource(R.string.a11y_try_field)
    OutlinedTextField(
        value = text,
        onValueChange = { text = it },
        placeholder = { Text(placeholder, fontSize = TypeScale.t3) },
        singleLine = false,
        minLines = 2,
        shape = RoundedCornerShape(Radius.medium),
        modifier = modifier
            .fillMaxWidth()
            .semantics { contentDescription = label }
            .let { if (focusRequester != null) it.focusRequester(focusRequester) else it },
    )
}

/**
 * 單選晶片。用在選項數量不固定的地方（配色有 6 個，而且日後會變多），
 * 分段控制在那種情況下會擠成一團看不懂。
 */
@Composable
fun Chip(
    label: String,
    selected: Boolean,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
) {
    Box(
        modifier = modifier
            .clip(RoundedCornerShape(Radius.medium))
            .background(
                if (selected) MaterialTheme.colorScheme.primaryContainer
                else MaterialTheme.colorScheme.surfaceVariant
            )
            .selectable(selected = selected, role = Role.RadioButton, onClick = onClick)
            .heightIn(min = Dimens.touchTarget)
            .padding(horizontal = Space.s4, vertical = Space.s4),
        contentAlignment = Alignment.Center,
    ) {
        Text(
            text = label,
            fontSize = TypeScale.t4,
            maxLines = 1,
            // 選中 = 底色 + 字重，**兩個訊號**（§4.3）。
            fontWeight = if (selected) FontWeight.SemiBold else FontWeight.Normal,
            color = if (selected) MaterialTheme.colorScheme.onPrimaryContainer
            else MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

/** 分隔線之後的那一行小灰字入口。刻意與上面的列**不等重**。 */
@Composable
fun QuietRow(text: String, onClick: () -> Unit) {
    val openLabel = stringResource(R.string.a11y_open_page, text)
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClickLabel = openLabel, onClick = onClick)
            .heightIn(min = Dimens.touchTarget)
            .padding(vertical = Space.s4, horizontal = Space.s1),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            text = text,
            fontSize = TypeScale.t4,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.weight(1f),
        )
        Chevron(fontSize = TypeScale.t4)
    }
}

/** 第二層頁面的頂端導覽列。**每一個第二層都有它**，出口不靠使用者記得按返回鍵。 */
@Composable
fun PageHeader(title: String, onBack: () -> Unit) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .heightIn(min = Dimens.touchTarget),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        val backLabel = stringResource(R.string.a11y_back)
        TextButton(
            onClick = onBack,
            modifier = Modifier.semantics { contentDescription = backLabel },
        ) { Text(stringResource(R.string.back), fontSize = TypeScale.t4) }
        Spacer(Modifier.padding(horizontal = Space.s1))
        Text(
            text = title,
            fontSize = TypeScale.t2,
            fontWeight = FontWeight.SemiBold,
            // 第二層的頁首是這一頁的標題。標記成 heading，TalkBack 才跳得到。
            modifier = Modifier.semantics { heading() },
        )
    }
}
