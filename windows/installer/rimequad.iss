; windows/installer/rimequad.iss — RimeQuad-Setup-x64.exe
;
; ══ 為什麼是 Inno Setup 而不是 WiX ══════════════════════════════════
;
; 需求是「使用者下載、雙擊、下一步、裝好、能用」,也就是**一個 .exe**。
;
;   · WiX 的原生產物是 .msi。要變成單一 .exe 得再套一層 Burn bootstrapper
;     (bundle),等於為了同一個結果多一層工具鏈與多一種失敗模式。
;   · Inno 直接產生自解的 .exe,而且 UAC 提權是寫在它自己的 manifest 裡
;     (PrivilegesRequired=admin)—— 雙擊就跳提權對話框,不必教使用者按右鍵。
;     這正是使用者這一輪回報的問題:「雙擊沒反應、管理員執行也沒反應」。
;   · 解除安裝要做的事(停掉服務、反註冊、**但保留使用者詞典**)需要真的
;     跑一段邏輯。Inno 的 Pascal script 直接做得到;MSI 的 custom action
;     要另外編一個 DLL。
;   · windows-latest runner 內建 Inno Setup,CI 不必多裝東西。
;
; MSI 唯一贏的地方是網域環境的 GPO 派送。那不是這一輪的目標,而且
; 日後真的需要時,把同一批檔案再包一份 MSI 並不會推翻現在的任何決定。
;
; ══ 編碼 ═══════════════════════════════════════════════════════════
;
; ⚠ 這個檔案裡有中文。Inno Setup 6 只有在 .iss **帶 UTF-8 BOM** 時才會
;   當成 UTF-8 讀,否則走系統 ANSI 代碼頁 —— 在英文的 runner 上(cp1252)
;   那些中文會在編譯階段就變成亂碼,而安裝程式看起來「編成功了」。
;   BOM 由 windows/make_installer.sh 在複製到暫存目錄時加上,
;   版控裡這一份保持乾淨的 UTF-8。**不要直接把這個檔案餵給 ISCC。**
;
; ══ 由 make_installer.sh 以 /D 傳進來的 ════════════════════════════
;   PayloadDir       已經備好的檔案樹(bin + data)
;   AppVersion       顯示用版本字串
;   VersionInfo      x.x.x.x 的數字版本(給 exe 的版本資源)
;   ArchDirective    x64compatible(Inno 6.3+)或 x64(更舊的)

#ifndef PayloadDir
  #error 必須以 /DPayloadDir=<目錄> 呼叫;請用 windows/make_installer.sh
#endif
#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef VersionInfo
  #define VersionInfo "0.0.0.0"
#endif
#ifndef ArchDirective
  #define ArchDirective "x64compatible"
#endif

