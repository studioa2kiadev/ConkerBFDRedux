; Conker Redux installer (Inno Setup 6), built by tools/make_installer.sh, which stages the files
; and passes StageDir, OutputDir, AppVersion and IconFile. A GOG-style install: the program in
; its own folder (Program Files by default), Start menu and desktop shortcuts, an uninstaller.
; Settings, saves and the player's ROM are kept in %LOCALAPPDATA%\ConkerRedux, which the
; uninstaller leaves alone. No game data is included: the game asks for the player's own ROM.

#ifndef StageDir
  #error StageDir isn't defined: build this with tools/make_installer.sh
#endif

[Setup]
AppId={{724504A0-8B9C-4660-B2CD-5729323AE8BA}
AppName=Conker Redux
AppVersion={#AppVersion}
AppVerName=Conker Redux {#AppVersion}
AppPublisher=Conker Redux (fan project, not affiliated with Rare or Microsoft)
AppPublisherURL=https://github.com/studioa2kiadev/ConkerBFDRedux
DefaultDirName={autopf}\Conker Redux
DefaultGroupName=Conker Redux
DisableProgramGroupPage=yes
OutputDir={#OutputDir}
OutputBaseFilename=ConkerRedux-Setup-{#AppVersion}
SetupIconFile={#IconFile}
UninstallDisplayIcon={app}\ConkerRedux.exe
UninstallDisplayName=Conker Redux
LicenseFile={#StageDir}\LICENSE.txt
InfoAfterFile={#StageDir}\README.txt
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\Conker Redux"; Filename: "{app}\ConkerRedux.exe"; WorkingDir: "{app}"
Name: "{group}\Conker Redux (launcher, mods)"; Filename: "{app}\ConkerRedux.exe"; Parameters: "--launcher"; WorkingDir: "{app}"
Name: "{group}\Uninstall Conker Redux"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Conker Redux"; Filename: "{app}\ConkerRedux.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\ConkerRedux.exe"; Description: "{cm:LaunchProgram,Conker Redux}"; Flags: nowait postinstall skipifsilent
