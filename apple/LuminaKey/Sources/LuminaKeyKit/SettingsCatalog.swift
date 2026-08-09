//
//  SettingsCatalog.swift — 設定介面的資訊架構,寫成資料
//
//  ── 為什麼設定要先變成一份資料 ──────────────────────────────────────────
//  使用者的原話是「你這個第一屏 99% 的人看不懂」。原因不是選項太多,
//  是每一個選項都用**實作的詞**在講話(`schema_list`、`page_size`、
//  `simplification`)。所以這裡規定:每一項設定都必須帶一句**白話**,
//  說它會改變什麼 —— 而且那句話由型別強制存在(`blurb` 不是 Optional)。
//
//  把 IA 變成資料還有兩個好處:
//    · **測得到。** 「有沒有哪一項沒有白話說明」「有沒有把 YAML 欄位名
//      搬到畫面上」「有沒有設定項不屬於任何一頁」在 CI 上是斷言,
//      不是 code review 的注意事項(見 SettingsCatalogTests)。
//    · **四端對得起來。** docs/settings-model.md 那張表就是這一份目錄,
//      Windows / iOS 端照抄 id 與分頁,不必重新發明一套。
//
//  ⚠ 分頁是照 Android 端的現況來的,不是新發明的:
//    鍵盤 / 外觀 / 手感 / 文字 / 連網 / 方案市集 / 進階。
//    桌面端拿掉「手感」整頁(震動、音效、長按都是軟鍵盤專屬),
//    把「鍵盤」改叫「輸入方案」(桌面沒有軟鍵盤可選,要選的是方案),
//    另外加一頁「詞庫」(桌面有檔案系統,做得起來;Android 端還欠著)。
//

import Foundation

// MARK: - 一項設定

public enum SettingKind: Equatable, Sendable {
    /// 開關。
    case toggle
    /// 幾選一。每個選項自己帶白話標籤。
    case choice([SettingChoice])
    /// 按下去會發生一件事(重新部署、匯入、重設)。
    case action
    /// 這一項的樣子是專門畫的(方案排序、詞庫表格、連網紀錄)。
    /// 目錄仍然記著它,是為了讓「每一項都有白話說明」這條規則沒有例外。
    case custom
}

public struct SettingChoice: Equatable, Sendable {
    public let value: String
    public let label: T
    public init(_ value: String, _ label: T) {
        self.value = value
        self.label = label
    }
}

/// 這一項最後寫到哪裡。四端共用的分類,見 docs/settings-model.md §2。
public enum SettingStorage: String, Equatable, Sendable {
    /// `settings.json`(本端的 UI 偏好)。
    case appSettings
    /// `default.custom.yaml` 的 `patch:` —— librime 自己會讀,換一個 RIME 前端也還在。
    case rimeCustomYaml
    /// 每次建立 session 之後用 `rs_set_option()` 設,不落地。
    case sessionOption
    /// 使用者詞庫檔(`custom_phrase.txt`)。
    case userPhrases
    /// 不是設定,是一個動作。
    case none
}

public struct SettingSpec: Equatable, Sendable {
    /// 四端共用的識別字。**不要**跟著介面文字改。
    public let id: String
    public let kind: SettingKind
    public let title: T
    /// **一句白話,說它會改變什麼。** 不是 Optional —— 沒有白話就不該上畫面。
    public let blurb: T
    public let storage: SettingStorage
    /// 對應 `LuminaKeySettings` 的欄位名(`Mirror` 的 label)。
    /// `nil` = 不存在 settings.json 裡。測試靠它確認沒有欄位漏掉。
    public let field: String?

    public init(id: String, kind: SettingKind, title: T, blurb: T,
                storage: SettingStorage, field: String? = nil) {
        self.id = id
        self.kind = kind
        self.title = title
        self.blurb = blurb
        self.storage = storage
        self.field = field
    }
}

public struct SettingsPage: Equatable, Sendable {
    public let id: String
    public let title: T
    /// 這一頁在回答使用者腦中的哪一句話。畫在標題底下。
    public let subtitle: T
    /// SF Symbol 名稱。畫不出來時側欄退回純文字(§9.5.1 的同一條紀律)。
    public let symbol: String
    public let items: [SettingSpec]
}