[Setup]
; ⚠ AppId 一旦發布出去就不能改。它決定「新增或移除程式」裡那一筆的登錄檔
;   鍵名,也決定升級時 Inno 認不認得舊版。改了的話,舊版會永遠留在
;   「新增或移除程式」裡,而且解除安裝不掉。
AppId={{7A033CF7-CB91-408E-A653-EF639F4173DB}
AppName=RIME 四端輸入法
AppVerName=RIME 四端輸入法 {#AppVersion}
AppVersion={#AppVersion}
VersionInfoVersion={#VersionInfo}
AppPublisher=RimeQuad
AppPublisherURL=https://github.com/IncredibleGG/rime-quad
DefaultDirName={autopf}\RimeQuad
DefaultGroupName=RIME 四端輸入法
OutputBaseFilename=RimeQuad-Setup-x64
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern

; ── 提權 ──────────────────────────────────────────────────────────
; 這一行就是「雙擊就跳提權對話框」的來源:Inno 會把
; requestedExecutionLevel=requireAdministrator 寫進安裝程式自己的 manifest。
; 需要提權是因為 COM 與 TSF 的註冊寫的是 HKLM,而且要裝進 Program Files。
PrivilegesRequired=admin

; ── 架構 ──────────────────────────────────────────────────────────
; 兩行都要。第一行擋掉在 32 位元 Windows 上執行(我們只有 x64 的 DLL);
; 第二行讓 {autopf} 展開成 C:\Program Files 而不是 Program Files (x86),
; 而且讓 Inno 用 64 位元的登錄檔檢視 —— 少了它,任何登錄檔動作都會落進
; WOW6432Node,而 64 位元的宿主一個都找不到這個輸入法。
ArchitecturesAllowed={#ArchDirective}
ArchitecturesInstallIn64BitMode={#ArchDirective}

; ToUnicodeEx 的「不要改動鍵盤狀態」旗標需要 Windows 10 1607+。
; 少了它,使用者按過法文的死鍵 ^ 之後那個狀態會被我們吃掉 ——
; 症狀是「裝了這個輸入法之後別的語言的重音字打不出來」。
; 與其在舊系統上安靜地壞掉,不如明著擋。
MinVersion=10.0

; ── 解除安裝 ──────────────────────────────────────────────────────
UninstallDisplayName=RIME 四端輸入法
UninstallDisplayIcon={app}\rime_service.exe

; 不去關使用者正在用的程式。TSF 的 DLL 會被載入到每一個接受文字輸入的
; 進程裡,讓 Inno 的重新啟動管理員去關它們等於「安裝輸入法會關掉你的 Word」。
; 檔案被佔用的情況由下面 [Files] 段的 restartreplace 承接。
CloseApplications=no
SetupMutex=RimeQuadSetupMutex

; 安裝完成頁不推銷任何東西。程式集資料夾裡只放兩個捷徑(見 [Icons]),
; 所以不必讓使用者選資料夾名字。
DisableProgramGroupPage=yes

; 歡迎頁要顯示。Inno 6 的 modern 樣式**預設把它藏起來**,但這裡它有實際用途:
; 使用者裝完之後還得自己去切輸入法,那件事得先講。
DisableWelcomePage=no

; 安裝位置固定,不讓使用者選。
; 服務進程靠「與執行檔同目錄的 data\shared」找執行期資料,而登錄檔裡存的是
; 絕對路徑 —— 使用者事後搬動資料夾就會壞,而症狀是「輸入法忽然不見了」。
; 少一個頁面,也少一種弄壞它的方法。
DisableDirPage=yes

[Icons]
; 只有兩個,而且兩個都真的做事。
;
; ⚠ 「診斷」那一個是這一輪加的,而且它是**修「使用者只能說不能用」這件事
;   本身**的一半:另一半是 doctor 那支程式,這一半是「他找得到它」。
;   一個要開命令列才跑得起來的診斷工具,對絕大多數使用者等於不存在。
Name: "{group}\輸入法設定"; Filename: "{app}\rime_service.exe"; Parameters: "--settings"; Comment: "開啟 RIME 輸入法的設定視窗"
Name: "{group}\診斷:輸入法為什麼不能用"; Filename: "{app}\rime_ime_setup.exe"; Parameters: "doctor --report"; Comment: "檢查安裝、註冊、服務、引擎,並把結果用記事本打開"

[Files]
; restartreplace + uninsrestartdelete:
;   rime_tsf.dll 在升級的當下可能正被宿主進程載入著而換不掉。
;   這兩個旗標讓它排到下次開機再換 / 再刪,而不是讓整個安裝失敗。
Source: "{#PayloadDir}\rime_tsf.dll";       DestDir: "{app}"; Flags: ignoreversion restartreplace uninsrestartdelete
Source: "{#PayloadDir}\rime_service.exe";   DestDir: "{app}"; Flags: ignoreversion restartreplace uninsrestartdelete
Source: "{#PayloadDir}\rime_ime_setup.exe"; DestDir: "{app}"; Flags: ignoreversion restartreplace uninsrestartdelete
; ⚠ rime_console.exe 是**給使用者的診斷工具**,不是開發者的東西。
;   它完全不經過 TSF、不經過管道,直接驅動 librime + 資料 —— 也就是
;   「引擎層通不通」那一刀。`rime_ime_setup.exe doctor` 的第 7 節呼叫它。
;   使用者回報「不能用」時,有沒有這一刀,決定我們是一次問清楚
;   還是來回五輪。
Source: "{#PayloadDir}\rime_console.exe";    DestDir: "{app}"; Flags: ignoreversion restartreplace uninsrestartdelete

; ⚠ 執行期資料。**少了這一段,前面每一步都會成功,服務起得來、輸入法
;   註冊得上,然後一個字都打不出來,而且沒有任何錯誤訊息。**
;   data\shared 是方案、詞庫與 opencc 詞典;data\user 是首次執行要補進
;   %APPDATA%\RimeQuad 的範本(default.custom.yaml 把 schema_list 限縮成
;   我們真的有詞庫的四個方案)。
;   make_installer.sh 在編譯之前會逐項點名檢查這棵樹,缺了就不出安裝程式。
Source: "{#PayloadDir}\data\*"; DestDir: "{app}\data"; Flags: ignoreversion recursesubdirs createallsubdirs

; ⚠ 解除安裝要做的事(停服務、停用、反註冊)**不在 [UninstallRun]**,
;   在底下 [Code] 的 CurUninstallStepChanged(usUninstall)。
;
;   原因:那三件事裡有兩件必須以**登入者**的身分做(服務是使用者自己那一支,
;   HKCU 也是使用者自己的),而 runasoriginaluser 是 [Run] 專屬的旗標 ——
;   放進 [UninstallRun] 會讓 ISCC 直接拒絕編譯:
;     「Parameter "Flags" includes a flag that is not supported in this section.」
;   (實測過,CI run 上就是這個錯。)
;   搬到 [Code] 之後順序、身分、以及失敗時的記錄都在我們手上,反而更清楚。

; ⚠ 沒有 [UninstallDelete] 去碰使用者資料目錄,而且**不可以加**。
;   那裡是使用者的詞典、自訂短語與設定 —— 是使用者的資料,不是我們的檔案。
;   預設一律留下;重新安裝時使用者的詞會原封不動地回來。
;
;   使用者真的不想要了的話,有一條**明確的**路:解除安裝時會問一次
;   (預設「否」),或靜默解除安裝時傳 /PURGEUSERDATA。見底下 [Code] 的
;   DecidePurge / DoPurge。用 [UninstallDelete] 做不到「預設不刪、
;   問過才刪」,而且它也沒辦法在刪之前向產品確認路徑。
;
;   CI 兩條路都驗:
;     · 不帶旗標的靜默解除安裝 → %APPDATA% 底下的檔案必須還在(§9)
;     · 帶 /PURGEUSERDATA 的   → 必須整個消失(§10)
;   只驗一條等於沒驗到這個功能。

[Messages]
; Inno 6 沒有隨附繁體中文的 .isl(官方只帶二十幾個語言,中文在非官方那一份)。
; 與其相依一個 runner 上不保證存在的檔案,不如把實際會出現在畫面上的那些字
; 直接覆寫掉 —— 使用者看到的是一致的中文,而建置不多一個外部相依。
SetupAppTitle=安裝
SetupWindowTitle=安裝 - %1
WelcomeLabel1=歡迎安裝 [name]
WelcomeLabel2=這將在您的電腦上安裝 [name/ver]。%n%n安裝完成後,請在工作列的輸入法切換清單(Win + 空白鍵)裡選擇本輸入法。%n%n建議先關閉其他程式再繼續。
ClickNext=按「下一步」繼續,或按「取消」結束安裝。
ButtonNext=下一步(&N) >
ButtonBack=< 上一步(&B)
ButtonCancel=取消
ButtonInstall=安裝(&I)
ButtonFinish=完成(&F)
ButtonYes=是(&Y)
ButtonNo=否(&N)
WizardReady=準備安裝
ReadyLabel1=安裝程式已準備好將 [name] 安裝到您的電腦。
ReadyLabel2a=按「安裝」開始安裝,或按「上一步」檢視或變更設定。
ReadyLabel2b=按「安裝」開始安裝。
WizardPreparing=準備中
PreparingDesc=安裝程式正在準備安裝 [name]。
WizardInstalling=安裝中
InstallingLabel=請稍候,正在安裝 [name]…
StatusExtractFiles=正在解開檔案…
StatusRunProgram=正在完成安裝…
FinishedHeadingLabel=[name] 安裝完成
FinishedLabelNoIcons=[name] 已安裝在您的電腦上。
FinishedLabel=[name] 已安裝在您的電腦上。%n%n用法:按 Win + 空白鍵切換輸入法,選擇「RIME 四端輸入法」。%n%n若清單裡沒有出現,請到「設定 → 時間與語言 → 語言與地區」把「中文(繁體,台灣)」加入語言清單,再試一次。%n%n首次使用時輸入法會在背景編譯詞庫,可能需要一到數分鐘;在那之前打出來的是英文,這是正常的。%n%n若打不出中文或看不到設定視窗,請執行「開始」功能表裡的「診斷:輸入法為什麼不能用」,它會把原因寫成一份報告並用記事本打開。
ExitSetupTitle=結束安裝
ExitSetupMessage=安裝尚未完成。現在結束的話,本輸入法不會被安裝。%n%n要結束安裝嗎?
ConfirmUninstall=您確定要完整移除 %1 嗎?
UninstallStatusLabel=請稍候,正在從您的電腦移除 %1…
StatusUninstalling=正在移除 %1…
UninstalledAll=%1 已從您的電腦順利移除。
UninstalledMost=%1 移除完成。%n%n有部分項目無法移除,重新啟動電腦後會自動清除。
WindowsVersionNotSupported=本輸入法需要 Windows 10 1607 或更新的版本。
OnlyOnTheseArchitectures=本輸入法只提供 64 位元版本,無法安裝在這台電腦上。

[CustomMessages]
RegisterFailed=註冊輸入法失敗(rime_ime_setup.exe %1,結束碼 %2)。%n%n輸入法沒有被系統接受,現在就算裝完了也不會出現在輸入法清單上。%n安裝已中止,不會留下一個裝了卻用不了的狀態。
UninstallKeptUserData=您的詞典與設定保留在:%n%n%1%n%n那是您自己的資料(學過的詞、自訂短語),移除輸入法時刻意不刪。%n重新安裝時會原封不動地回來;確定不要了再自行刪除該資料夾。
UninstallAskPurge=要順便刪除您的詞典與設定嗎?%n%n%1%n%n那裡面是您使用期間學會的詞、自訂短語與設定。%n%n⚠ 刪除之後**無法復原** —— 重新安裝也救不回來。%n%n選「否」會保留它(建議);之後改變主意再自行刪除該資料夾即可。%n選「是」會立刻永久刪除。
UninstallPurgeDone=您的詞典與設定已刪除:%n%n%1
UninstallPurgeFailed=有部分資料沒有刪掉(可能有檔案正被使用):%n%n%1%n%n重新開機後手動刪除該資料夾即可。

[Code]

const
  SetupExeName = 'rime_ime_setup.exe';

// 停掉舊的服務進程。
//
// 兩件事:升級時 rime_service.exe 被佔用著換不掉;而且它持有使用者詞庫的
// LevelDB,不能直接被砍。rime_ime_setup.exe stop-service 會先送結束事件、
// 等它收尾,真的沒反應才強制結束。
//
// 兩種身分都跑一次是刻意的:服務是**登入者**那一支(所以 runasoriginaluser),
// 但使用者也可能是以另一個帳號登入後提權的,那時提權那一側才找得到。
procedure StopExistingService;
var
  OldExe: String;
  Rc: Integer;
begin
  OldExe := ExpandConstant('{app}\' + SetupExeName);
  if not FileExists(OldExe) then Exit;
  ExecAsOriginalUser(OldExe, 'stop-service --dir "' + ExpandConstant('{app}') + '"',
                     '', SW_HIDE, ewWaitUntilTerminated, Rc);
  Exec(OldExe, 'stop-service --dir "' + ExpandConstant('{app}') + '"',
       '', SW_HIDE, ewWaitUntilTerminated, Rc);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  // 在複製檔案**之前**。跑到 [Run] 才停就已經太遲了 —— 那時檔案早就換過。
  StopExistingService;
  Result := '';
end;

procedure RunSetupVerbOrFail(Verb: String);
var
  Exe: String;
  Rc: Integer;
begin
  Exe := ExpandConstant('{app}\' + SetupExeName);
  if (not Exec(Exe, Verb, '', SW_HIDE, ewWaitUntilTerminated, Rc)) or (Rc <> 0) then
    RaiseException(FmtMessage(CustomMessage('RegisterFailed'), [Verb, IntToStr(Rc)]));
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Exe: String;
  Rc: Integer;
begin
  if CurStep <> ssPostInstall then Exit;
  Exe := ExpandConstant('{app}\' + SetupExeName);

  // 註冊失敗必須讓整個安裝失敗。
  //
  // 這個專案吃過太多次「一片全綠而其實沒在做事」的虧,而在安裝程式上,
  // 那的具體長相就是「顯示安裝成功,使用者的輸入法清單裡什麼都沒有」。
  // 寧可中止並留下訊息。
  RunSetupVerbOrFail('register');

  // ── 使用者那一側 ────────────────────────────────────────────────
  //
  // **兩種身分都跑**,而且都 best-effort。
  //
  //   · ExecAsOriginalUser 是正確的那一條:使用者用系統管理員帳號提權安裝時,
  //     提權後的 HKCU 是**那個管理員的**,真正在用電腦的人什麼都沒拿到。
  //   · 但它需要拿得到「原始使用者」的權杖,而那來自 shell(Explorer)。
  //     沒有互動式桌面的環境(例如 CI runner)上它拿不到,於是整步靜靜地
  //     什麼都沒做 —— 實測就是這樣:全機註冊全綠,HKCU 底下一片空白。
  //   · 所以提權那一條也跑一次。它寫的是執行安裝那個帳號的 HKCU,
  //     對「自己就是管理員」的一般情況正好是對的。
  //
  // 失敗不中止安裝:輸入法已經全機註冊好了,使用者仍然可以自己從「設定」
  // 把它加進來。為了這一步讓整個安裝回滾是不成比例的。
  //
  // 結果寫進安裝記錄(/LOG)。沒有它的話,「enable-user 沒跑到」與
  // 「跑了但 EnableLanguageProfile 回錯」在報表上長得一模一樣,
  // 而要分辨得再等一輪 CI。
  try
    if ExecAsOriginalUser(Exe, 'enable-user', '', SW_HIDE, ewWaitUntilTerminated, Rc) then
      Log('RimeQuad: enable-user(登入者)rc=' + IntToStr(Rc))
    else
      Log('RimeQuad: enable-user(登入者)**啟動失敗** —— 多半是沒有互動式 shell');
  except
    Log('RimeQuad: ExecAsOriginalUser 不可用');
  end;

  if Exec(Exe, 'enable-user', '', SW_HIDE, ewWaitUntilTerminated, Rc) then
    Log('RimeQuad: enable-user(目前身分)rc=' + IntToStr(Rc))
  else
    Log('RimeQuad: enable-user(目前身分)啟動失敗');

  // ── 最後才自我檢查 ──────────────────────────────────────────────
  //
  // ⚠ 順序是踩出來的,不是隨便排的。原本 check 排在 enable-user **之前**,
  //   而 check 失敗時 RaiseException 會讓整個 CurStepChanged 中止 ——
  //   於是 enable-user 從來沒被執行過。症狀:全機註冊全綠、
  //   使用者清單一片空白,而錯誤訊息講的是「註冊失敗」。
  //   一個判斷失誤造成兩個看起來無關的症狀。
  //
  // ⚠ --no-enum:剛註冊完的當下 CTF 還看不到新的設定檔(實測 register 回傳
  //   成功之後 0.12 秒時 EnumLanguageProfiles 看不到,22 秒後同一支程式跑
  //   同一段就全部看得到)。登錄檔是同步的,CTF 的可見性不是。
  //   所以這裡只驗登錄檔;「系統接受了嗎」由 CI 事後不帶 --no-enum 再問一次。
  //
  // ⚠ 而且要誠實說一件事:在 /SUPPRESSMSGBOXES 之下,RaiseException 只會被
  //   記進安裝記錄、對話框自動按掉,**Setup 仍然以 0 結束**。
  //   所以這一步對「靜默安裝」不是真正的閘門 —— 真正的閘門是 CI 的
  //   windows/verify_installer.sh(它會斷言安裝記錄裡沒有 raised an exception)。
  //   互動安裝時使用者會看到錯誤訊息,那一條是成立的。
  RunSetupVerbOrFail('check --no-enum');
end;

// 解除安裝時的收尾。
//
// usUninstall 是**刪檔案之前**,所以 {app}\rime_ime_setup.exe 還在 ——
// 這三件事全都要靠它。
//
// 身分很重要,而且兩種都要跑:
//   · 服務是**登入者**那一支,結束事件在他的工作階段命名空間裡;
//     HKCU 也是他的。這兩件要 ExecAsOriginalUser。
//   · 但使用者也可能是以另一個帳號提權來解除安裝的,那時提權那一側
//     才找得到東西。所以提權身分也跑一次。
//   · unregister 寫的是 HKLM,只有提權那一側做得到。
//
// 全部 best-effort:解除安裝不該因為服務停不掉就失敗。真的清乾淨了沒有,
// 由 CI 的 windows/verify_installer.sh 去斷言,不是靠這裡的結束碼。
procedure UninstallCleanup;
var
  Exe, AppDir: String;
  Rc: Integer;
begin
  AppDir := ExpandConstant('{app}');
  Exe := AppDir + '\' + SetupExeName;
  if not FileExists(Exe) then begin
    Log('RimeQuad: 找不到 ' + Exe + ',跳過反註冊');
    Exit;
  end;

  // ⚠ ExecAsOriginalUser 包在 try 裡。
  //
  // Inno 有一部分的支援函式在**解除安裝**的情境下不能呼叫,叫了會丟例外。
  // 我沒有辦法在這台機器上確認它屬不屬於那一類,而賭錯的後果是
  // 「解除安裝走到一半丟出例外」—— 比停不掉服務嚴重得多。
  // 包起來之後,最壞情況只是退回下面提權那一次,清理照樣完成。
  try
    if ExecAsOriginalUser(Exe, 'stop-service --dir "' + AppDir + '"', '',
                          SW_HIDE, ewWaitUntilTerminated, Rc) then
      Log('RimeQuad: stop-service(登入者)rc=' + IntToStr(Rc))
    else
      Log('RimeQuad: stop-service(登入者)啟動失敗');
  except
    Log('RimeQuad: ExecAsOriginalUser 在解除安裝情境下不可用,改走提權那一條');
  end;

  if Exec(Exe, 'stop-service --dir "' + AppDir + '"', '',
          SW_HIDE, ewWaitUntilTerminated, Rc) then
    Log('RimeQuad: stop-service(提權)rc=' + IntToStr(Rc));

  try
    if ExecAsOriginalUser(Exe, 'disable-user', '', SW_HIDE, ewWaitUntilTerminated, Rc) then
      Log('RimeQuad: disable-user(登入者)rc=' + IntToStr(Rc))
    else
      Log('RimeQuad: disable-user(登入者)啟動失敗');
  except
    Log('RimeQuad: disable-user(登入者)無法執行');
  end;
  // 提權那一側的 HKCU 也清一次(unregister 會刪掉整棵 HKCU CTF 子樹)。
  if Exec(Exe, 'disable-user', '', SW_HIDE, ewWaitUntilTerminated, Rc) then
    Log('RimeQuad: disable-user(提權)rc=' + IntToStr(Rc));

  if Exec(Exe, 'unregister', '', SW_HIDE, ewWaitUntilTerminated, Rc) then
    Log('RimeQuad: unregister rc=' + IntToStr(Rc))
  else
    Log('RimeQuad: unregister 啟動失敗 —— 登錄檔會留下殘骸');
end;

// ══════════════════════════════════════════════════════════════════
//  「連我的資料一起刪」
// ══════════════════════════════════════════════════════════════════
//
// 使用者的要求:真的不用了的人,要能一次清乾淨,不必自己去翻 AppData。
//
// ⚠ 這是整個安裝程式裡**唯一一個不可回復**的動作,所以每一條規則都往
//   「寧可少刪」那一邊倒:
//
//   1. **預設保留。** 對話框的預設按鈕是「否」(MB_DEFBUTTON2),
//      而且被 /SUPPRESSMSGBOXES 壓掉時的答案也是 IDNO。
//      也就是說「什麼都不做」是每一條路徑的預設。
//   2. **靜默解除安裝永遠不刪**,除非明確傳 /PURGEUSERDATA。
//      這一條不是潔癖:windows/verify_installer.sh 第 9 步斷言
//      「解除安裝後 %APPDATA% 底下的檔案還在」,那道關卡守的是
//      「移除輸入法不會順手毀掉使用者的東西」。靜默模式若預設會刪,
//      那道斷言就會開始紅 —— 而最糟的情況是有人為了讓它變綠去改斷言。
//   3. **路徑向產品要,不自己拼。** GUserDataDir 來自
//      `rime_ime_setup.exe user-data-path`。這裡若寫死
//      {userappdata}\<資料夾名>,產品改名時就會漏掉這一處,
//      結果是「刪了一個不存在的資料夾然後回報成功」。
//   4. **在檔案還沒被刪掉之前就把路徑問出來。** usPostUninstall 的時候
//      {app}\rime_ime_setup.exe 已經不在了,問不到任何東西。
var
  GUserDataDir: String;
  GPurge: Boolean;
  GPurgeOk: Boolean;

// ⚠ 兩種找法都做。
//
// Inno 的解除安裝程式會**把自己複製到暫存目錄再跑一次**(_iu*.tmp),
// 而那一次的參數列是它自己組出來的(會多一個 /SECONDPHASE=...)。
// 只看 ParamStr 的話,旗標有可能在第二階段就不見了 —— 而症狀是
// 「帶了 /PURGEUSERDATA 卻沒有刪」,看起來像刪除功能壞掉。
// GetCmdTail 拿的是原始命令列字串,兩種都查一次是零成本的保險。
function CmdLineParamExists(const Value: String): Boolean;
var
  I: Integer;
begin
  Result := False;
  for I := 1 to ParamCount do
    if CompareText(ParamStr(I), Value) = 0 then begin
      Result := True;
      Exit;
    end;
  if Pos(Uppercase(Value), Uppercase(GetCmdTail)) > 0 then
    Result := True;
end;

// 問產品自己:使用者資料目錄在哪裡。
// 走 ExecAsOriginalUser 優先 —— %APPDATA% 是**登入者的**,而解除安裝程式
// 可能跑在另一個(提權的)帳號底下。拿不到就退回提權那一側。
function QueryUserDataDir(Exe: String): String;
var
  Tmp: String;
  Rc: Integer;
  Lines: TArrayOfString;
begin
  Result := '';
  Tmp := ExpandConstant('{tmp}\rimequad-userdir.txt');
  // cmd /C 才能做輸出重導向。Exec 本身不會解析 > 。
  try
    if not ExecAsOriginalUser(ExpandConstant('{cmd}'),
         '/C ""' + Exe + '" user-data-path > "' + Tmp + '""',
         '', SW_HIDE, ewWaitUntilTerminated, Rc) then
      Rc := -1;
  except
    Rc := -1;
  end;
  if (Rc <> 0) or (not FileExists(Tmp)) then
    Exec(ExpandConstant('{cmd}'),
         '/C ""' + Exe + '" user-data-path > "' + Tmp + '""',
         '', SW_HIDE, ewWaitUntilTerminated, Rc);
  if FileExists(Tmp) and LoadStringsFromFile(Tmp, Lines) and
     (GetArrayLength(Lines) > 0) then
    Result := Trim(Lines[0]);
  DeleteFile(Tmp);
end;

procedure DecidePurge;
begin
  GPurge := CmdLineParamExists('/PURGEUSERDATA');
  if GPurge then begin
    Log('RimeQuad: /PURGEUSERDATA —— 明確要求刪除使用者資料');
    Exit;
  end;
  // 靜默解除安裝時**不問也不刪**。沒有旗標就是不刪。
  if UninstallSilent then Exit;
  if (GUserDataDir = '') or (not DirExists(GUserDataDir)) then Exit;
  // MB_DEFBUTTON2:預設按鈕是「否」。
  // 最後那個 IDNO 是 /SUPPRESSMSGBOXES 之下採用的答案 —— 同樣是不刪。
  GPurge := SuppressibleMsgBox(
              FmtMessage(CustomMessage('UninstallAskPurge'), [GUserDataDir]),
              mbConfirmation, MB_YESNO or MB_DEFBUTTON2, IDNO) = IDYES;
  if GPurge then Log('RimeQuad: 使用者選擇「連資料一起刪」');
end;

procedure DoPurge(Exe: String);
var
  Rc: Integer;
begin
  GPurgeOk := False;
  if GUserDataDir = '' then begin
    Log('RimeQuad: 算不出使用者資料目錄,不刪');
    Exit;
  end;
  // ⚠ 身分很重要:要刪的是**登入者的** %APPDATA%。
  //   ExecAsOriginalUser 是正確的那一條;它在沒有互動式 shell 的環境
  //   (例如 CI runner)上拿不到權杖,那時才退回提權那一側 ——
  //   而提權那一側刪的是執行解除安裝那個帳號的資料夾,對「自己就是
  //   管理員」的一般情況正好也是對的。
  Rc := -1;
  try
    if not ExecAsOriginalUser(Exe,
         'purge-user-data --yes-delete-my-dictionary', '', SW_HIDE,
         ewWaitUntilTerminated, Rc) then
      Rc := -1;
  except
    Rc := -1;
  end;
  if Rc <> 0 then begin
    Log('RimeQuad: purge(登入者)rc=' + IntToStr(Rc) + ',改走提權那一條');
    if not Exec(Exe, 'purge-user-data --yes-delete-my-dictionary', '',
                SW_HIDE, ewWaitUntilTerminated, Rc) then
      Rc := -1;
  end;
  Log('RimeQuad: purge-user-data rc=' + IntToStr(Rc));
  GPurgeOk := (Rc = 0) and (not DirExists(GUserDataDir));
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then begin
    // 順序是刻意的:
    //   1. 先把路徑問出來(這時 {app}\rime_ime_setup.exe 還在)
    //   2. 再問使用者要不要刪(問的時候路徑已經是真的,能顯示給他看)
    //   3. 停服務 / 反註冊(UninstallCleanup)—— 服務還握著詞庫時刪不掉
    //   4. 最後才刪資料
    GUserDataDir := QueryUserDataDir(ExpandConstant('{app}\' + SetupExeName));
    Log('RimeQuad: 使用者資料目錄 = ' + GUserDataDir);
    DecidePurge;
    UninstallCleanup;
    if GPurge then DoPurge(ExpandConstant('{app}\' + SetupExeName));
    Exit;
  end;
  if CurUninstallStep <> usPostUninstall then Exit;
  if UninstallSilent then Exit;
  if GPurge then begin
    if GPurgeOk then
      SuppressibleMsgBox(FmtMessage(CustomMessage('UninstallPurgeDone'),
                                    [GUserDataDir]), mbInformation, MB_OK, IDOK)
    else
      SuppressibleMsgBox(FmtMessage(CustomMessage('UninstallPurgeFailed'),
                                    [GUserDataDir]), mbInformation, MB_OK, IDOK);
    Exit;
  end;
  // 明著告訴使用者他的詞典還在、在哪裡。悄悄留下一個資料夾跟悄悄刪掉一樣糟。
  if (GUserDataDir <> '') and DirExists(GUserDataDir) then
    SuppressibleMsgBox(FmtMessage(CustomMessage('UninstallKeptUserData'),
                                  [GUserDataDir]), mbInformation, MB_OK, IDOK);
end;
