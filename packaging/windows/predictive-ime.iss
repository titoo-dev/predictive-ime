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
; predict-admin.exe peut être ouvert pendant une mise à jour : on le ferme
; (et on le rouvre) plutôt que d'échouer sur un fichier verrouillé.
CloseApplications=yes
RestartApplications=no

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
; Le panneau d'administration (réglages, clé API, état du daemon) : lancé
; depuis le menu Démarrer, l'icône de la barre des tâches ou la fin du setup.
Source: "{#BuildDir}\win\admin\Release\predict-admin.exe"; DestDir: "{app}"; Flags: ignoreversion
; Le modèle n-gramme, LIVRÉ dans l'installeur (scripts\package-windows.ps1 le
; dépose dans {#BuildDir}\model) : aucun téléchargement ni zstd à l'installation,
; donc une machine sans réseau ou sans outils installe quand même quelque chose
; qui prédit. Partagé par tous les utilisateurs, en lecture seule sous Program Files.
Source: "{#BuildDir}\model\*"; DestDir: "{app}\model"; Flags: ignoreversion recursesubdirs
; Scripts d'exploitation (modèle, tâche de session, sondes).
Source: "..\..\scripts\setup-windows.ps1";  DestDir: "{app}\scripts"; Flags: ignoreversion
Source: "..\..\scripts\PredictLayout.ps1";  DestDir: "{app}\scripts"; Flags: ignoreversion
Source: "..\..\scripts\probe-daemon.ps1";   DestDir: "{app}\scripts"; Flags: ignoreversion
Source: "..\..\scripts\try-daemon.ps1";     DestDir: "{app}\scripts"; Flags: ignoreversion
Source: "..\..\README.md";                  DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\Tester la prédiction"; Filename: "powershell.exe"; \
  Parameters: "-NoExit -ExecutionPolicy Bypass -File ""{app}\scripts\try-daemon.ps1"""; \
  IconFilename: "{app}\predict-tsf.dll"
; Le panneau lit %APPDATA% de l'utilisateur qui le LANCE (aucun chemin figé
; à l'installation, qui se fait en admin).
Name: "{group}\Predict — Administration"; Filename: "{app}\predict-admin.exe"; \
  Comment: "Réglages, clé API de reformulation, état du daemon"
Name: "{autodesktop}\Predict — Administration"; Filename: "{app}\predict-admin.exe"; \
  Tasks: desktopicon
Name: "{group}\Désinstaller {#AppName}"; Filename: "{uninstallexe}"

[Tasks]
Name: "desktopicon"; Description: "Créer une icône « Predict — Administration » sur le Bureau"; \
  GroupDescription: "Raccourcis :"; Flags: unchecked

[Run]
; (La configuration du daemon — tâche de session, config.json, ajout de
; « Predict » à Win+Espace — est lancée depuis [Code] : une entrée [Run]
; ignore le code de retour, et un setup qui échoue passait pour réussi.)
; Dernière page : proposer d'ouvrir le panneau (clé API, langue…). Jamais en
; installation silencieuse, et dans le profil de l'utilisateur, pas de l'admin.
Filename: "{app}\predict-admin.exe"; Description: "Ouvrir le panneau d'administration de Predict"; \
  Flags: postinstall nowait skipifsilent runasoriginaluser

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

// Tâche de session + config.json + ajout de « Predict » à Win+Espace, dans le
// contexte de l'utilisateur qui installe (ExecAsOriginalUser : si l'élévation
// s'est faite avec un AUTRE compte administrateur, c'est quand même le profil
// de l'utilisateur qui reçoit le clavier et le daemon). Le script écrit son
// journal dans %LOCALAPPDATA%\ime-predictord\setup.log ; un échec est SIGNALÉ.
procedure RunUserSetup();
var
  ResultCode: Integer;
  Params: String;
begin
  Params := '-ExecutionPolicy Bypass -NoProfile -File "' + ExpandConstant('{app}') +
            '\scripts\setup-windows.ps1" -SkipIme -AddInputMethod -ModelDir "' +
            ExpandConstant('{app}') + '\model"';
  if not ExecAsOriginalUser('powershell.exe', Params, '', SW_HIDE,
                            ewWaitUntilTerminated, ResultCode) then
    ResultCode := -1;
  Log('setup-windows.ps1 -> code ' + IntToStr(ResultCode));
  if ResultCode <> 0 then
    SuppressibleMsgBox(
      'Le text service est installé, mais la configuration du daemon de prédiction a échoué (code ' +
      IntToStr(ResultCode) + ').' + #13#10#13#10 +
      'Journal : %LOCALAPPDATA%\ime-predictord\setup.log' + #13#10 +
      'Pour réessayer : ' + ExpandConstant('{app}') + '\scripts\setup-windows.ps1',
      mbError, MB_OK, IDOK);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
  begin
    GrantAppContainerRead();
    RunUserSetup();
  end;
end;