// MARK: - 目錄

public enum SettingsCatalog {

    /// 實際會出現在側欄的分頁。順序照 `docs/ui-design.md` §5.3。
    ///
    /// ── `dictionaryPage` 的上架紀錄(2026-08-09)────────────────────
    /// 這一頁曾經被拿下來一輪。整條路徑當時都寫好了(解析/合併/掛載、UI、
    /// 單元測試),但 `apple/scripts/verify_user_dict.sh` 用**真的 librime**
    /// 跑出來是:加了詞之後,打它的編碼**候選裡沒有那個詞**。
    /// 一頁長得完全正常、按鈕按得下去、加完詞還會出現在清單裡,
    /// 而使用者回去打字時什麼都不會發生 —— 那正是這個專案抓到過五次的
    /// 「看得到但摸不到」,也是最難回報的一種:使用者只會覺得自己做錯了。
    ///
    /// 現在紅的原因找到了,而且**不是**當時猜的那三個 ——
    /// 是編碼欄裡的空格(見 `UserPhrases` 檔頭)。那一關已經變綠,
    /// 而且加了反向測試證明它對編碼格式是敏感的,所以這一頁回到線上。
    ///
    /// ⚠ **下架的條件跟上架一樣只有一個:那一關紅了就再拿下來。**
    /// 它在 `.github/workflows/macos.yml` 裡是硬失敗,不是 `continue-on-error`。
    public static let pages: [SettingsPage] = [
        schemasPage, remapPage, appearancePage, textPage, dictionaryPage,
        storePage, networkPage, advancedPage,
    ]

    public static var allItems: [SettingSpec] { pages.flatMap(\.items) }

    // ── 輸入方案 ─────────────────────────────────────────
    //
    // 使用者最痛的那一點:「沒有設置界面,用戶要怎麼選擇方案」。
    // 所以它是第一頁,而且第一項就是清單本身。
    public static let schemasPage = SettingsPage(
        id: "schemas",
        title: T("輸入方案", "输入方案", "Input Schemas"),
        subtitle: T("你要用哪一種打字方式", "你要用哪一种打字方式", "How you want to type"),
        symbol: "keyboard",
        items: [
            SettingSpec(
                id: "schemas.list",
                kind: .custom,
                title: T("啟用的方案", "启用的方案", "Enabled schemas"),
                blurb: T("打勾的方案才會出現在切換清單裡。可以拖曳排序,最上面那一個是預設。",
                         "打勾的方案才会出现在切换清单里。可以拖曳排序,最上面那一个是默认。",
                         "Only ticked schemas appear in the switcher. Drag to reorder — the top one is the default."),
                storage: .rimeCustomYaml),
            SettingSpec(
                id: "schemas.followInputMode",
                kind: .toggle,
                title: T("跟著輸入來源自動選繁簡", "跟着输入源自动选繁简", "Match the macOS input source"),
                blurb: T("在系統的輸入來源裡選「簡體」就打簡體字,選「繁體」就打繁體字。關掉的話兩邊都用同一個方案。",
                         "在系统的输入源里选「简体」就打简体字,选「繁体」就打繁体字。关掉的话两边都用同一个方案。",
                         "Picking the Simplified input source types simplified characters, Traditional types traditional. Turn this off to use one schema for both."),
                storage: .appSettings, field: "followInputMode"),
            SettingSpec(
                id: "schemas.pinnedHant",
                kind: .custom,
                title: T("繁體輸入來源用哪一個方案", "繁体输入源用哪一个方案", "Schema for the Traditional input source"),
                blurb: T("選了就固定用它,不再自動挑。",
                         "选了就固定用它,不再自动挑。",
                         "Pin one schema instead of letting LuminaKey choose."),
                storage: .appSettings, field: "pinnedSchemaHant"),
            SettingSpec(
                id: "schemas.pinnedHans",
                kind: .custom,
                title: T("簡體輸入來源用哪一個方案", "简体输入源用哪一个方案", "Schema for the Simplified input source"),
                blurb: T("選了就固定用它,不再自動挑。",
                         "选了就固定用它,不再自动挑。",
                         "Pin one schema instead of letting LuminaKey choose."),
                storage: .appSettings, field: "pinnedSchemaHans"),
            SettingSpec(
                id: "schemas.pinnedGlobal",
                kind: .custom,
                title: T("一律用這個方案", "一律用这个方案", "Always use this schema"),
                blurb: T("關掉上面的自動選擇之後,兩個輸入來源都用它。",
                         "关掉上面的自动选择之后,两个输入源都用它。",
                         "With automatic matching off, both input sources use this one."),
                storage: .appSettings, field: "pinnedSchemaId"),
            SettingSpec(
                id: "schemas.openStore",
                kind: .action,
                title: T("下載更多方案", "下载更多方案", "Get more schemas"),
                blurb: T("倉頡、五筆、粵拼、雙拼等三十多種,需要開啟連網。",
                         "仓颉、五笔、粤拼、双拼等三十多种,需要开启联网。",
                         "Cangjie, Wubi, Cantonese, double pinyin and thirty more. Requires turning networking on."),
                storage: .none),
        ])


