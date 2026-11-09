; Per-user installer for IslandBridge. Built by installer\build.ps1, which passes
; AppVersion, SourceDir (the publish folder) and OutputDir.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\artifacts\publish"
#endif
#ifndef OutputDir
  #define OutputDir "..\artifacts\installer"
#endif

#define AppName "IslandBridge"
#define AppExe "IslandBridge.exe"
#define IslandExe "IslandBridge.Island.exe"
#define UpdaterExe "IslandBridge.Updater.exe"

[Setup]
AppId={{6A4F1C2E-8B3D-4F7A-9E21-5C0D7B9A3F48}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=1Filipek2
AppPublisherURL=https://github.com/1Filipek2/island-bridge
AppSupportURL=https://github.com/1Filipek2/island-bridge/issues
AppUpdatesURL=https://github.com/1Filipek2/island-bridge/releases
VersionInfoVersion={#AppVersion}
DefaultDirName={userpf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
DisableDirPage=auto
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.22000
OutputDir={#OutputDir}
OutputBaseFilename={#AppName}-{#AppVersion}-setup-x64
SetupIconFile=..\src\IslandBridge\Assets\IslandBridge.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
LicenseFile=..\LICENSE
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
CloseApplications=no
ShowLanguageDialog=auto

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"
Name: "sk"; MessagesFile: "compiler:Languages\Slovak.isl"

[CustomMessages]
en.DeleteData=Also delete IslandBridge settings, the paired iPhone and logs?
sk.DeleteData=Odstrániť aj nastavenia IslandBridge, spárovaný iPhone a logy?
en.LaunchApp=Start IslandBridge
sk.LaunchApp=Spustiť IslandBridge

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Excludes: "*.pdb"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\LICENSE"; DestDir: "{app}\licenses"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "..\THIRD-PARTY-NOTICES.txt"; DestDir: "{app}\licenses"; Flags: ignoreversion
Source: "{#SourceDir}\..\licenses\*"; DestDir: "{app}\licenses"; Flags: ignoreversion skipifsourcedoesntexist

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchApp}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{sys}\taskkill.exe"; Parameters: "/F /IM {#AppExe} /IM {#IslandExe} /IM {#UpdaterExe}"; Flags: runhidden; RunOnceId: "StopIslandBridge"

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: none; ValueName: "{#AppName}"; Flags: uninsdeletevalue dontcreatekey
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run"; ValueType: none; ValueName: "{#AppName}"; Flags: uninsdeletevalue dontcreatekey

[Code]
// The app, the island it supervises and the updater must be gone before files are replaced.
procedure StopRunningApp();
var
  ResultCode: Integer;
begin
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM {#AppExe} /IM {#IslandExe} /IM {#UpdaterExe}', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Sleep(500);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  StopRunningApp();
  Result := '';
end;

// The in-app updater runs setup silently with /relaunch=1. The app starts again whether the
// install succeeded or not, so a failed update never leaves the user without IslandBridge.
procedure DeinitializeSetup();
var
  ResultCode: Integer;
  AppPath: String;
begin
  if WizardSilent and (ExpandConstant('{param:relaunch|0}') = '1') then
  begin
    try
      AppPath := ExpandConstant('{app}\{#AppExe}');
    except
      AppPath := '';
    end;
    if (AppPath <> '') and FileExists(AppPath) then
      Exec(AppPath, '', ExtractFileDir(AppPath), SW_SHOWNORMAL, ewNoWait, ResultCode);
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if (CurUninstallStep = usPostUninstall) and not UninstallSilent then
  begin
    if MsgBox(CustomMessage('DeleteData'), mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
    begin
      DelTree(ExpandConstant('{localappdata}\IslandBridge'), True, True, True);
      RegDeleteKeyIncludingSubkeys(HKEY_CURRENT_USER, 'Software\IslandBridge');
    end;
  end;
end;
