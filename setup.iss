[Setup]
AppName=AVIF-Master
AppVersion=1.0
DefaultDirName={autopf}\AVIF-Master
DefaultGroupName=AVIF-Master
OutputDir=Output
OutputBaseFilename=AVIFMaster_Setup
SetupIconFile=Setup.ico
UninstallDisplayIcon={app}\AVIFMaster2.exe
Compression=lzma2
SolidCompression=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "korean"; MessagesFile: "compiler:Languages\Korean.isl"

[CustomMessages]
english.CtxMenuText=Convert with AVIF-Master
korean.CtxMenuText=AVIF-Master로 고속 변환

[Files]
Source: "build\AVIFMaster2.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "lib\*"; DestDir: "{app}\lib"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "icon.ico"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\AVIF-Master"; Filename: "{app}\AVIFMaster2.exe"

[Registry]
Root: HKCU; Subkey: "Software\AVIFMaster2"; ValueName: "UILanguage"; ValueType: string; ValueData: "{language}"; Flags: uninsdeletevalue
Root: HKCR; Subkey: "*\shell\AVIFMaster"; ValueType: string; ValueData: "{cm:CtxMenuText}"; Flags: uninsdeletekey
Root: HKCR; Subkey: "*\shell\AVIFMaster"; ValueName: "Icon"; ValueType: string; ValueData: """{app}\AVIFMaster2.exe"""
Root: HKCR; Subkey: "*\shell\AVIFMaster\command"; ValueType: string; ValueData: """{app}\AVIFMaster2.exe"" ""%1"""
Root: HKCR; Subkey: "Directory\shell\AVIFMaster"; ValueType: string; ValueData: "{cm:CtxMenuText}"; Flags: uninsdeletekey
Root: HKCR; Subkey: "Directory\shell\AVIFMaster"; ValueName: "Icon"; ValueType: string; ValueData: """{app}\AVIFMaster2.exe"""
Root: HKCR; Subkey: "Directory\shell\AVIFMaster\command"; ValueType: string; ValueData: """{app}\AVIFMaster2.exe"" ""%1"""
Root: HKCR; Subkey: "Folder\shell\AVIFMaster"; ValueType: string; ValueData: "{cm:CtxMenuText}"; Flags: uninsdeletekey
Root: HKCR; Subkey: "Folder\shell\AVIFMaster"; ValueName: "Icon"; ValueType: string; ValueData: """{app}\AVIFMaster2.exe"""
Root: HKCR; Subkey: "Folder\shell\AVIFMaster\command"; ValueType: string; ValueData: """{app}\AVIFMaster2.exe"" ""%1"""
Root: HKCR; Subkey: "Directory\Background\shell\AVIFMaster"; ValueType: string; ValueData: "{cm:CtxMenuText}"; Flags: uninsdeletekey
Root: HKCR; Subkey: "Directory\Background\shell\AVIFMaster"; ValueName: "Icon"; ValueType: string; ValueData: """{app}\AVIFMaster2.exe"""
Root: HKCR; Subkey: "Directory\Background\shell\AVIFMaster\command"; ValueType: string; ValueData: """{app}\AVIFMaster2.exe"" ""%1"""