    // ── 換鍵 ─────────────────────────────────────────────
    //
    // 使用者的原話:「按下 a 實際上是 b」。
    //
    // ⚠ 這一頁是側欄裡唯一有自己一整套互動的分頁(`docs/ui-design.md` §4.5
    //   把它與市集並列為「兩層深度」的唯二例外),所以它的兩個項目都是
    //   `.custom` / `.action` —— 目錄記著它們只是為了讓「每一項都有白話說明」
    //   這條規則沒有例外,畫面由 SettingsSources/RemapPage.swift 自己畫。
    public static let remapPage = SettingsPage(
        id: "remap",
        title: T("換鍵", "换键", "Swap Keys"),
        subtitle: T("讓某一顆鍵打出別的字母", "让某一颗键打出别的字母",
                    "Make a key type a different letter"),
        symbol: "arrow.left.arrow.right",
        items: [
            SettingSpec(
                id: "remap.keys",
                kind: .custom,
                title: T("把兩顆鍵對調", "把两颗键对调", "Swap two keys"),
                blurb: T("點兩顆鍵就會對調。按著 Shift 也跟著換,⌘ 和 ⌃ 的快捷鍵不受影響。",
                         "点两颗键就会对调。按着 Shift 也跟着换,⌘ 和 ⌃ 的快捷键不受影响。",
                         "Tap two keys to trade them. Shift follows along; ⌘ and ⌃ shortcuts are left alone."),
                storage: .none),
            SettingSpec(
                id: "remap.resetAll",
                kind: .action,
                title: T("全部還原成原本的樣子", "全部还原成原本的样子", "Put every key back"),
                blurb: T("這台電腦上換過的鍵全部回到原位。你打出來的字、你自己加的詞都不受影響。",
                         "这台电脑上换过的键全部回到原位。你打出来的字、你自己加的词都不受影响。",
                         "Every key you changed on this computer goes back. What you type and the words you added are not affected."),
                storage: .none),
        ])

