; Installeur Windows de predictive-ime (Inno Setup 6).
;
; Construire :  iscc packaging\windows\predictive-ime.iss
; Prérequis  :  .\scripts\build-windows.ps1   (produit predictord.exe + predict-tsf.dll)
;
; Ce que l'installeur fait, et pourquoi :
;   - installe pour TOUS les utilisateurs sous Program Files : un text service
;     TSF est enregistré dans HKLM, le mettre dans un dossier utilisateur le
;     rendrait modifiable par un process non privilégié qui se ferait alors
;     charger dans chaque application (élévation de privilèges).
;   - enregistre la DLL (regsvr32 implicite via `regserver`) ;
;   - donne à ALL APPLICATION PACKAGES le droit de lire la DLL, sans quoi les
;     applications du Store ne peuvent pas la charger et échouent EN SILENCE ;
;   - installe le daemon et sa tâche de session, per-utilisateur.

#define AppName      "predictive-ime"
#define AppVersion   "0.1.0"
#define AppPublisher "predictive-ime"
#define AppURL       "https://github.com/titoo-dev/predictive-ime"
#define BuildDir     "..\..\build-win-x64"

[Setup]
AppId={{9E4C2A15-7D30-4B6E-A0F2-3C81D5E7B920}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
OutputDir=..\..\dist
OutputBaseFilename=predictive-ime-{#AppVersion}-x64
Compression=lzma2/max
SolidCompression=yes
; Le text service est 64 bits : on refuse d'installer sur un Windows 32 bits
; plutôt que de laisser une DLL qui ne se chargera jamais.
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; HKLM + Program Files + regsvr32 : l'élévation est indispensable.
PrivilegesRequired=admin
MinVersion=10.0.17763
; « dynamic » suit le thème clair/sombre de Windows (Inno Setup 6.6+) ; un
; compilateur plus ancien refuserait le mot-clé, on retombe alors sur modern.
#if Ver >= EncodeVer(6, 6, 0)
WizardStyle=modern dynamic
#else
WizardStyle=modern
#endif
LicenseFile=..\..\LICENSE
; Même icône que le profil clavier (ressource de la DLL) : l'installeur,
; « Applications installées » et le menu Démarrer montrent tous la même.
SetupIconFile=..\..\win\tsf\predict.ico
UninstallDisplayIcon={app}\predict-tsf.dll,0
UninstallDisplayName={#AppName}

[Languages]
Name: "french";  MessagesFile: "compiler:Languages\French.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
; Le text service, enregistré comme serveur COM in-process.
Source: "{#BuildDir}\win\tsf\Release\predict-tsf.dll"; DestDir: "{app}"; \
  Flags: ignoreversion regserver restartreplace uninsrestartdelete
; Le daemon et ses dépendances vcpkg.
Source: "{#BuildDir}\daemon\Release\predictord.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\daemon\Release\*.dll";          DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
; Scripts d'exploitation (modèle, tâche de session, sondes).
Source: "..\..\scripts\setup-windows.ps1";  DestDir: "{app}\scripts"; Flags: ignoreversion
Source: "..\..\scripts\probe-daemon.ps1";   DestDir: "{app}\scripts"; Flags: ignoreversion
Source: "..\..\scripts\try-daemon.ps1";     DestDir: "{app}\scripts"; Flags: ignoreversion
Source: "..\..\README.md";                  DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\Tester la prédiction"; Filename: "powershell.exe"; \
  Parameters: "-NoExit -ExecutionPolicy Bypass -File ""{app}\scripts\try-daemon.ps1"""; \
  IconFilename: "{app}\predict-tsf.dll"
; Les réglages vivent dans %APPDATA% de CHAQUE utilisateur. Un raccourci qui
; figerait ce chemin à l'installation (faite en admin) pointerait vers le
; profil de l'administrateur : on résout donc la variable au LANCEMENT.
Name: "{group}\Réglages (config.json)"; Filename: "{sys}\cmd.exe"; \
  Parameters: "/c start """" ""%APPDATA%\ime-predictord\config.json"""; \
  IconFilename: "{app}\predict-tsf.dll"; Flags: runminimized
Name: "{group}\Désinstaller {#AppName}"; Filename: "{uninstallexe}"

[Run]
; Modèle + tâche de session + ajout de « Predict » à Win+Espace, dans le
; contexte de l'utilisateur qui installe (runasoriginaluser : si l'élévation
; s'est faite avec un AUTRE compte administrateur, c'est quand même le profil
; de l'utilisateur qui reçoit le clavier et le modèle). Sans -AddInputMethod,
; le clavier était inscrit mais introuvable tant qu'on ne l'ajoutait pas à la
; main dans les Paramètres.
Filename: "powershell.exe"; \
  Parameters: "-ExecutionPolicy Bypass -NoProfile -File ""{app}\scripts\setup-windows.ps1"" -SkipIme -AddInputMethod"; \
  StatusMsg: "Téléchargement du modèle et configuration du daemon…"; \
  Flags: runhidden waituntilterminated runasoriginaluser

[UninstallRun]
Filename: "powershell.exe"; \
  Parameters: "-ExecutionPolicy Bypass -NoProfile -File ""{app}\scripts\setup-windows.ps1"" -Uninstall"; \
  Flags: runhidden waituntilterminated; RunOnceId: "RemovePredictTask"

[Code]
// ALL APPLICATION PACKAGES (S-1-15-2-1) doit pouvoir LIRE et EXÉCUTER la DLL,
// sinon les applications du Store et une partie d'Edge ne la chargent pas — et
// ne disent rien. On passe par le SID, pas par le nom : celui-ci est traduit
// selon la langue de Windows.
procedure GrantAppContainerRead();
var
  ResultCode: Integer;
begin
  Exec(ExpandConstant('{sys}\icacls.exe'),
       ExpandConstant('"{app}" /grant *S-1-15-2-1:(OI)(CI)(RX) /T /C /Q'),
       '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    GrantAppContainerRead();
end;
