; TurboGrab setup — Inno Setup 6
; PrivilegesRequired=lowest -> per-user install into %LOCALAPPDATA%, no UAC
; v1.2.0: browser extension auto-install — after install, the TurboGrab
; Integration extension is force-installed into every Chromium browser and
; Firefox found on the machine (HKCU policies, per-user, no admin needed).
; Self-hosted update source: https://abaduchanna.github.io/3SVerse_TurboGrab/
#define MyAppName "TurboGrab"
#define MyAppVersion "1.2.0"
#define MyAppPublisher "3S Verse"
#define MyAppExeName "TurboGrab.exe"

; --- extension auto-install constants --------------------------------------
#define ExtId "ooclbgcgolihefahehfjjcmlfdlhncno"
#define ExtUpdateUrl "https://abaduchanna.github.io/3SVerse_TurboGrab/update.xml"
#define ExtSources "https://abaduchanna.github.io/*"
#define ExtXpiUrl "https://abaduchanna.github.io/3SVerse_TurboGrab/TurboGrab.xpi"

[Setup]
AppId={{4B8D2F6A-9C31-4E57-A8D2-6F1B3C9E5A70}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} v{#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL=https://3sverse.com
AppSupportURL=https://github.com/abaduchanna/3SVerse_TurboGrab
DefaultDirName={localappdata}\3SVerse\TurboGrab
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
LicenseFile=LICENSE
OutputDir=Output
OutputBaseFilename=TurboGrab_v{#MyAppVersion}_Setup
SetupIconFile=src\icon.ico
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=lowest
UninstallDisplayIcon={app}\{#MyAppExeName}
CloseApplications=yes

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "browserinteg"; Description: "Auto-install the TurboGrab extension into all installed browsers (Chrome, Edge, Brave, Opera, Vivaldi, Yandex, Firefox)"; GroupDescription: "Browser integration:"; Flags: checkedonce

[Files]
Source: "dist\TurboGrab.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "dist\LICENSE.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "dist\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "Output\TurboGrab_Extension_Chrome.zip"; DestDir: "{app}\extensions"; Flags: ignoreversion
Source: "Output\TurboGrab_Extension_Firefox.zip"; DestDir: "{app}\extensions"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; partial .part files in the user's save-to folders are kept on purpose
Type: files; Name: "{app}\*.part"

[Code]
const
  ExtId       = '{#ExtId}';
  ExtUpdUrl   = '{#ExtUpdateUrl}';
  ExtSrcUrl   = '{#ExtSources}';
  ExtXpiUrl   = '{#ExtXpiUrl}';

// ---------------------------------------------------------------- detection
// 32-bit setup process: HKCU is NOT subject to WOW64 redirection, but
// HKLM\Software is (we read Wow6432Node transparently). So for system-wide
// browsers we additionally probe the well-known exe paths directly.

function ChromePresent(): Boolean;
begin
  Result :=
    RegKeyExists(HKEY_CURRENT_USER, 'Software\Google\Chrome') or
    RegKeyExists(HKEY_LOCAL_MACHINE, 'Software\Google\Chrome') or
    RegKeyExists(HKEY_LOCAL_MACHINE, 'Software\Wow6432Node\Google\Chrome') or
    FileExists('C:\Program Files\Google\Chrome\Application\chrome.exe') or
    FileExists('C:\Program Files (x86)\Google\Chrome\Application\chrome.exe') or
    FileExists(ExpandConstant('{localappdata}\Google\Chrome\Application\chrome.exe'));
end;

function EdgePresent(): Boolean;
begin
  Result :=
    RegKeyExists(HKEY_CURRENT_USER, 'Software\Microsoft\Edge') or
    FileExists('C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe') or
    FileExists('C:\Program Files\Microsoft\Edge\Application\msedge.exe');
end;

function BravePresent(): Boolean;
begin
  Result :=
    RegKeyExists(HKEY_CURRENT_USER, 'Software\BraveSoftware\Brave') or
    FileExists('C:\Program Files\BraveSoftware\Brave-Browser\Application\brave.exe') or
    FileExists('C:\Program Files (x86)\BraveSoftware\Brave-Browser\Application\brave.exe') or
    FileExists(ExpandConstant('{localappdata}\BraveSoftware\Brave-Browser\Application\brave.exe'));
end;

function VivaldiPresent(): Boolean;
begin
  Result :=
    RegKeyExists(HKEY_CURRENT_USER, 'Software\Vivaldi') or
    FileExists('C:\Program Files\Vivaldi\Application\vivaldi.exe') or
    FileExists('C:\Program Files (x86)\Vivaldi\Application\vivaldi.exe') or
    FileExists(ExpandConstant('{localappdata}\Vivaldi\Application\vivaldi.exe'));
end;

function OperaPresent(): Boolean;
begin
  Result :=
    RegKeyExists(HKEY_CURRENT_USER, 'Software\Opera Software') or
    FileExists('C:\Program Files\Opera\opera.exe') or
    FileExists('C:\Program Files (x86)\Opera\opera.exe') or
    FileExists(ExpandConstant('{localappdata}\Programs\Opera\opera.exe'));
end;

function YandexPresent(): Boolean;
begin
  Result :=
    RegKeyExists(HKEY_CURRENT_USER, 'Software\Yandex\YandexBrowser') or
    FileExists(ExpandConstant('{localappdata}\Yandex\YandexBrowser\Application\browser.exe'));
end;

function FirefoxPresent(): Boolean;
begin
  Result :=
    RegKeyExists(HKEY_CURRENT_USER, 'Software\Mozilla\Mozilla Firefox') or
    RegKeyExists(HKEY_LOCAL_MACHINE, 'Software\Mozilla\Mozilla Firefox') or
    FileExists('C:\Program Files\Mozilla Firefox\firefox.exe') or
    FileExists('C:\Program Files (x86)\Mozilla Firefox\firefox.exe');
end;

// ------------------------------------------------------------ policy writes

procedure WriteChromiumPolicy(const PolicyRoot: String);
begin
  RegWriteStringValue(HKEY_CURRENT_USER,
    'Software\Policies\' + PolicyRoot + '\ExtensionInstallForcelist',
    '1', ExtId + ';' + ExtUpdUrl);
  RegWriteStringValue(HKEY_CURRENT_USER,
    'Software\Policies\' + PolicyRoot + '\ExtensionInstallSources',
    '1', ExtSrcUrl);
end;

procedure RemoveChromiumPolicy(const PolicyRoot: String);
var
  FL, FS, V: String;
begin
  FL := 'Software\Policies\' + PolicyRoot + '\ExtensionInstallForcelist';
  FS := 'Software\Policies\' + PolicyRoot + '\ExtensionInstallSources';
  // only delete values we own ("1"); foreign enterprise entries (2,3,...) stay
  if RegQueryStringValue(HKEY_CURRENT_USER, FL, '1', V) and (V = ExtId + ';' + ExtUpdUrl) then
  begin
    RegDeleteValue(HKEY_CURRENT_USER, FL, '1');
    RegDeleteKeyIfEmpty(HKEY_CURRENT_USER, FL);
    RegDeleteKeyIfEmpty(HKEY_CURRENT_USER, 'Software\Policies\' + PolicyRoot);
  end;
  if RegQueryStringValue(HKEY_CURRENT_USER, FS, '1', V) and (V = ExtSrcUrl) then
  begin
    RegDeleteValue(HKEY_CURRENT_USER, FS, '1');
    RegDeleteKeyIfEmpty(HKEY_CURRENT_USER, FS);
  end;
end;

procedure InstallBrowserIntegration();
var
  N: Integer;
begin
  N := 0;
  if ChromePresent() then begin WriteChromiumPolicy('Google\Chrome'); N := N + 1; end;
  if EdgePresent() then begin WriteChromiumPolicy('Microsoft\Edge'); N := N + 1; end;
  if BravePresent() then begin WriteChromiumPolicy('BraveSoftware\Brave'); N := N + 1; end;
  if VivaldiPresent() then begin WriteChromiumPolicy('Vivaldi'); N := N + 1; end;
  if OperaPresent() then begin WriteChromiumPolicy('Opera Software\Opera'); N := N + 1; end;
  if YandexPresent() then begin WriteChromiumPolicy('Yandex\YandexBrowser'); N := N + 1; end;
  if FirefoxPresent() then
  begin
    // Firefox reads the same Chromium-style policy tree from HKCU.
    // NOTE: release-channel Firefox still enforces add-on signing for remote
    // XPIs; ESR/Dev/Nightly honor this. Harmless no-op otherwise.
    RegWriteStringValue(HKEY_CURRENT_USER,
      'Software\Policies\Mozilla\Firefox\Extensions\Install',
      '1', ExtXpiUrl);
    N := N + 1;
  end;
  RegWriteStringValue(HKEY_CURRENT_USER, 'Software\3SVerse\TurboGrab',
    'ExtensionIntegration', IntToStr(N) + ' browsers configured');
  RegWriteStringValue(HKEY_CURRENT_USER, 'Software\3SVerse\TurboGrab',
    'ExtensionId', ExtId);
end;

procedure RemoveBrowserIntegration();
var
  V: String;
begin
  RemoveChromiumPolicy('Google\Chrome');
  RemoveChromiumPolicy('Microsoft\Edge');
  RemoveChromiumPolicy('BraveSoftware\Brave');
  RemoveChromiumPolicy('Vivaldi');
  RemoveChromiumPolicy('Opera Software\Opera');
  RemoveChromiumPolicy('Yandex\YandexBrowser');
  begin
    if RegQueryStringValue(HKEY_CURRENT_USER,
         'Software\Policies\Mozilla\Firefox\Extensions\Install', '1', V) and
       (V = ExtXpiUrl) then
    begin
      RegDeleteValue(HKEY_CURRENT_USER,
        'Software\Policies\Mozilla\Firefox\Extensions\Install', '1');
      RegDeleteKeyIfEmpty(HKEY_CURRENT_USER,
        'Software\Policies\Mozilla\Firefox\Extensions\Install');
      RegDeleteKeyIfEmpty(HKEY_CURRENT_USER,
        'Software\Policies\Mozilla\Firefox\Extensions');
      RegDeleteKeyIfEmpty(HKEY_CURRENT_USER,
        'Software\Policies\Mozilla\Firefox');
    end;
  end;
  RegDeleteValue(HKEY_CURRENT_USER, 'Software\3SVerse\TurboGrab', 'ExtensionIntegration');
  RegDeleteValue(HKEY_CURRENT_USER, 'Software\3SVerse\TurboGrab', 'ExtensionId');
end;

// -------------------------------------------------------------------- hooks

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    if IsTaskSelected('browserinteg') then
      InstallBrowserIntegration();
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
    RemoveBrowserIntegration();
end;
