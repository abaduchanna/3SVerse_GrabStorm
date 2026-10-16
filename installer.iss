; TurboGrab setup — Inno Setup 6
; PrivilegesRequired=lowest -> per-user install into %LOCALAPPDATA%, no UAC
#define MyAppName "TurboGrab"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "3S Verse"
#define MyAppExeName "TurboGrab.exe"

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

[Files]
Source: "dist\TurboGrab.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "dist\LICENSE.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "dist\README.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; partial .part files in the user's save-to folders are kept on purpose
Type: files; Name: "{app}\*.part"
