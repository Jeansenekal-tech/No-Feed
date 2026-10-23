; Inno Setup script for the NoFeed Windows installer.
; Built by CI with:  iscc /DAppVersion=1.0.0 /DBuildDir=<artefacts\Release> /DOutputDir=<dist> NoFeed.iss

#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#ifndef BuildDir
  #define BuildDir "..\..\build\NoFeed_artefacts\Release"
#endif
#ifndef OutputDir
  #define OutputDir "..\..\dist"
#endif

[Setup]
AppId={{6B0C4D51-2E3A-4F7B-9C1D-8A5E2F3B7C90}
AppName=NoFeed
AppVersion={#AppVersion}
AppVerName=NoFeed {#AppVersion}
AppPublisher=UniqueAV
DefaultDirName={autopf}\NoFeed
DefaultGroupName=NoFeed
DisableProgramGroupPage=yes
OutputDir={#OutputDir}
OutputBaseFilename=NoFeed-{#AppVersion}-Windows-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
UninstallDisplayName=NoFeed
UninstallDisplayIcon={app}\NoFeed.exe

[Types]
Name: "full";   Description: "Full installation (VST3 plugin + standalone app)"
Name: "plugin"; Description: "VST3 plugin only"
Name: "custom"; Description: "Custom installation"; Flags: iscustom

[Components]
Name: "vst3";       Description: "VST3 plugin  (C:\Program Files\Common Files\VST3)"; Types: full plugin custom
Name: "standalone"; Description: "Standalone app";                                     Types: full

[Files]
Source: "{#BuildDir}\VST3\NoFeed.vst3\*"; DestDir: "{commoncf64}\VST3\NoFeed.vst3"; Components: vst3; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#BuildDir}\Standalone\NoFeed.exe"; DestDir: "{app}"; Components: standalone; Flags: ignoreversion

[Icons]
Name: "{group}\NoFeed"; Filename: "{app}\NoFeed.exe"; Components: standalone
Name: "{group}\Uninstall NoFeed"; Filename: "{uninstallexe}"

[UninstallDelete]
Type: filesandordirs; Name: "{commoncf64}\VST3\NoFeed.vst3"

[Run]
Filename: "{app}\NoFeed.exe"; Description: "Launch NoFeed now"; Components: standalone; Flags: nowait postinstall skipifsilent
