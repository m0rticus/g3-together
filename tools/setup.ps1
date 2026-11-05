# SPDX-License-Identifier: GPL-3.0-or-later
# G3 Together setup (owner 24.10.): finds Gothic 3, checks the tested patches, copies the mod into its scripts folder and
# writes g3together.ini. Setup.cmd starts it in English, Einrichten.cmd in German, Instalacja.cmd in Polish. Every answer
# can also be given as a parameter; with -Yes nothing is asked (the given values and the defaults are taken).
# The INI gets language = auto: the mod's messages follow the game's text language, whichever language the setup spoke.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\setup.ps1 [-Language de|pl] [-GamePath <folder>] [-Role host|client]
#              [-Yes]
# Steam is required. The mod needs Valve's 32-bit steam_api.dll in <game>\G3Together: from the package's
# G3Together folder if it has one, else downloaded from the Steamworks.NET release after asking, checked by its SHA-256.
param(
    [ValidateSet('en', 'de', 'pl')][string]$Language = 'en',
    [string]$GamePath = '',
    [ValidateSet('', 'host', 'client')][string]$Role = '',
    [switch]$Yes
)
$ErrorActionPreference = 'Stop'
$package = Split-Path -Parent $PSScriptRoot
$modDll = Join-Path $package 'scripts\Script_G3Together.dll'
# Steamworks.NET Standalone 2025.164.1, Windows-x86/steam_api.dll: the copy coop-anything's playbook and Jericho Co-op tested.
$steamZip = 'https://github.com/rlabrecque/Steamworks.NET/releases/download/2025.164.1/Steamworks.NET-Standalone_2025.164.1.zip'
$steamDllHash = 'FDAFB7C65EC7D4182B42BF92CC8671C68E4DC547CC6E613899D2618A47EB65DA'

