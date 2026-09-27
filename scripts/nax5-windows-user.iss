; NAX5 product installer. Separate AppId from upstream chiaki-ng
; (scripts/chiaki-ng.iss) so the two can coexist.
;
; Compile (ISCC 6):
;   ISCC.exe /DMyAppPath=<portable NAX5-user dir> /DMyOutputDir=<out dir> scripts\nax5-windows-user.iss

#ifndef MyAppPath
  #error Define MyAppPath (absolute path to the deployed NAX5-user tree)
#endif
#ifndef MyOutputDir
  #define MyOutputDir "."
#endif
#ifndef MyOutputBase
  #define MyOutputBase "NAX5-windows-installer"
#endif

#define MyAppName "NAX5"
#define MyAppPublisher "NAX5"
#define MyAppURL "https://cloudgta6.com"
#define MyAppExeName "chiaki.exe"
#define MyAppExe MyAppPath + "\" + MyAppExeName
#define MyAppVersion() \
  GetVersionComponents(MyAppExe, Local[0], Local[1], Local[2], Local[3]), \
  Str(Local[0]) + "." + Str(Local[1]) + "." + Str(Local[2])

[Setup]
AppId={{A1D5E8B2-4C73-4F90-8E16-9B2A5C7D4E01}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
DefaultDirName={localappdata}\Programs\{#MyAppName}
DefaultGroupName={#MyAppName}
; Qt 6 (bundled 6.11) runs on Windows 10 1809 (build 17763) and later.
MinVersion=10.0.17763
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
DisableProgramGroupPage=yes
DisableDirPage=no
AllowNoIcons=yes
LicenseFile=..\LICENSES\AGPL-3.0-only-OpenSSL.txt
SetupIconFile=..\gui\nax5.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
OutputBaseFilename={#MyOutputBase}
OutputDir={#MyOutputDir}
Compression=lzma
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#MyAppPath}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; Tasks: desktopicon

[Registry]
; Windows Error Reporting local dumps: a backup for crashes the in-app handler
; cannot catch (e.g. a crash before it is installed). HKLM only, so this is
; written only when the user chose "install for all users" (admin mode).
; The client uploads these files from the same folder on the next start.
Root: HKLM; Subkey: "SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\{#MyAppExeName}"; Flags: uninsdeletekey; Check: IsAdminInstallMode
Root: HKLM; Subkey: "SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\{#MyAppExeName}"; ValueType: expandsz; ValueName: "DumpFolder"; ValueData: "%APPDATA%\NAX5\NAX5\log\crash-dumps"; Check: IsAdminInstallMode
Root: HKLM; Subkey: "SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\{#MyAppExeName}"; ValueType: dword; ValueName: "DumpType"; ValueData: "1"; Check: IsAdminInstallMode
Root: HKLM; Subkey: "SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\{#MyAppExeName}"; ValueType: dword; ValueName: "DumpCount"; ValueData: "5"; Check: IsAdminInstallMode

[Run]
Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent
