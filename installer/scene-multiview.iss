; Inno Setup script for Scene Multiview Dock (OBS Studio plugin, Windows x64)
; Built by GitHub Actions: ISCC /DAppVersion=... /DStageDir=<folder containing bin\ and data\> ...

#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#ifndef StageDir
  #define StageDir "..\release\stage"
#endif

[Setup]
AppId={{B7C2D6A4-5F0E-4E5B-9B53-6A1D2C8F9E31}
AppName=Scene Multiview Dock for OBS
AppVersion={#AppVersion}
AppPublisher=CLT
; OBS loads plugins from %ProgramData%\obs-studio\plugins\<name>\bin\64bit
DefaultDirName={commonappdata}\obs-studio\plugins\scene-multiview
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputBaseFilename=scene-multiview-{#AppVersion}-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName=Scene Multiview Dock for OBS
CloseApplications=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Code]
function IsObsRunning: Boolean;
var
  ResultCode: Integer;
begin
  Result := False;
  if Exec(ExpandConstant('{cmd}'), '/C tasklist /FI "IMAGENAME eq obs64.exe" | find /I "obs64.exe"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
    Result := (ResultCode = 0);
end;

function InitializeSetup: Boolean;
begin
  Result := True;
  while IsObsRunning do
  begin
    if MsgBox('OBS Studio is running. Please close OBS, then click Retry.', mbConfirmation, MB_RETRYCANCEL) = IDCANCEL then
    begin
      Result := False;
      Exit;
    end;
  end;
end;