$texts = @{
    en = @{
        Title        = 'G3 Together setup'
        NoDll        = 'scripts\Script_G3Together.dll is missing next to this script. Unpack the whole package first.'
        Found        = 'Gothic 3 found:'
        Choose       = 'Number of the folder to use, or a folder path'
        AskPath      = 'Gothic 3 folder (the one with Gothic3.exe)'
        NoGame       = 'No Gothic3.exe in "{0}".'
        CopyHint     = 'Tip: a separate copy of Gothic 3 for co-op keeps your own game untouched.'
        Using        = 'Gothic 3 folder: {0}'
        CpOk         = 'Community Patch 1.75.14: found ({0}).'
        CpBad        = 'Community Patch: Gothic3.exe is version {0}, tested is 1.75.14. Both players need the same version.'
        PupOk        = 'Parallel Universe Patch 1.1.1: found.'
        PupBad       = 'Parallel Universe Patch: {0}; tested is 1.1.1. Both players need the same version.'
        PupMissing   = 'not installed'
        OtherCoop    = 'Another co-op mod is in scripts: {0}. It cannot run together with G3 Together.'
        MoveOther    = 'Move it out of scripts (into G3Together-backup)?'
        Moved        = 'Moved to {0}.'
        GoOn         = 'Set up G3 Together anyway?'
        Stopped      = 'Nothing changed.'
        Running      = 'Gothic 3 is running from this folder. Close it, then press Enter.'
        StillRunning = 'Gothic 3 is still running; nothing changed.'
        DllSame      = 'G3 Together is installed already (the same version).'
        DllDone      = 'G3 Together copied to {0}.'
        DllBackup    = 'The previous version is kept in {0}.'
        AskRole      = 'Do you host the campaign (1) or join a host (2)?'
        SteamDllOk   = 'steam_api.dll for the Steam connection: in place ({0}).'
        SteamDllDone = 'steam_api.dll copied to {0}.'
        SteamDllAsk  = 'The Steam connection needs Valve''s steam_api.dll (32-bit, from the Steamworks.NET release 2025.164.1 on GitHub, checked by its SHA-256). Download it now?'
        SteamDllBad  = 'The downloaded steam_api.dll does not have the expected SHA-256; it is not used.'
        SteamDllFail = 'steam_api.dll could not be downloaded ({0}).'
        SteamDllNone = 'Steam is required. Setup cannot install the mod without the verified steam_api.dll.'
        SteamHost    = 'Over Steam: both players must own Gothic 3 on Steam and be Steam friends. Steam must be running. Names come from Steam automatically; activity shows Gothic 3.'
        SteamClient  = 'Over Steam: the host must be on your Steam friends list and have Steam running; your game finds theirs by itself.'
        IniBackup    = 'Your previous g3together.ini is kept in {0}.'
        IniDone      = 'g3together.ini written: {0}'
        DoneHost     = 'Done. Start Gothic 3 and begin a new game or load your co-op campaign; then your partner starts.'
        DoneClient   = 'Done. Start Gothic 3 once the host plays. If the host loaded a save, stay in the main menu: it is loaded for you.'
        Yes          = 'y'
        YesNo        = '(y/n)'
    }
    de = @{
        Title        = 'G3 Together einrichten'
        NoDll        = 'Neben diesem Skript fehlt scripts\Script_G3Together.dll. Bitte erst das ganze Paket entpacken.'
        Found        = 'Gothic 3 gefunden:'
        Choose       = 'Nummer des Ordners oder ein Ordnerpfad'
        AskPath      = 'Gothic-3-Ordner (der mit Gothic3.exe)'
        NoGame       = 'In „{0}“ liegt keine Gothic3.exe.'
        CopyHint     = 'Tipp: Mit einer eigenen Kopie von Gothic 3 für den Koop bleibt dein normales Spiel unberührt.'
        Using        = 'Gothic-3-Ordner: {0}'
        CpOk         = 'Community Patch 1.75.14: vorhanden ({0}).'
        CpBad        = 'Community Patch: Gothic3.exe hat Version {0}, getestet ist 1.75.14. Beide Spieler brauchen dieselbe Version.'
        PupOk        = 'Parallel Universe Patch 1.1.1: vorhanden.'
        PupBad       = 'Parallel Universe Patch: {0}; getestet ist 1.1.1. Beide Spieler brauchen dieselbe Version.'
        PupMissing   = 'nicht installiert'
        OtherCoop    = 'In scripts liegt eine andere Koop-Mod: {0}. Sie läuft nicht zusammen mit G3 Together.'
        MoveOther    = 'Aus scripts herausnehmen (nach G3Together-backup)?'
        Moved        = 'Verschoben nach {0}.'
        GoOn         = 'G3 Together trotzdem einrichten?'
        Stopped      = 'Nichts geändert.'
        Running      = 'Gothic 3 läuft aus diesem Ordner. Bitte beenden, dann Enter drücken.'
        StillRunning = 'Gothic 3 läuft noch; nichts geändert.'
        DllSame      = 'G3 Together ist schon installiert (dieselbe Version).'
        DllDone      = 'G3 Together kopiert nach {0}.'
        DllBackup    = 'Die vorige Version liegt in {0}.'
        AskRole      = 'Bist du Host der Kampagne (1) oder trittst du einem Host bei (2)?'
        SteamDllOk   = 'steam_api.dll für die Steam-Verbindung: vorhanden ({0}).'
        SteamDllDone = 'steam_api.dll kopiert nach {0}.'
        SteamDllAsk  = 'Die Steam-Verbindung braucht Valves steam_api.dll (32 Bit, aus dem Steamworks.NET-Release 2025.164.1 auf GitHub, geprüft per SHA-256). Jetzt herunterladen?'
        SteamDllBad  = 'Die heruntergeladene steam_api.dll hat nicht die erwartete SHA-256; sie wird nicht verwendet.'
        SteamDllFail = 'steam_api.dll konnte nicht heruntergeladen werden ({0}).'
        SteamDllNone = 'Steam ist erforderlich. Ohne die geprüfte steam_api.dll kann die Mod nicht installiert werden.'
        SteamHost    = 'Beide müssen Gothic 3 auf Steam besitzen und Steam-Freunde sein. Steam muss laufen. Namen kommen automatisch von Steam; die Aktivität zeigt Gothic 3.'
        SteamClient  = 'Über Steam: Der Host muss in deiner Steam-Freundesliste sein und Steam laufen haben; dein Spiel findet seins von selbst.'
        IniBackup    = 'Deine vorige g3together.ini liegt in {0}.'
        IniDone      = 'g3together.ini geschrieben: {0}'
        DoneHost     = 'Fertig. Starte Gothic 3 und beginne ein neues Spiel oder lade deine Koop-Kampagne; danach startet dein Mitspieler.'
        DoneClient   = 'Fertig. Starte Gothic 3, sobald der Host spielt. Hat der Host einen Spielstand geladen, bleib im Hauptmenü: er wird für dich geladen.'
        Yes          = 'j'
        YesNo        = '(j/n)'
    }
    pl = @{
        Title        = 'Instalacja G3 Together'
        NoDll        = 'Obok tego skryptu brakuje pliku scripts\Script_G3Together.dll. Najpierw rozpakuj cały pakiet.'
        Found        = 'Znaleziono Gothic 3:'
        Choose       = 'Numer folderu albo ścieżka do folderu'
        AskPath      = 'Folder Gothic 3 (ten z plikiem Gothic3.exe)'
        NoGame       = 'W „{0}” nie ma pliku Gothic3.exe.'
        CopyHint     = 'Wskazówka: osobna kopia Gothic 3 do gry w kooperacji zostawia twoją zwykłą grę nietkniętą.'
        Using        = 'Folder Gothic 3: {0}'
        CpOk         = 'Community Patch 1.75.14: znaleziono ({0}).'
        CpBad        = 'Community Patch: Gothic3.exe ma wersję {0}, testowana jest 1.75.14. Obaj gracze potrzebują tej samej wersji.'
        PupOk        = 'Parallel Universe Patch 1.1.1: znaleziono.'
        PupBad       = 'Parallel Universe Patch: {0}; testowana jest 1.1.1. Obaj gracze potrzebują tej samej wersji.'
        PupMissing   = 'nie zainstalowano'
        OtherCoop    = 'W folderze scripts jest inny mod kooperacji: {0}. Nie działa razem z G3 Together.'
        MoveOther    = 'Przenieść go ze scripts (do G3Together-backup)?'
        Moved        = 'Przeniesiono do {0}.'
        GoOn         = 'Mimo to zainstalować G3 Together?'
        Stopped      = 'Nic nie zmieniono.'
        Running      = 'Gothic 3 działa z tego folderu. Zamknij grę, potem naciśnij Enter.'
        StillRunning = 'Gothic 3 wciąż działa; nic nie zmieniono.'
        DllSame      = 'G3 Together jest już zainstalowany (ta sama wersja).'
        DllDone      = 'Skopiowano G3 Together do {0}.'
        DllBackup    = 'Poprzednia wersja jest w {0}.'
        AskRole      = 'Prowadzisz kampanię jako host (1) czy dołączasz do hosta (2)?'
        SteamDllOk   = 'steam_api.dll do połączenia przez Steam: jest ({0}).'
        SteamDllDone = 'Skopiowano steam_api.dll do {0}.'
        SteamDllAsk  = 'Połączenie przez Steam wymaga pliku steam_api.dll od Valve (32-bitowy, z wydania Steamworks.NET 2025.164.1 na GitHubie, sprawdzany sumą SHA-256). Pobrać go teraz?'
        SteamDllBad  = 'Pobrany steam_api.dll nie ma oczekiwanej sumy SHA-256; nie zostanie użyty.'
        SteamDllFail = 'Nie udało się pobrać steam_api.dll ({0}).'
        SteamDllNone = 'Steam jest wymagany. Bez sprawdzonego steam_api.dll nie można zainstalować moda.'
        SteamHost    = 'Obaj gracze muszą posiadać Gothic 3 na Steam i być znajomymi. Steam musi działać. Nazwy pochodzą automatycznie ze Steam; aktywność pokazuje Gothic 3.'
        SteamClient  = 'Przez Steam: host musi być na twojej liście znajomych na Steam i mieć uruchomiony Steam; twoja gra sama znajdzie jego grę.'
        IniBackup    = 'Twój poprzedni g3together.ini jest w {0}.'
        IniDone      = 'Zapisano g3together.ini: {0}'
        DoneHost     = 'Gotowe. Uruchom Gothic 3 i rozpocznij nową grę albo wczytaj swoją kampanię kooperacji; potem startuje drugi gracz.'
        DoneClient   = 'Gotowe. Uruchom Gothic 3, gdy host już gra. Jeśli host wczytał zapis, zostań w menu głównym: zapis zostanie wczytany dla ciebie.'
        Yes          = 't'
        YesNo        = '(t/n)'
    }
}
$t = $texts[$Language]