    // ── 外觀 ─────────────────────────────────────────────
    public static let appearancePage = SettingsPage(
        id: "appearance",
        title: T("外觀", "外观", "Appearance"),
        subtitle: T("選字的那個小窗長什麼樣", "选字的那个小窗长什么样", "How the candidate window looks"),
        symbol: "paintpalette",
        items: [
            SettingSpec(
                id: "appearance.themeFamily",
                kind: .custom,
                title: T("配色", "配色", "Colour scheme"),
                blurb: T("換掉選字窗的顏色。深淺是下面另一個選項,這裡只選配色。",
                         "换掉选字窗的颜色。深浅是下面另一个选项,这里只选配色。",
                         "Changes the candidate window's colours. Light/dark is a separate setting below."),
                storage: .appSettings, field: "themeFamily"),
            SettingSpec(
                id: "appearance.appearance",
                kind: .choice([
                    SettingChoice("followSystem", T("跟著電腦", "跟着电脑", "Follow the Mac")),
                    SettingChoice("light", T("一直用淺色", "一直用浅色", "Always light")),
                    SettingChoice("dark", T("一直用深色", "一直用深色", "Always dark")),
                ]),
                title: T("深色或淺色", "深色或浅色", "Light or dark"),
                blurb: T("跟著電腦的話,晚上電腦轉深色,選字窗也會跟著轉。",
                         "跟着电脑的话,晚上电脑转深色,选字窗也会跟着转。",
                         "Following the Mac means the window switches when your Mac does."),
                storage: .appSettings, field: "appearance"),
            SettingSpec(
                id: "appearance.candidateScale",
                kind: .choice([
                    SettingChoice("small", T("小", "小", "Small")),
                    SettingChoice("medium", T("標準", "标准", "Standard")),
                    SettingChoice("large", T("大", "大", "Large")),
                    SettingChoice("extraLarge", T("特大", "特大", "Extra large")),
                ]),
                title: T("選字的字要多大", "选字的字要多大", "Candidate text size"),
                blurb: T("覺得字太小看不清楚就調大。只影響選字窗,不影響你打進去的內容。",
                         "觉得字太小看不清楚就调大。只影响选字窗,不影响你打进去的内容。",
                         "Make it bigger if the candidates are hard to read. Only affects the window, not what you type."),
                storage: .appSettings, field: "candidateScale"),
            SettingSpec(
                id: "appearance.candidateCount",
                kind: .choice([
                    SettingChoice("5", T("5 個", "5 个", "5")),
                    SettingChoice("6", T("6 個", "6 个", "6")),
                    SettingChoice("7", T("7 個", "7 个", "7")),
                    SettingChoice("9", T("9 個", "9 个", "9")),
                ]),
                title: T("一次顯示幾個候選字", "一次显示几个候选字", "Candidates per page"),
                blurb: T("多一點比較不用翻頁,少一點窗比較小。",
                         "多一点比较不用翻页,少一点窗比较小。",
                         "More means less paging; fewer means a smaller window."),
                storage: .rimeCustomYaml),
            SettingSpec(
                id: "appearance.orientation",
                kind: .choice([
                    SettingChoice("followTheme", T("跟著配色", "跟着配色", "Follow the scheme")),
                    SettingChoice("horizontal", T("橫著排", "横着排", "Horizontal")),
                    SettingChoice("vertical", T("直著排", "直着排", "Vertical")),
                ]),
                title: T("候選字排成一橫排還是一直排", "候选字排成一横排还是一直排", "Candidate layout"),
                blurb: T("直排在字多的時候比較好讀,橫排比較不擋住畫面。",
                         "直排在字多的时候比较好读,横排比较不挡住画面。",
                         "Vertical reads better with long candidates; horizontal covers less of your screen."),
                storage: .appSettings, field: "candidateOrientation"),
            SettingSpec(
                id: "appearance.showLabels",
                kind: .choice([
                    SettingChoice("followTheme", T("跟著配色", "跟着配色", "Follow the scheme")),
                    SettingChoice("on", T("顯示", "显示", "Show")),
                    SettingChoice("off", T("不顯示", "不显示", "Hide")),
                ]),
                title: T("候選字前面的號碼", "候选字前面的号码", "Numbers before candidates"),
                blurb: T("號碼就是你可以按的那個數字鍵。不顯示的話窗比較乾淨,但要自己數。",
                         "号码就是你可以按的那个数字键。不显示的话窗比较干净,但要自己数。",
                         "The number is the key you can press. Hiding them looks cleaner but you have to count."),
                storage: .appSettings, field: "showCandidateLabels"),
            SettingSpec(
                id: "appearance.showStatusBar",
                kind: .choice([
                    SettingChoice("followTheme", T("跟著配色", "跟着配色", "Follow the scheme")),
                    SettingChoice("on", T("顯示", "显示", "Show")),
                    SettingChoice("off", T("不顯示", "不显示", "Hide")),
                ]),
                title: T("在選字窗裡顯示目前狀態", "在选字窗里显示目前状态", "Status strip in the window"),
                blurb: T("顯示現在是中文還是英文、簡體還是繁體。不確定自己按到什麼的時候很有用。",
                         "显示现在是中文还是英文、简体还是繁体。不确定自己按到什么的时候很有用。",
                         "Shows whether you are in Chinese or English, simplified or traditional. Useful when you are not sure what you just pressed."),
                storage: .appSettings, field: "showStatusBar"),
        ])

