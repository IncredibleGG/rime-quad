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
; 檔案被佔用的情況由下面 [Files] 的 restartreplace 承接。
CloseApplications=no
SetupMutex=RimeQuadSetupMutex

; 安裝完成頁不推銷任何東西,也不建捷徑(沒有可以按的 GUI 程式)。
DisableProgramGroupPage=yes

; 歡迎頁要顯示。Inno 6 的 modern 樣式**預設把它藏起來**,但這裡它有實際用途:
; 使用者裝完之後還得自己去切輸入法,那件事得先講。
DisableWelcomePage=no

; 安裝位置固定,不讓使用者選。
; 服務進程靠「與執行檔同目錄的 data\shared」找執行期資料,而登錄檔裡存的是
; 絕對路徑 —— 使用者事後搬動資料夾就會壞,而症狀是「輸入法忽然不見了」。
; 少一個頁面,也少一種弄壞它的方法。
DisableDirPage=yes

[Files]
; restartreplace + uninsrestartdelete:
;   rime_tsf.dll 在升級的當下可能正被宿主進程載入著而換不掉。
;   這兩個旗標讓它排到下次開機再換 / 再刪,而不是讓整個安裝失敗。
Source: "{#PayloadDir}\rime_tsf.dll";       DestDir: "{app}"; Flags: ignoreversion restartreplace uninsrestartdelete
Source: "{#PayloadDir}\rime_service.exe";   DestDir: "{app}"; Flags: ignoreversion restartreplace uninsrestartdelete
Source: "{#PayloadDir}\rime_ime_setup.exe"; DestDir: "{app}"; Flags: ignoreversion restartreplace uninsrestartdelete

; ⚠ 執行期資料。**少了這一段,前面每一步都會成功,服務起得來、輸入法
;   註冊得上,然後一個字都打不出來,而且沒有任何錯誤訊息。**
;   data\shared 是方案、詞庫與 opencc 詞典;data\user 是首次執行要補進
;   %APPDATA%\RimeQuad 的範本(default.custom.yaml 把 schema_list 限縮成
;   我們真的有詞庫的四個方案)。
;   make_installer.sh 在編譯之前會逐項點名檢查這棵樹,缺了就不出安裝程式。
Source: "{#PayloadDir}\data\*"; DestDir: "{app}\data"; Flags: ignoreversion recursesubdirs createallsubdirs

[UninstallRun]
; 順序有意義,而且 [UninstallRun] 是在**刪檔案之前**跑的 —— 這幾支都住在
; 即將被刪掉的目錄裡。
;
; runasoriginaluser:解除安裝是提權跑的,但服務是使用者自己那一支,
; 而 HKCU 也是使用者自己的。用提權的權杖去做這兩件事,停到的是不存在的
; 服務、清到的是管理員帳號的登錄檔,而真正在用電腦的那個人什麼都沒被清。
Filename: "{app}\rime_ime_setup.exe"; Parameters: "stop-service --dir ""{app}"""; RunOnceId: "RimeStopService"; Flags: runhidden waituntilterminated runasoriginaluser
Filename: "{app}\rime_ime_setup.exe"; Parameters: "disable-user";  RunOnceId: "RimeDisableUser";  Flags: runhidden waituntilterminated runasoriginaluser
Filename: "{app}\rime_ime_setup.exe"; Parameters: "unregister";    RunOnceId: "RimeUnregister";   Flags: runhidden waituntilterminated

; ⚠ 沒有 [UninstallDelete] 去碰 %APPDATA%\RimeQuad,而且**不可以加**。
;   那裡是使用者的詞典、自訂短語與設定 —— 是使用者的資料,不是我們的檔案。
;   解除安裝一律留下;重新安裝時使用者的詞會原封不動地回來。
;   (CI 有一道斷言:解除安裝之後 %APPDATA%\RimeQuad 必須還在而且非空。)

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
FinishedLabel=[name] 已安裝在您的電腦上。%n%n用法:按 Win + 空白鍵切換輸入法,選擇「RIME 四端輸入法」。%n%n若清單裡沒有出現,請到「設定 → 時間與語言 → 語言與地區」把「中文(繁體,台灣)」加入語言清單,再試一次。%n%n首次使用時輸入法會在背景編譯詞庫,可能需要一到數分鐘;在那之前打出來的是英文,這是正常的。
ExitSetupTitle=結束安裝
ExitSetupMessage=安裝尚未完成。現在結束的話,本輸入法不會被安裝。%n%n要結束安裝嗎?
ConfirmUninstall=您確定要完整移除 %1 嗎?
UninstallStatusLabel=請稍候,正在從您的電腦移除 %1…
StatusUninstalling=正在移除 %1…
UninstalledAll=%1 已從您的電腦順利移除。
UninstalledMost=%1 移除完成。%n%n有部分項目無法移除,重新啟動電腦後會自動清除。
SetupIsAlreadyRunning=安裝程式已經在執行中。
WindowsVersionNotSupported=本輸入法需要 Windows 10 1607 或更新的版本。
OnlyOnTheseArchitectures=本輸入法只提供 64 位元版本,無法安裝在這台電腦上。

[CustomMessages]
RegisterFailed=註冊輸入法失敗(rime_ime_setup.exe %1,結束碼 %2)。%n%n輸入法沒有被系統接受,現在就算裝完了也不會出現在輸入法清單上。%n安裝已中止,不會留下一個裝了卻用不了的狀態。
UninstallKeptUserData=您的詞典與設定保留在:%n%n%1%n%n那是您自己的資料(學過的詞、自訂短語),移除輸入法時刻意不刪。%n重新安裝時會原封不動地回來;確定不要了再自行刪除該資料夾。

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

  // 註冊完立刻自我檢查一次。register 回傳成功只代表那幾個 API 沒有報錯;
  // check 才會去確認登錄檔真的長出東西、而且 TSF 的列舉 API 看得到我們。
  RunSetupVerbOrFail('check');

  // 使用者那一側。**失敗不中止安裝**:輸入法已經全機註冊好了,使用者仍然
  // 可以自己從「設定」把它加進來。為了這一步讓整個安裝回滾是不成比例的。
  ExecAsOriginalUser(Exe, 'enable-user', '', SW_HIDE, ewWaitUntilTerminated, Rc);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  UserDir: String;
begin
  if CurUninstallStep <> usPostUninstall then Exit;
  // 明著告訴使用者他的詞典還在、在哪裡。悄悄留下一個資料夾跟悄悄刪掉一樣糟。
  UserDir := ExpandConstant('{userappdata}\RimeQuad');
  if DirExists(UserDir) and (not UninstallSilent) then
    SuppressibleMsgBox(FmtMessage(CustomMessage('UninstallKeptUserData'), [UserDir]),
                       mbInformation, MB_OK, IDOK);
end;
