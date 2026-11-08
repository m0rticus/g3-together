# SPDX-License-Identifier: GPL-3.0-or-later
# G3 Together English setup: finds Gothic 3, checks patches, copies the mod and writes g3together.ini.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\setup.ps1 [-GamePath <folder>] [-Role host|client]
#              [-SessionId <id>] [-Yes]
# Steam is required. The setup installs the verified 32-bit steam_api.dll and the G3 Together mod.
param(
    [string]$GamePath = '',
    [ValidateSet('', 'host', 'client')][string]$Role = '',
    [string]$SessionId = '',
    [switch]$Yes
)
$ErrorActionPreference = 'Stop'
$package = Split-Path -Parent $PSScriptRoot
$modDll = Join-Path $package 'scripts\Script_G3Together.dll'
$steamZip = 'https://github.com/rlabrecque/Steamworks.NET/releases/download/2025.164.1/Steamworks.NET-Standalone_2025.164.1.zip'
$steamDllHash = 'FDAFB7C65EC7D4182B42BF92CC8671C68E4DC547CC6E613899D2618A47EB65DA'

$t = @{
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
    AskSessionId = 'Shared session ID (the lobby ID; 1-63 letters, digits, _ or -)'
    SessionIdBad = 'Invalid session ID. Use 1-63 ASCII letters, digits, underscore or hyphen.'
    SteamDllOk   = 'steam_api.dll for the Steam connection: in place ({0}).'
    SteamDllDone = 'steam_api.dll copied to {0}.'
    SteamDllAsk  = 'The Steam connection needs Valve''s steam_api.dll (32-bit, from the Steamworks.NET release 2025.164.1 on GitHub, checked by its SHA-256). Download it now?'
    SteamDllBad  = 'The downloaded steam_api.dll does not have the expected SHA-256; it is not used.'
    SteamDllFail = 'steam_api.dll could not be downloaded ({0}).'
    SteamDllNone = 'Steam is required. Setup cannot install the mod without the verified steam_api.dll.'
    SteamHost    = 'Over Steam: both players must own Gothic 3 on Steam and be Steam friends. Steam must be running. Names come from Steam automatically; activity shows Gothic 3.'
    SteamClient  = 'Over Steam: the host must be on your Steam friends list and have Steam running; accept its invite (or Join Game) to connect.'
    IniBackup    = 'Your previous g3together.ini is kept in {0}.'
    IniDone      = 'g3together.ini written: {0}'
    DoneHost     = 'Done. Start Gothic 3 and begin a new game or load your co-op campaign; then your partner starts.'
    DoneClient   = 'Done. Start Gothic 3 once the host plays. If the host loaded a save, stay in the main menu: it is loaded for you.'
    Yes          = 'y'
    YesNo        = '(y/n)'
}

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

function ValidSessionId([string]$value) {
    return $value -cmatch '^[A-Za-z0-9_-]{1,63}\z'
}

function VersionOf([string]$file) {
    if (-not (Test-Path -LiteralPath $file)) { return $null }
    $v = (Get-Item -LiteralPath $file).VersionInfo.FileVersion
    if (-not $v) { return '' }
    return ($v -replace '\s', '' -replace ',', '.')
}

