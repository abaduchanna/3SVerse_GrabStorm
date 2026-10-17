; GrabStorm v1.3.0 — 3SVerse multi-segment download accelerator for Windows
; Per-user installer (no admin). Auto-installs the GrabStorm Integration
; browser extension into current and future supported Chromium browsers +
; Firefox via persistent per-user policies, registers the grabstorm:// protocol, and writes a
; diagnostic log to %LOCALAPPDATA%\3SVerse\GrabStorm\install.log

#define AppName "GrabStorm"
#define AppVer "1.3.0"
#define ExtId "ooclbgcgolihefahehfjjcmlfdlhncno"
#define ExtUpdateUrl "https://abaduchanna.github.io/3SVerse_GrabStorm/update.xml"
#define ExtSources "https://abaduchanna.github.io/*"
#define FfXpiUrl "https://abaduchanna.github.io/3SVerse_GrabStorm/GrabStorm.xpi"

[Setup]
AppId={{6E2F1C44-9A83-4D17-B55B-9C1E2F3A4B5C}}
AppName={#AppName}
AppVersion={#AppVer}
AppPublisher=3SVerse
AppPublisherURL=https://www.3sverse.com
DefaultDirName={localappdata}\Programs\GrabStorm
DefaultGroupName=GrabStorm
UninstallDisplayIcon={app}\GrabStorm.exe
OutputDir=Output
OutputBaseFilename=GrabStorm_v{#AppVer}_Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=lowest
CloseApplications=no

[Files]
Source: "dist\GrabStorm.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "dist\LICENSE.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "dist\README.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\GrabStorm"; Filename: "{app}\GrabStorm.exe"
Name: "{userdesktop}\GrabStorm"; Filename: "{app}\GrabStorm.exe"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Additional icons:"
Name: "browserinteg"; Description: "Auto-install GrabStorm Integration in current and future supported browsers"; GroupDescription: "Browser integration:"; Flags: checkedonce

[Registry]
Root: HKCU; Subkey: "Software\Classes\grabstorm"; ValueType: string; ValueData: "URL:GrabStorm Protocol"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\grabstorm"; ValueType: string; ValueName: "URL Protocol"; ValueData: ""; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\grabstorm\DefaultIcon"; ValueType: string; ValueData: "{app}\GrabStorm.exe,0"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\grabstorm\shell\open\command"; ValueType: string; ValueData: """{app}\GrabStorm.exe"" ""%1"""; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\3SVerse\GrabStorm"; ValueType: string; ValueName: "AppPath"; ValueData: "{app}\GrabStorm.exe"; Flags: uninsdeletekey

[Run]
Filename: "{app}\GrabStorm.exe"; Description: "Launch GrabStorm"; Flags: nowait postinstall skipifsilent

[Code]
const
  ExtIdV = '{#ExtId}';
  ExtUpdateUrlV = '{#ExtUpdateUrl}';
  ExtSourcesV = '{#ExtSources}';
  FfXpiUrlV = '{#FfXpiUrl}';
  WM_QUIT_V = $0012;

var
  LogFile: String;
  // Chromium policy roots + their executable names (filled by InitBrowserArrays;
  // Inno Pascal Script does not support Delphi-style array constants)
  ChromiumRoots: array[0..6] of String;
  BrowserNames: array[0..6] of String;
  BrowserExes: array[0..6] of String;

procedure Sleep(ms: LongInt); external 'Sleep@kernel32.dll stdcall';

procedure InitBrowserArrays;
begin
  ChromiumRoots[0] := 'Google\Chrome';
  ChromiumRoots[1] := 'Microsoft\Edge';
  ChromiumRoots[2] := 'BraveSoftware\Brave';
  ChromiumRoots[3] := 'Vivaldi';
  ChromiumRoots[4] := 'Opera Software\Opera';
  ChromiumRoots[5] := 'Yandex\YandexBrowser';
  ChromiumRoots[6] := 'Chromium';
  BrowserNames[0] := 'Google Chrome';
  BrowserNames[1] := 'Microsoft Edge';
  BrowserNames[2] := 'Brave';
  BrowserNames[3] := 'Vivaldi';
  BrowserNames[4] := 'Opera';
  BrowserNames[5] := 'Yandex';
  BrowserNames[6] := 'Chromium';
  BrowserExes[0] := 'chrome.exe';
  BrowserExes[1] := 'msedge.exe';
  BrowserExes[2] := 'brave.exe';
  BrowserExes[3] := 'vivaldi.exe';
  BrowserExes[4] := 'opera.exe';
  BrowserExes[5] := 'yandex.exe';
  BrowserExes[6] := 'chrome.exe';
end;

procedure LogLine(const S: String);
var
  A: AnsiString;
begin
  A := S + #13#10;
  SaveStringToFile(LogFile, A, True);
end;

function DirHas(const dir, exe: String): Boolean;
begin
  Result := FileExists(AddBackslash(dir) + exe);
end;

// search the standard install roots of one browser; returns its Application dir
function DetectBrowser(idx: Integer): String;
var
  pf, pf32, lad, lap: String;
  rel: String;
begin
  Result := '';
  pf  := ExpandConstant('{pf}');
  pf32 := ExpandConstant('{pf32}');
  lad := ExpandConstant('{localappdata}');
  lap := lad + '\Programs';
  rel := '\' + ChromiumRoots[idx] + '\Application';

  if DirHas(pf  + rel, BrowserExes[idx]) then Result := pf  + rel
  else if DirHas(pf32 + rel, BrowserExes[idx]) then Result := pf32 + rel
  else if DirHas(lap + rel,  BrowserExes[idx]) then Result := lap + rel
  else if DirHas(lad + rel,  BrowserExes[idx]) then Result := lad + rel;
end;

procedure WriteChromiumPolicy(const Root: String);
begin
  RegWriteStringValue(HKEY_CURRENT_USER,
    'Software\Policies\' + Root + '\ExtensionInstallForcelist', '1',
    ExtIdV + ';' + ExtUpdateUrlV);
  RegWriteStringValue(HKEY_CURRENT_USER,
    'Software\Policies\' + Root + '\ExtensionInstallSources', '1',
    ExtSourcesV);
end;

procedure KillRunningApp;
var
  wnd: HWND;
begin
  wnd := FindWindowByClassName('3SVerseGrabStorm');
  if wnd <> 0 then
  begin
    LogLine('GrabStorm was running - asked it to close.');
    PostMessage(wnd, WM_QUIT_V, 0, 0);
    Sleep(900);
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  i, n, ff: Integer;
  b: String;
  chk: String;
begin
  if CurStep <> ssPostInstall then Exit;
  InitBrowserArrays;
  LogFile := ExpandConstant('{localappdata}') + '\3SVerse\GrabStorm\install.log';
  ForceDirectories(ExtractFilePath(LogFile));
  LogLine('=== GrabStorm install ' + GetDateTimeString('yyyy/mm/dd hh:nn:ss', '-', ':') + ' ===');

  KillRunningApp;

  if not IsTaskSelected('browserinteg') then
  begin
    LogLine('Browser integration task not selected - skipping extension policy install.');
    LogLine('=== done ===');
    Exit;
  end;

  n := 0;
  for i := 0 to 6 do
  begin
    b := DetectBrowser(i);
    { Provision every supported policy root, even when the browser is not yet
      installed. Chromium browsers read it on launch, so later installations
      receive the extension automatically without rerunning GrabStorm Setup. }
    WriteChromiumPolicy(ChromiumRoots[i]);
    if b <> '' then
    begin
      LogLine('Detected ' + BrowserNames[i] + ' at ' + b);
      n := n + 1;
    end
    else
      LogLine('Not currently installed: ' + BrowserNames[i] + ' (policy pre-provisioned for future install)');
    LogLine('  wrote HKCU\Software\Policies\' + ChromiumRoots[i] + '\ExtensionInstallForcelist');
  end;

  ff := 0;
  { Keep Firefox policy present even before Firefox is installed. }
  RegWriteStringValue(HKEY_CURRENT_USER,
    'Software\Policies\Mozilla\Firefox\Extensions\Install', '1', FfXpiUrlV);
  if DirHas(ExpandConstant('{pf}') + '\Mozilla Firefox', 'firefox.exe') or
     DirHas(ExpandConstant('{pf32}') + '\Mozilla Firefox', 'firefox.exe') or
     DirHas(ExpandConstant('{localappdata}') + '\Mozilla Firefox', 'firefox.exe') then
  begin
    LogLine('Detected Firefox - wrote Extensions\Install policy (ESR/Dev/Nightly only; release Firefox requires a signed add-on).');
    ff := 1;
  end
  else
    LogLine('Firefox not currently installed (policy pre-provisioned for future install).');

  LogLine('--- verification readback ---');
  for i := 0 to 6 do
  begin
    if RegQueryStringValue(HKEY_CURRENT_USER,
        'Software\Policies\' + ChromiumRoots[i] + '\ExtensionInstallForcelist', '1', chk) then
      LogLine('VERIFY ' + ChromiumRoots[i] + ': ' + chk)
    else
      LogLine('VERIFY ' + ChromiumRoots[i] + ': MISSING');
  end;

  LogLine(IntToStr(n) + ' Chromium browser(s) detected; 7 Chromium policy roots and Firefox policy provisioned. Firefox detected=' + IntToStr(ff) + '.');
  LogLine('IMPORTANT: restart every browser once so the policy-installed extension loads.');
  LogLine('=== done ===');
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  i: Integer;
  v: String;
begin
  if CurUninstallStep <> usPostUninstall then Exit;
  InitBrowserArrays;
  LogFile := ExpandConstant('{localappdata}') + '\3SVerse\GrabStorm\install.log';
  ForceDirectories(ExtractFilePath(LogFile));
  LogLine('=== GrabStorm uninstall ' + GetDateTimeString('yyyy/mm/dd hh:nn:ss', '-', ':') + ' ===');
  for i := 0 to 6 do
  begin
    if RegQueryStringValue(HKEY_CURRENT_USER,
        'Software\Policies\' + ChromiumRoots[i] + '\ExtensionInstallForcelist', '1', v) and
       (Pos(ExtIdV, v) > 0) then
    begin
      RegDeleteValue(HKEY_CURRENT_USER,
        'Software\Policies\' + ChromiumRoots[i] + '\ExtensionInstallForcelist', '1');
      RegDeleteValue(HKEY_CURRENT_USER,
        'Software\Policies\' + ChromiumRoots[i] + '\ExtensionInstallSources', '1');
      RegDeleteKeyIfEmpty(HKEY_CURRENT_USER,
        'Software\Policies\' + ChromiumRoots[i] + '\ExtensionInstallForcelist');
      RegDeleteKeyIfEmpty(HKEY_CURRENT_USER,
        'Software\Policies\' + ChromiumRoots[i] + '\ExtensionInstallSources');
      LogLine('Removed integration policy for ' + ChromiumRoots[i]);
    end;
  end;
  if RegQueryStringValue(HKEY_CURRENT_USER,
      'Software\Policies\Mozilla\Firefox\Extensions\Install', '1', v) and
     (Pos('GrabStorm.xpi', v) > 0) then
  begin
    RegDeleteValue(HKEY_CURRENT_USER, 'Software\Policies\Mozilla\Firefox\Extensions\Install', '1');
    RegDeleteKeyIfEmpty(HKEY_CURRENT_USER, 'Software\Policies\Mozilla\Firefox\Extensions\Install');
    RegDeleteKeyIfEmpty(HKEY_CURRENT_USER, 'Software\Policies\Mozilla\Firefox\Extensions');
    RegDeleteKeyIfEmpty(HKEY_CURRENT_USER, 'Software\Policies\Mozilla\Firefox');
    RegDeleteKeyIfEmpty(HKEY_CURRENT_USER, 'Software\Policies\Mozilla');
    LogLine('Removed Firefox integration policy.');
  end;
  LogLine('=== done ===');
end;
