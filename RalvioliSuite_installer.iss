[Setup]
AppId={{E2A9C4B1-7D3F-4B58-A6C2-91F0D5E8B3A7}
AppName=Ralvioli Suite
AppVersion=1.1.1
AppPublisher=Ralvioli
DefaultDirName={autopf}\Ralvioli Suite
DefaultGroupName=Ralvioli Suite
DisableProgramGroupPage=yes
OutputDir=build
OutputBaseFilename=Ralvioli Suite Setup
SetupIconFile=RalvioliMerger.ico
UninstallDisplayIcon={uninstallexe}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin

[Types]
Name: "full"; Description: "All applications"
Name: "custom"; Description: "Choose applications"; Flags: iscustom

[Components]
Name: "merger"; Description: "Ralvioli Merger"; Types: full custom
Name: "viewer"; Description: "Ralvioli Viewer"; Types: full custom
Name: "plotter"; Description: "Ralvioli Plotter"; Types: full custom

[Tasks]
Name: "desktopicons"; Description: "Create desktop shortcuts"; Flags: unchecked; Components: merger or viewer or plotter

[Files]
Source: "build\Ralvioli Merger.exe"; DestDir: "{app}"; Flags: ignoreversion; Components: merger
Source: "build\Ralvioli Viewer.exe"; DestDir: "{app}"; Flags: ignoreversion; Components: viewer
Source: "build\Ralvioli Plotter.exe"; DestDir: "{app}"; Flags: ignoreversion; Components: plotter

[Icons]
Name: "{group}\Ralvioli Merger"; Filename: "{app}\Ralvioli Merger.exe"; Components: merger
Name: "{group}\Ralvioli Viewer"; Filename: "{app}\Ralvioli Viewer.exe"; Components: viewer
Name: "{group}\Ralvioli Plotter"; Filename: "{app}\Ralvioli Plotter.exe"; Components: plotter
Name: "{group}\Uninstall Ralvioli Suite"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Ralvioli Merger"; Filename: "{app}\Ralvioli Merger.exe"; Tasks: desktopicons; Components: merger
Name: "{autodesktop}\Ralvioli Viewer"; Filename: "{app}\Ralvioli Viewer.exe"; Tasks: desktopicons; Components: viewer
Name: "{autodesktop}\Ralvioli Plotter"; Filename: "{app}\Ralvioli Plotter.exe"; Tasks: desktopicons; Components: plotter