function FindGothic3 {
    $found = New-Object System.Collections.Generic.List[string]
    $steam = (Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath
    if ($steam) {
        $libraries = @($steam)
        $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
        if (Test-Path -LiteralPath $vdf) {
            foreach ($m in [regex]::Matches((Get-Content -LiteralPath $vdf -Raw), '"path"\s+"([^"]+)"')) {
                $libraries += ($m.Groups[1].Value -replace '\\\\', '\')
            }
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

function ReadIni([string]$path) {
    $values = [ordered]@{}
    if (-not (Test-Path -LiteralPath $path)) { return $values }
    foreach ($line in Get-Content -LiteralPath $path) {
        $text = ($line -split '#', 2)[0]
        if ($text -match '^\s*([A-Za-z_]+)\s*=\s*(.*?)\s*$') { $values[$Matches[1].ToLower()] = $Matches[2] }
    }
    return $values
}

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

$game = $GamePath
if (-not $game) {
    $candidates = FindGothic3
    if ($candidates.Count -gt 0) {
        Say $t.Found
        for ($i = 0; $i -lt $candidates.Count; ++$i) { Say ("  {0}) {1}" -f ($i + 1), $candidates[$i]) }
        $answer = Ask $t.Choose '1'
        if ($answer -match '^\d+$' -and [int]$answer -ge 1 -and [int]$answer -le $candidates.Count) {
            $game = $candidates[[int]$answer - 1]
        } else {
            $game = $answer
        }
    } else {
        $game = Ask $t.AskPath ''
    }
}
$game = "$game".Trim().Trim('"')
if (-not $game -or -not (Test-Path -LiteralPath (Join-Path $game 'Gothic3.exe'))) {
    Say ($t.NoGame -f $game) 'Red'
    exit 1
}
$game = (Resolve-Path -LiteralPath $game).Path.TrimEnd('\')
Say ($t.Using -f $game) 'Green'
Say $t.CopyHint

$iniPath = Join-Path $game 'g3together.ini'
$old = ReadIni $iniPath
$oldSessionId = if (ValidSessionId $old['session_id']) { $old['session_id'] } else { 'default' }
if ([string]::IsNullOrWhiteSpace($SessionId)) {
    $SessionId = if ($Yes) { $oldSessionId } else { Ask $t.AskSessionId $oldSessionId }
}
while (-not (ValidSessionId $SessionId)) {
    if ($Yes) { Say $t.SessionIdBad 'Red'; exit 1 }
    Say $t.SessionIdBad 'Red'
    $SessionId = Ask $t.AskSessionId $oldSessionId
}

$problems = 0
$exeVersion = VersionOf (Join-Path $game 'Gothic3.exe')
if ($exeVersion -match '^1\.75\.\d+\.14$') {
    Say ($t.CpOk -f $exeVersion) 'Green'
} else {
    Say ($t.CpBad -f $exeVersion) 'Yellow'
    ++$problems
}
$paru = VersionOf (Join-Path $game 'scripts\Script_paru.dll')
if ($paru -eq '1.1.1.0') {
    Say $t.PupOk 'Green'
} else {
    Say ($t.PupBad -f $(if ($null -eq $paru) { $t.PupMissing } else { $paru })) 'Yellow'
    ++$problems
}
$scripts = Join-Path $game 'scripts'
if (Test-Path -LiteralPath $scripts) {
    foreach ($other in @(Get-ChildItem -LiteralPath $scripts -Filter 'Script_*coop*.dll' -File -ErrorAction SilentlyContinue)) {
        if ($other.Name -ieq 'Script_G3Together.dll') { continue }
        Say ($t.OtherCoop -f $other.Name) 'Yellow'
        if (AskYes $t.MoveOther $true) {
            $to = Join-Path (Backup $game) $other.Name
            Move-Item -LiteralPath $other.FullName -Destination $to -Force
            Say ($t.Moved -f $to)
        } else {
            ++$problems
        }
    }
}
if ($problems -gt 0 -and -not (AskYes $t.GoOn $true)) { Say $t.Stopped; exit 1 }

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
        $oldDll = Join-Path (Backup $game) "Script_G3Together-$stamp.dll"
        Copy-Item -LiteralPath $target -Destination $oldDll -Force
        Say ($t.DllBackup -f $oldDll)
    }
    Copy-Item -LiteralPath $modDll -Destination $target -Force
    Say ($t.DllDone -f $target) 'Green'
}

$oldRole = if ($old['role'] -eq 'client') { 'client' } elseif ($old['role']) { 'host' } else { '' }
if (-not $Role) {
    $answer = Ask $t.AskRole $(if ($oldRole -eq 'client') { '2' } else { '1' })
    $Role = if ($answer -eq '2' -or $answer -ieq 'client') { 'client' } else { 'host' }
}

$lines = New-Object System.Collections.Generic.List[string]
if ($Role -eq 'host') {
    $lines.Add('# G3 Together: this player hosts the campaign (written by the setup).')
} else {
    $lines.Add('# G3 Together: this player joins the host (written by the setup).')
}
$lines.Add("role = $Role")
$lines.Add('transport = steam')
$lines.Add("session_id = $SessionId")
$lines.Add('# Player names are taken from Steam automatically.')
$lines.Add('# Mod messages: auto follows the Gothic 3 text language; en, de or pl can override it.')
$lines.Add('language = auto')
$productionKeys = @(
    'partner_key', 'look', 'puppet_template', 'reputation', 'host_roi', 'host_sim',
    'puppet_lead', 'motion_seek', 'listener_textbox', 'friendly_fire', 'npc_damage',
    'npc_hp', 'steam_host', 'connection', 'ardea_remove_npcs'
)
$kept = @($old.Keys | Where-Object { $productionKeys -contains $_.ToString().ToLower() })
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
[System.IO.File]::WriteAllText($iniPath, (($lines -join "`r`n") + "`r`n"), (New-Object System.Text.UTF8Encoding $false))
Say ($t.IniDone -f $iniPath) 'Green'

Say $(if ($Role -eq 'host') { $t.SteamHost } else { $t.SteamClient }) 'Green'
if ($Role -eq 'host') { Say $t.DoneHost 'Cyan' } else { Say $t.DoneClient 'Cyan' }
exit 0