    // ── 文字 ─────────────────────────────────────────────
    public static let textPage = SettingsPage(
        id: "text",
        title: T("文字", "文字", "Text"),
        subtitle: T("打出來是什麼字", "打出来是什么字", "What actually gets typed"),
        symbol: "textformat",
        items: [
            SettingSpec(
                id: "text.variant",
                kind: .choice([
                    SettingChoice("followInputMode", T("跟著輸入來源", "跟着输入源", "Follow the input source")),
                    SettingChoice("traditional", T("一律繁體", "一律繁体", "Always traditional")),
                    SettingChoice("simplified", T("一律簡體", "一律简体", "Always simplified")),
                ]),
                title: T("繁體字還是簡體字", "繁体字还是简体字", "Traditional or simplified"),
                blurb: T("選「一律」的話,不管你在系統選了哪個輸入來源都打同一種字。",
                         "选「一律」的话,不管你在系统选了哪个输入源都打同一种字。",
                         "Choosing Always means you get the same characters whichever input source you picked."),
                storage: .sessionOption, field: "variant"),
            SettingSpec(
                id: "text.punctuation",
                kind: .choice([
                    SettingChoice("followSchema", T("跟著方案", "跟着方案", "Follow the schema")),
                    SettingChoice("full", T("中文標點", "中文标点", "Chinese punctuation")),
                    SettingChoice("half", T("英文標點", "英文标点", "Western punctuation")),
                ]),
                title: T("逗號句號用哪一種", "逗号句号用哪一种", "Punctuation style"),
                blurb: T("中文標點是「,。」,英文標點是「, .」。寫程式的人通常要英文的。",
                         "中文标点是「,。」,英文标点是「, .」。写程序的人通常要英文的。",
                         "Chinese punctuation looks like ,。 and Western looks like , . — most people writing code want Western."),
                storage: .sessionOption, field: "punctuation"),
            SettingSpec(
                id: "text.shape",
                kind: .choice([
                    SettingChoice("followSchema", T("跟著方案", "跟着方案", "Follow the schema")),
                    SettingChoice("halfShape", T("一般寬度", "一般宽度", "Normal width")),
                    SettingChoice("fullShape", T("全形(和漢字一樣寬)", "全角(和汉字一样宽)", "Full width (as wide as a Chinese character)")),
                ]),
                title: T("英文字母和數字的寬度", "英文字母和数字的宽度", "Width of letters and digits"),
                blurb: T("全形的 A 和「中」一樣寬。排版對齊時偶爾會用到,平常用一般寬度。",
                         "全角的 A 和「中」一样宽。排版对齐时偶尔会用到,平常用一般宽度。",
                         "A full-width A is as wide as a Chinese character. Occasionally useful for alignment; normally leave it alone."),
                storage: .sessionOption, field: "shape"),
        ])

