[Setup]
AppId={{E2A9C4B1-7D3F-4B58-A6C2-91F0D5E8B3A7}
AppName=Ralvioli Suite
AppVersion=1.1
AppPublisher=Ralvioli
DefaultDirName={autopf}\Ralvioli Suite
DefaultGroupName=Ralvioli Suite
DisableProgramGroupPage=yes
OutputDir=build
OutputBaseFilename=Ralvioli Suite Setup
SetupIconFile=RalvioliMerger.ico
UninstallDisplayIcon={app}\Ralvioli Merger.exe
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin

[Tasks]
Name: "desktopicons"; Description: "Create desktop shortcuts"; Flags: unchecked

[Files]
Source: "build\Ralvioli Merger.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\Ralvioli Viewer.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\Ralvioli Plotter.exe"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\Ralvioli Merger"; Filename: "{app}\Ralvioli Merger.exe"
Name: "{group}\Ralvioli Viewer"; Filename: "{app}\Ralvioli Viewer.exe"
Name: "{group}\Ralvioli Plotter"; Filename: "{app}\Ralvioli Plotter.exe"
Name: "{group}\Uninstall Ralvioli Suite"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Ralvioli Merger"; Filename: "{app}\Ralvioli Merger.exe"; Tasks: desktopicons
Name: "{autodesktop}\Ralvioli Viewer"; Filename: "{app}\Ralvioli Viewer.exe"; Tasks: desktopicons
Name: "{autodesktop}\Ralvioli Plotter"; Filename: "{app}\Ralvioli Plotter.exe"; Tasks: desktopicons