function Say([string]$text, [string]$color = 'Gray') { Write-Host $text -ForegroundColor $color }

function Ask([string]$question, [string]$default) {
    if ($Yes) { return $default }
    $shown = if ($default) { "$question [$default]" } else { $question }
    $answer = Read-Host $shown
    if ([string]::IsNullOrWhiteSpace($answer)) { return $default }
    return $answer.Trim()
}

function AskYes([string]$question, [bool]$default) {
    if ($Yes) { return $default }
    $answer = Read-Host "$question $($t.YesNo)"
    if ([string]::IsNullOrWhiteSpace($answer)) { return $default }
    return $answer.Trim().ToLower().StartsWith($t.Yes) -or $answer.Trim().ToLower().StartsWith('y')
}

# "1, 75, 20108, 14" -> "1.75.20108.14"
function VersionOf([string]$file) {
    if (-not (Test-Path -LiteralPath $file)) { return $null }
    $v = (Get-Item -LiteralPath $file).VersionInfo.FileVersion
    if (-not $v) { return '' }
    return ($v -replace '\s', '' -replace ',', '.')
}

# Gothic 3 folders this PC knows: Steam's libraries, GOG, and the folder the package was unpacked in or beside.
function FindGothic3 {
    $found = New-Object System.Collections.Generic.List[string]
    $steam = (Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath
    if ($steam) {
        $libraries = @($steam)
        $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
        if (Test-Path -LiteralPath $vdf) {
            foreach ($m in [regex]::Matches((Get-Content -LiteralPath $vdf -Raw), '"path"\s+"([^"]+)"')) { $libraries += ($m.Groups[1].Value -replace '\\\\', '\') }
        }
        foreach ($library in $libraries) { $found.Add((Join-Path $library 'steamapps\common\Gothic 3')) }
    }
    foreach ($key in 'HKLM:\SOFTWARE\WOW6432Node\GOG.com\Games\*', 'HKLM:\SOFTWARE\GOG.com\Games\*') {
        foreach ($game in @(Get-ItemProperty -Path $key -ErrorAction SilentlyContinue)) {
            if ("$($game.gameName)" -like 'Gothic 3*' -and $game.path) { $found.Add($game.path) }
        }
    }
    $found.Add($package)
    $found.Add((Split-Path -Parent $package))
    $result = @()
    foreach ($folder in $found) {
        if ($folder -and (Test-Path -LiteralPath (Join-Path $folder 'Gothic3.exe'))) {
            $full = (Resolve-Path -LiteralPath $folder).Path.TrimEnd('\')
            if ($result -notcontains $full) { $result += $full }
        }
    }
    return , $result
}

function Backup([string]$game) {
    $folder = Join-Path $game 'G3Together-backup'
    if (-not (Test-Path -LiteralPath $folder)) { New-Item -ItemType Directory -Path $folder | Out-Null }
    return $folder
}

# key = value lines of an INI (comments after # ignored, as the mod reads them).
function ReadIni([string]$path) {
    $values = [ordered]@{}
    if (-not (Test-Path -LiteralPath $path)) { return $values }
    foreach ($line in Get-Content -LiteralPath $path) {
        $text = ($line -split '#', 2)[0]
        if ($text -match '^\s*([A-Za-z_]+)\s*=\s*(.*?)\s*$') { $values[$Matches[1].ToLower()] = $Matches[2] }
    }
    return $values
}

# Required <game>\G3Together\steam_api.dll: in place, from the package, or downloaded (asked) and checked.
function InstallSteamDll([string]$game) {
    $folder = Join-Path $game 'G3Together'
    $target = Join-Path $folder 'steam_api.dll'
    if ((Test-Path -LiteralPath $target) -and (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -eq $steamDllHash) {
        Say ($t.SteamDllOk -f $target) 'Green'
        return $true
    }
    if (-not (Test-Path -LiteralPath $folder)) { New-Item -ItemType Directory -Path $folder | Out-Null }
    $bundled = Join-Path $package 'G3Together\steam_api.dll'
    if ((Test-Path -LiteralPath $bundled) -and (Get-FileHash -LiteralPath $bundled -Algorithm SHA256).Hash -eq $steamDllHash) {
        Copy-Item -LiteralPath $bundled -Destination $target -Force
        Say ($t.SteamDllDone -f $target) 'Green'
        return $true
    }
    if (-not (AskYes $t.SteamDllAsk $true)) { return $false }
    $temp = Join-Path ([System.IO.Path]::GetTempPath()) "g3t-steam-$([guid]::NewGuid().ToString('N'))"
    try {
        New-Item -ItemType Directory -Path $temp | Out-Null
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        $zip = Join-Path $temp 'steamworks.zip'
        Invoke-WebRequest -Uri $steamZip -OutFile $zip -UseBasicParsing
        Expand-Archive -LiteralPath $zip -DestinationPath (Join-Path $temp 'x') -Force
        $dll = Join-Path $temp 'x\Windows-x86\steam_api.dll'
        if (-not (Test-Path -LiteralPath $dll) -or (Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash -ne $steamDllHash) {
            Say $t.SteamDllBad 'Red'
            return $false
        }
        Copy-Item -LiteralPath $dll -Destination $target -Force
        Say ($t.SteamDllDone -f $target) 'Green'
        return $true
    } catch {
        Say ($t.SteamDllFail -f $_.Exception.Message) 'Yellow'
        return $false
    } finally {
        Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Say ''
Say "== $($t.Title) ==" 'Cyan'
if (-not (Test-Path -LiteralPath $modDll)) { Say $t.NoDll 'Red'; exit 1 }

# 1. The game folder.
$game = $GamePath
if (-not $game) {
    $candidates = FindGothic3
    if ($candidates.Count -gt 0) {
        Say $t.Found
        for ($i = 0; $i -lt $candidates.Count; ++$i) { Say ("  {0}) {1}" -f ($i + 1), $candidates[$i]) }
        $answer = Ask $t.Choose '1'
        if ($answer -match '^\d+$' -and [int]$answer -ge 1 -and [int]$answer -le $candidates.Count) { $game = $candidates[[int]$answer - 1] }
        else { $game = $answer }
    } else {
        $game = Ask $t.AskPath ''
    }
}
$game = "$game".Trim().Trim('"')
if (-not $game -or -not (Test-Path -LiteralPath (Join-Path $game 'Gothic3.exe'))) { Say ($t.NoGame -f $game) 'Red'; exit 1 }
$game = (Resolve-Path -LiteralPath $game).Path.TrimEnd('\')
Say ($t.Using -f $game) 'Green'
Say $t.CopyHint

# 2. The tested baseline: Community Patch 1.75.14 and Parallel Universe Patch 1.1.1; no other co-op mod in scripts.
$problems = 0
$exeVersion = VersionOf (Join-Path $game 'Gothic3.exe')
if ($exeVersion -match '^1\.75\.\d+\.14$') { Say ($t.CpOk -f $exeVersion) 'Green' } else { Say ($t.CpBad -f $exeVersion) 'Yellow'; ++$problems }
$paru = VersionOf (Join-Path $game 'scripts\Script_paru.dll')
if ($paru -eq '1.1.1.0') { Say $t.PupOk 'Green' } else { Say ($t.PupBad -f $(if ($null -eq $paru) { $t.PupMissing } else { $paru })) 'Yellow'; ++$problems }
foreach ($other in @('Script_G3Coop.dll')) {
    $path = Join-Path $game "scripts\$other"
    if (Test-Path -LiteralPath $path) {
        Say ($t.OtherCoop -f $other) 'Yellow'
        if (AskYes $t.MoveOther $true) {
            $to = Join-Path (Backup $game) $other
            Move-Item -LiteralPath $path -Destination $to -Force
            Say ($t.Moved -f $to)
        } else { ++$problems }
    }
}
if ($problems -gt 0 -and -not (AskYes $t.GoOn $true)) { Say $t.Stopped; exit 1 }

# 3. The mod into scripts (the game must not be running from this folder).
$exe = Join-Path $game 'Gothic3.exe'
$running = { @(Get-Process -Name 'Gothic3' -ErrorAction SilentlyContinue | Where-Object { $_.Path -and ($_.Path -ieq $exe) }).Count -gt 0 }
if (& $running) {
    if (-not $Yes) { Read-Host $t.Running | Out-Null }
    if (& $running) { Say $t.StillRunning 'Red'; exit 1 }
}
if (-not (InstallSteamDll $game)) { Say $t.SteamDllNone 'Red'; exit 1 }
$target = Join-Path $game 'scripts\Script_G3Together.dll'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
if ((Test-Path -LiteralPath $target) -and ((Get-FileHash -LiteralPath $target).Hash -eq (Get-FileHash -LiteralPath $modDll).Hash)) {
    Say $t.DllSame 'Green'
} else {
    if (Test-Path -LiteralPath $target) {
        $old = Join-Path (Backup $game) "Script_G3Together-$stamp.dll"
        Copy-Item -LiteralPath $target -Destination $old -Force
        Say ($t.DllBackup -f $old)
    }
    Copy-Item -LiteralPath $modDll -Destination $target -Force
    Say ($t.DllDone -f $target) 'Green'
}

# 4. g3together.ini: Steam-only role, name and language; unrelated earlier settings are kept.
$iniPath = Join-Path $game 'g3together.ini'
$old = ReadIni $iniPath
$oldRole = if ($old['role'] -eq 'client') { 'client' } elseif ($old['role']) { 'host' } else { '' }
if (-not $Role) {
    $answer = Ask $t.AskRole $(if ($oldRole -eq 'client') { '2' } else { '1' })
    $Role = if ($answer -eq '2' -or $answer -ieq 'client') { 'client' } else { 'host' }
}

$lines = New-Object System.Collections.Generic.List[string]
if ($Role -eq 'host') { $lines.Add('# G3 Together: this player hosts the campaign (written by the setup).') }
else { $lines.Add('# G3 Together: this player joins the host (written by the setup).') }
$lines.Add("role = $Role")
$lines.Add('transport = steam')
$lines.Add('# Player names are taken from Steam automatically.')
$lines.Add('# Mod messages only: auto (the language of your Gothic 3 texts), en, de or pl. Each player can choose independently.')
$lines.Add('language = auto')
$known = @('role', 'transport', 'address', 'port', 'name', 'language', 'offer_accept_key', 'offer_decline_key', 'price_up_key', 'price_down_key', 'qa_keycheck')
$kept = @($old.Keys | Where-Object { $known -notcontains $_ })
if ($kept.Count -gt 0) {
    $lines.Add('# Kept from the previous g3together.ini:')
    foreach ($key in $kept) { $lines.Add("$key = $($old[$key])") }
} else {
    $lines.Add('# Jump to the other player (F1-F24, PAGEUP, HOME, END, INSERT, DELETE or a letter)')
    $lines.Add('partner_key = PAGEDOWN')
    $lines.Add('# Your own look for the other players (a character template such as Diego)')
    $lines.Add('# look = Diego')
    if ($Role -eq 'host') {
        $lines.Add('# Town reputation: shared (one record per town for both; default), together (shared guilt), personal')
        $lines.Add('# reputation = shared')
    }
}
if (-not $old.Contains('friendly_fire')) {
    $lines.Add('# Friendly fire is off, including allied hit reactions and harmful effects. Both players must match.')
    $lines.Add('friendly_fire = 0')
}
if (Test-Path -LiteralPath $iniPath) {
    $saved = Join-Path (Backup $game) "g3together-$stamp.ini"
    Copy-Item -LiteralPath $iniPath -Destination $saved -Force
    Say ($t.IniBackup -f $saved)
}
# Without a byte order mark: the mod reads the first key as it stands.
[System.IO.File]::WriteAllText($iniPath, (($lines -join "`r`n") + "`r`n"), (New-Object System.Text.UTF8Encoding $false))
Say ($t.IniDone -f $iniPath) 'Green'

Say $(if ($Role -eq 'host') { $t.SteamHost } else { $t.SteamClient }) 'Green'
if ($Role -eq 'host') {
    Say $t.DoneHost 'Cyan'
} else {
    Say $t.DoneClient 'Cyan'
}
exit 0