    // ── 自己加的詞 ───────────────────────────────────────
    //
    // ⚠ 頁名不叫「詞庫」是有理由的(ui-design §6.2 的對照表):
    //   「詞庫」是我們的詞,不是使用者的詞。使用者想的是「我要它記住這個名字」。
    public static let dictionaryPage = SettingsPage(
        id: "dictionary",
        title: T("自己加的詞", "自己加的词", "Words You Add"),
        subtitle: T("讓它記住人名、公司名、專有名詞",
                    "让它记住人名、公司名、专有名词",
                    "Teach it names and jargon it doesn't know"),
        symbol: "text.book.closed",
        items: [
            SettingSpec(
                id: "dictionary.list",
                kind: .custom,
                title: T("你加過的詞", "你加过的词", "Words you added"),
                // ⚠ 「中間不要空格」這一句是這一頁最要緊的一句話。
                //   右邊那一欄留了空格的話,那個詞永遠打不出來,而且畫面上
                //   一切正常 —— 這一頁曾經因此被拿下來一輪(見 pages 的註解)。
                blurb: T("加了之後,打那幾個鍵它就會排在最前面。右邊那一欄要照你實際按的鍵寫,中間不要空格。",
                         "加了之后,打那几个键它就会排在最前面。右边那一栏要照你实际按的键写,中间不要空格。",
                         "Once added, typing those keys brings it up first. Write the right-hand box exactly as you type it, with no spaces."),
                storage: .userPhrases),
            SettingSpec(
                id: "dictionary.export",
                kind: .action,
                title: T("匯出成一個檔案", "导出成一个文件", "Export to a file"),
                blurb: T("存成一個文字檔,可以帶去另一台電腦或手機。手機版讀得懂同一個格式。",
                         "存成一个文本文件,可以带去另一台电脑或手机。手机版读得懂同一个格式。",
                         "Saves a plain text file you can move to another computer or phone — the mobile version reads the same format."),
                storage: .none),
            SettingSpec(
                id: "dictionary.import",
                kind: .action,
                title: T("從檔案匯入", "从文件导入", "Import from a file"),
                blurb: T("把別台機器匯出的詞加進來。重複的詞會被合併,不會變成兩筆。",
                         "把别台机器导出的词加进来。重复的词会被合并,不会变成两笔。",
                         "Brings in words exported elsewhere. Duplicates are merged, not doubled."),
                storage: .none),
        ])

    // ── 方案市集 ─────────────────────────────────────────
    public static let storePage = SettingsPage(
        id: "store",
        title: T("方案市集", "方案市集", "Schema Store"),
        subtitle: T("下載更多打字方式", "下载更多打字方式", "Download more ways to type"),
        symbol: "square.and.arrow.down",
        items: [
            SettingSpec(
                id: "store.browse",
                kind: .custom,
                title: T("可以下載的方案", "可以下载的方案", "Available schemas"),
                blurb: T("下載會連上網路,而且只在你按下按鈕的那一刻。每一次連線都會記在「連網」那一頁。",
                         "下载会连上网络,而且只在你按下按钮的那一刻。每一次连线都会记在「联网」那一页。",
                         "Downloading connects to the internet, and only when you press the button. Every connection is listed on the Networking page."),
                storage: .none),
            SettingSpec(
                id: "store.indexUrl",
                kind: .custom,
                title: T("清單從哪裡來", "清单从哪里来", "Where the list comes from"),
                blurb: T("預設是我們的伺服器。你可以換成自己架的一份。",
                         "默认是我们的服务器。你可以换成自己架的一份。",
                         "Defaults to our server. You can point it at your own copy."),
                storage: .appSettings, field: "storeIndexUrl"),
        ])

    // ── 連網 ─────────────────────────────────────────────
    //
    // 離線是這個專案的招牌,所以它是一整頁,不是「進階」底下的一列。
    public static let networkPage = SettingsPage(
        id: "network",
        title: T("連網", "联网", "Networking"),
        subtitle: T("預設完全不連。開不開由你決定", "默认完全不连。开不开由你决定", "Off by default. You decide."),
        symbol: "network",
        items: [
            SettingSpec(
                id: "network.enabled",
                kind: .toggle,
                title: T("允許連上網路", "允许连上网络", "Allow internet access"),
                blurb: T("關著的時候,這個輸入法一個封包都不會送出去。下載方案與檢查更新才需要打開。",
                         "关着的时候,这个输入法一个数据包都不会送出去。下载方案与检查更新才需要打开。",
                         "While off, this input method sends nothing at all. Only downloading schemas and checking for updates need it on."),
                storage: .appSettings, field: "networkEnabled"),
            SettingSpec(
                id: "network.log",
                kind: .custom,
                title: T("連過哪些地方", "连过哪些地方", "What it connected to"),
                blurb: T("每一次真的連上去都會留一筆。開關從沒開過的話,這裡就是空的 —— 你可以自己確認。",
                         "每一次真的连上去都会留一笔。开关从没开过的话,这里就是空的 —— 你可以自己确认。",
                         "Every connection that actually happened is listed. If you never turned the switch on, this list is empty — check it yourself."),
                storage: .none),
        ])

