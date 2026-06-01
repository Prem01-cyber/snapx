; snapx Windows Installer Script — Inno Setup 6
; Build: iscc installer.iss

#define MyAppName      "snapx"
; Allow the version to be supplied on the command line (iscc /DMyAppVersion=…);
; the literal below is only the local-build fallback.
#ifndef MyAppVersion
  #define MyAppVersion "2.0.2"
#endif
#define MyAppPublisher "snapx Team"
#define MyAppURL       "https://github.com/Prem01-cyber/snapx"
#define MyAppExeName   "snapx.exe"
#define MyBuildDir     "..\..\build"

[Setup]
AppId={{8F9A1234-ABCD-4321-EFGH-000000000001}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}/issues
AppUpdatesURL={#MyAppURL}/releases
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
AllowNoIcons=yes
LicenseFile=..\..\LICENSE
InfoAfterFile=..\..\README.md
OutputDir=output
OutputBaseFilename=snapx-{#MyAppVersion}-win64-setup
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog
UninstallDisplayIcon={app}\{#MyAppExeName}
ArchitecturesInstallIn64BitMode=x64
MinVersion=10.0

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon";    Description: "{cm:CreateDesktopIcon}";    GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "quicklaunchicon"; Description: "{cm:CreateQuickLaunchIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked; OnlyBelowVersion: 6.1; Check: not IsAdminInstallMode

[Files]
Source: "{#MyBuildDir}\{#MyAppExeName}";   DestDir: "{app}"; Flags: ignoreversion
; GTK runtime DLLs (bundled flat into build/ via ldd in build-installer.sh).
; No recursesubdirs here — loader DLLs under lib\ are installed by the lib\ rule.
Source: "{#MyBuildDir}\*.dll";             DestDir: "{app}"; Flags: ignoreversion
Source: "{#MyBuildDir}\lib\*";             DestDir: "{app}\lib"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "{#MyBuildDir}\share\*";           DestDir: "{app}\share"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "..\..\resources\icons\hicolor\256x256\apps\snapx.png"; DestDir: "{app}"; DestName: "snapx.png"; Flags: ignoreversion
Source: "..\..\LICENSE";                   DestDir: "{app}"; Flags: ignoreversion
Source: "..\..\README.md";                 DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}";                                       Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}";                 Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}";                                  Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon
Name: "{userappdata}\Microsoft\Internet Explorer\Quick Launch\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: quicklaunchicon

[Registry]
; Register for Print Screen shell integration
Root: HKCU; Subkey: "Software\{#MyAppName}"; ValueType: string; ValueName: "InstallPath"; ValueData: "{app}"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\{#MyAppName}"; ValueType: string; ValueName: "Version"; ValueData: "{#MyAppVersion}"

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

[Code]
function InitializeSetup(): Boolean;
begin
  Result := True;
end;