    // ── 進階 ─────────────────────────────────────────────
    public static let advancedPage = SettingsPage(
        id: "advanced",
        title: T("進階", "进阶", "Advanced"),
        subtitle: T("出問題的時候來這裡", "出问题的时候来这里", "Come here when something is wrong"),
        symbol: "wrench.and.screwdriver",
        items: [
            SettingSpec(
                id: "advanced.redeploy",
                kind: .action,
                title: T("重新整理字詞", "重新整理字词", "Rebuild dictionaries"),
                blurb: T("改過設定但沒生效的時候按這個。要花幾秒到幾十秒,過程中會顯示進度。",
                         "改过设置但没生效的时候按这个。要花几秒到几十秒,过程中会显示进度。",
                         "Press this when a change did not take effect. It takes seconds to a minute and shows progress while it runs."),
                storage: .none),
            SettingSpec(
                id: "advanced.import",
                kind: .action,
                title: T("匯入自己準備的方案檔", "导入自己准备的方案文件", "Import your own schema file"),
                blurb: T("從別的地方拿到的方案壓縮檔或單一設定檔。裝之前會先檢查它有沒有夾帶奇怪的東西。",
                         "从别的地方拿到的方案压缩档或单一配置档。装之前会先检查它有没有夹带奇怪的东西。",
                         "A schema archive or single config file you got elsewhere. It is checked for anything odd before being installed."),
                storage: .none),
            SettingSpec(
                id: "advanced.language",
                kind: .choice([
                    SettingChoice("system", T("跟著系統", "跟随系统", "Follow system")),
                    SettingChoice("zh-Hant", T("繁體中文", "繁体中文", "Traditional Chinese")),
                    SettingChoice("zh-Hans", T("簡體中文", "简体中文", "Simplified Chinese")),
                    SettingChoice("en", T("English", "English", "English")),
                ]),
                title: T("這個設定畫面用什麼語言", "这个设置画面用什么语言", "Language of this settings window"),
                blurb: T("只影響這個視窗的文字,不影響你打得出什麼字。",
                         "只影响这个窗口的文字,不影响你打得出什么字。",
                         "Only changes the text in this window, not what you can type."),
                storage: .appSettings, field: "uiLanguage"),
            SettingSpec(
                id: "advanced.reset",
                kind: .action,
                title: T("全部回復預設", "全部恢复默认", "Reset everything"),
                blurb: T("把上面每一項都變回原廠設定。你自己加的詞和裝的方案不會被刪掉。",
                         "把上面每一项都变回原厂设置。你自己加的词和装的方案不会被删掉。",
                         "Puts every setting back to how it shipped. Your own words and installed schemas are kept."),
                storage: .none),
            SettingSpec(
                id: "advanced.diagnostics",
                kind: .custom,
                title: T("回報問題用的資訊", "报告问题用的信息", "Information for bug reports"),
                blurb: T("這些數字是給我們看的,你不用懂。按一下複製,貼進問題回報裡就好。",
                         "这些数字是给我们看的,你不用懂。按一下复制,贴进问题报告里就好。",
                         "These numbers are for us, not for you. Copy them and paste into a bug report."),
                storage: .none),
        ])

    /// 診斷區塊**永遠是英文**,與 Android 端同一個決定:
    /// 它不是介面文字,是一份要被貼進 issue 的回報載荷。翻譯它會讓收到的
    /// issue 有三種語言的欄位名,grep 不到也對不起來。
    public static let diagnosticsNote = T(
        "這一段永遠是英文,因為它是要貼給我們看的。",
        "这一段永远是英文,因为它是要贴给我们看的。",
        "This block is always in English — it is meant to be pasted to us.")

    // MARK: - 給測試與 docs 用的查詢

    /// `LuminaKeySettings` 裡**不**由使用者直接操作的欄位。
    /// 測試會拿它去扣掉,剩下的每一個欄位都必須在目錄裡出現。
    public static let internalFields: Set<String> = ["version", "seenFirstRunNotice"]

    public static func page(id: String) -> SettingsPage? { pages.first { $0.id == id } }
    public static func item(id: String) -> SettingSpec? { allItems.first { $0.id == id } }
}
