<#
.SYNOPSIS
    HIDway setup for Windows: choose what to install on this PC.

.DESCRIPTION
    Every part is installed separately, by its own official installer, so each
    one also updates on its own and nothing third-party is bundled with HIDway:

      HIDway client  this folder (the HIDway release)       the PC you play from
      Tailscale      winget Tailscale.Tailscale              every PC
      Moonlight      winget MoonlightGameStreamingProject... the PC you play from
      Sunshine       winget LizardByte.Sunshine              the gaming PC

    Without winget, the official download page opens instead. The Raspberry Pi
    relay and the Pico firmware are set up from the HIDway app (docs/RELAY.md).

.EXAMPLE
    Setup.cmd
    Interactive: asks which PC this is, then shows the checklist.

.EXAMPLE
    Setup.cmd -Role player -Install HIDway,Tailscale,Moonlight -Yes

.EXAMPLE
    Setup.cmd -Update
    Updates whatever is installed.
#>
[CmdletBinding()]
param(
    [ValidateSet('', 'player', 'gaming')][string]$Role = '',
    [ValidateSet('HIDway', 'Tailscale', 'Moonlight', 'Sunshine')][string[]]$Install = @(),
    [switch]$Update,
    [switch]$Yes,     # no questions: take the defaults
    [switch]$WhatIf,  # show what would happen, change nothing
    [switch]$NoShortcuts, # no Start menu / desktop shortcuts, no autostart (portable)
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA 'Programs\HIDway')
)

Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'

$Components = @(
    [pscustomobject]@{ Key = 'HIDway'; Name = 'HIDway client'; Roles = @('player'); Kind = 'hidway'
        What = 'reads your keyboard and mouse and sends them to the Raspberry Pi'
        Page = 'https://github.com/wannabexaker/HIDway/releases/latest'; Match = $null; Id = $null; License = 'MIT' }
    [pscustomobject]@{ Key = 'Tailscale'; Name = 'Tailscale'; Roles = @('player', 'gaming'); Kind = 'winget'
        What = 'private network between your PCs and the Raspberry Pi'
        Page = 'https://tailscale.com/download/windows'; Match = 'Tailscale'; Id = 'Tailscale.Tailscale'
        License = 'https://tailscale.com/terms' }
    [pscustomobject]@{ Key = 'Moonlight'; Name = 'Moonlight'; Roles = @('player'); Kind = 'winget'
        What = 'shows the gaming PC''s picture and sound on this PC'
        Page = 'https://moonlight-stream.org/'; Match = 'Moonlight Game Streaming Client*'
        Id = 'MoonlightGameStreamingProject.Moonlight'; License = 'GPL-3.0' }
    [pscustomobject]@{ Key = 'Sunshine'; Name = 'Sunshine'; Roles = @('gaming'); Kind = 'winget'
        What = 'sends this PC''s picture and sound to Moonlight'
        Page = 'https://github.com/LizardByte/Sunshine/releases/latest'; Match = 'Sunshine*'
        Id = 'LizardByte.Sunshine'; License = 'GPL-3.0' }
)

# ---------------------------------------------------------------- helpers

function Say([string]$Text, [string]$Color = 'Gray') { Write-Host $Text -ForegroundColor $Color }
function Step([string]$Text) { Write-Host ''; Write-Host "== $Text" -ForegroundColor Cyan }

function Ask([string]$Question, [bool]$Default) {
    if ($Yes) { return $Default }
    $hint = if ($Default) { '[Y/n]' } else { '[y/N]' }
    $a = Read-Host "$Question $hint"
    if ([string]::IsNullOrWhiteSpace($a)) { return $Default }
    return $a.Trim().ToLower().StartsWith('y')
}

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    return ([Security.Principal.WindowsPrincipal]$id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-Winget {
    $w = Get-Command winget.exe -ErrorAction SilentlyContinue
    if ($w) { return $w.Source } else { return $null }
}

# Version of an installed program from the uninstall entries, or $null.
function Get-InstalledVersion($c) {
    if ($c.Kind -eq 'hidway') {
        if (-not (Test-Path (Join-Path $InstallDir 'hidway-client.exe'))) { return $null }
        $v = Join-Path $InstallDir 'VERSION'
        if (Test-Path $v) { return (Get-Content $v -TotalCount 1).Trim() } else { return 'installed' }
    }
    $roots = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*',
             'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*',
             'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*'
    $e = Get-ItemProperty $roots -ErrorAction SilentlyContinue |
        Where-Object { $_.PSObject.Properties['DisplayName'] -and $_.DisplayName -like $c.Match } |
        Select-Object -First 1
    if (-not $e) { return $null }
    if ($e.PSObject.Properties['DisplayVersion'] -and $e.DisplayVersion) { return $e.DisplayVersion }
    return 'installed'
}

# ---------------------------------------------------------------- installers

function Install-WithWinget($c) {
    $winget = Get-Winget
    if (-not $winget) {
        Say "  winget is not available: install $($c.Name) from its official page:" Yellow
        Say "  $($c.Page)" Yellow
        if (-not $WhatIf -and -not $Yes) {
            Start-Process $c.Page
            Read-Host '  Press Enter once it is installed'
        }
        return
    }
    $verb = if ($Update -and (Get-InstalledVersion $c)) { 'upgrade' } else { 'install' }
    $wargs = @($verb, '--id', $c.Id, '--exact', '--source', 'winget',
        '--accept-package-agreements', '--accept-source-agreements')
    Say "  winget $($wargs -join ' ')" DarkGray
    if ($WhatIf) { return }
    & $winget @wargs
    $code = $LASTEXITCODE
    # 0x8A15002B: no newer version / already installed
    if ($code -ne 0 -and $code -ne -1978335189) { throw "$($c.Name): winget failed (exit $code)" }
}

function New-Shortcut([string]$Path, [string]$Target) {
    $sh = New-Object -ComObject WScript.Shell
    $s = $sh.CreateShortcut($Path)
    $s.TargetPath = $Target
    $s.WorkingDirectory = Split-Path $Target
    $s.IconLocation = "$Target,0"
    $s.Description = 'HIDway client'
    $s.Save()
}

function Install-HIDway {
    $src = $PSScriptRoot
    if (-not (Test-Path (Join-Path $src 'hidway-client.exe'))) {
        Say "  hidway-client.exe is not next to this script. Download the HIDway release (zip), unpack it and run Setup.cmd from there:" Yellow
        Say "  $($Components[0].Page)" Yellow
        return
    }
    Say "  into $InstallDir" DarkGray
    if ($WhatIf) { return }

    $running = Get-Process hidway-client -ErrorAction SilentlyContinue
    if ($running) {
        if (-not (Ask '  HIDway is running. Close it to update it?' $true)) { throw 'HIDway is running' }
        $running | ForEach-Object { $_.CloseMainWindow() | Out-Null }
        Start-Sleep 2
        Get-Process hidway-client -ErrorAction SilentlyContinue | Stop-Process -Force
    }
    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
    foreach ($f in 'hidway-client.exe', 'hidway-probe.exe', 'hidway.ini.example', 'VERSION', 'LICENSE', 'README.md') {
        $p = Join-Path $src $f
        if (Test-Path $p) { Copy-Item $p $InstallDir -Force }
    }
    if (Test-Path (Join-Path $src 'firmware')) {
        Copy-Item (Join-Path $src 'firmware') $InstallDir -Recurse -Force
    }
    $ini = Join-Path $InstallDir 'hidway.ini'
    if (-not (Test-Path $ini)) { # your settings are never overwritten
        Copy-Item (Join-Path $InstallDir 'hidway.ini.example') $ini
    }

    $exe = Join-Path $InstallDir 'hidway-client.exe'
    if ($NoShortcuts) { return }
    $menu = Join-Path ([Environment]::GetFolderPath('Programs')) 'HIDway.lnk'
    New-Shortcut $menu $exe
    Say '  Start menu: HIDway' DarkGray
    if (Ask '  Also a shortcut on the desktop?' $true) {
        New-Shortcut (Join-Path ([Environment]::GetFolderPath('Desktop')) 'HIDway.lnk') $exe
    }
    $run = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
    if (Ask '  Start HIDway with Windows?' $false) {
        Set-ItemProperty $run -Name HIDway -Value "`"$exe`""
    } else {
        Remove-ItemProperty $run -Name HIDway -ErrorAction SilentlyContinue
    }
}

# ---------------------------------------------------------------- after install

function After-Tailscale {
    $ts = Join-Path $env:ProgramFiles 'Tailscale\tailscale.exe'
    if (-not (Test-Path $ts)) { return }
    $state = (& $ts status 2>&1 | Out-String)
    if ($state -match 'Logged out|NeedsLogin|not running') {
        Say '  Sign in to Tailscale with the SAME account on every PC and on the Raspberry Pi' Yellow
        Say '  (Tailscale icon in the tray -> Log in).' Yellow
        $ipn = Join-Path $env:ProgramFiles 'Tailscale\tailscale-ipn.exe'
        if (-not $WhatIf -and (Test-Path $ipn) -and -not (Get-Process tailscale-ipn -ErrorAction SilentlyContinue)) {
            Start-Process $ipn
        }
    } else {
        Say '  Tailscale is signed in.' Green
    }
}

function After-Moonlight {
    $k = 'HKCU:\Software\Moonlight Game Streaming Project\Moonlight'
    Say '  Recommended with HIDway: borderless window (the HIDway badge stays on top)' DarkGray
    Say '  and a free mouse pointer (so the badge can be clicked).' DarkGray
    if (-not (Ask '  Apply these Moonlight settings?' $true)) { return }
    if ($WhatIf) { return }
    if (Get-Process Moonlight -ErrorAction SilentlyContinue) {
        Say '  Close Moonlight first, then run Setup again for these settings.' Yellow
        return
    }
    New-Item -Path $k -Force | Out-Null
    New-ItemProperty -Path $k -Name windowmode -Value 1 -PropertyType DWord -Force | Out-Null
    New-ItemProperty -Path $k -Name mouseacceleration -Value 'true' -PropertyType String -Force | Out-Null
    Say '  Moonlight: borderless window, free mouse pointer.' Green
}

function After-Sunshine {
    Say '  Open https://localhost:47990 on THIS PC and create the Sunshine user name and' Yellow
    Say '  password; then pair Moonlight with the PIN it shows.' Yellow
    Say '  Sunshine''s own keyboard/mouse (from Moonlight) is virtual input: fine for the' DarkGray
    Say '  desktop and the login screen, ignored by games that need HIDway.' DarkGray
    if (-not (Ask '  Turn Sunshine''s own keyboard/mouse OFF (HIDway only)?' $false)) { return }
    if ($WhatIf) { return }
    $cmd = {
        $conf = Join-Path $env:ProgramFiles 'Sunshine\config\sunshine.conf'
        New-Item -ItemType File -Force -Path $conf -ErrorAction SilentlyContinue | Out-Null
        Copy-Item $conf "$conf.bak-$(Get-Date -Format yyyyMMdd-HHmmss)"
        $lines = @(Get-Content $conf | Where-Object { $_ -notmatch '^\s*(keyboard|mouse|controller)\s*=' })
        $lines += 'keyboard = disabled', 'mouse = disabled', 'controller = disabled'
        Set-Content -Path $conf -Value $lines -Encoding ASCII
        Restart-Service SunshineService -ErrorAction SilentlyContinue
    }
    if (Test-Admin) {
        & $cmd
    } else { # writes under Program Files: ask Windows for administrator rights once
        $enc = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($cmd.ToString()))
        Start-Process powershell -Verb RunAs -Wait -ArgumentList '-NoProfile', '-EncodedCommand', $enc
    }
    Say '  Sunshine: keyboard/mouse/controller input off (backup kept next to sunshine.conf).' Green
}

# ---------------------------------------------------------------- main

Write-Host ''
Write-Host 'HIDway setup' -ForegroundColor White
Say 'Each part is installed separately with its official installer, so each one updates on its own.' DarkGray
if (-not (Get-Winget)) { Say 'winget not found: the official download pages will open instead.' Yellow }
if ($WhatIf) { Say 'WhatIf: nothing will be changed.' Yellow }

if (-not $Role -and -not $Update -and $Install.Count -eq 0) {
    Write-Host ''
    Say 'Which PC is this?'
    Say '  1  The PC you play FROM (your keyboard and mouse are here)'
    Say '  2  The gaming PC (the game runs here)'
    $Role = if ($Yes) { 'player' } elseif ((Read-Host 'Choose 1 or 2') -eq '2') { 'gaming' } else { 'player' }
}

# The checklist: defaults to what this PC needs and does not have yet; with
# -Update, to whatever is installed.
$rows = foreach ($c in $Components) {
    $ver = Get-InstalledVersion $c
    $on = if ($Install.Count) { $Install -contains $c.Key }
          elseif ($Update) { [bool]$ver }
          else { ($c.Roles -contains $Role) -and -not $ver }
    [pscustomobject]@{ C = $c; Version = $ver; On = $on }
}

while ($true) {
    Write-Host ''
    $label = switch ($Role) { 'gaming' { 'the gaming PC' } 'player' { 'the PC you play from' } default { 'this PC' } }
    Say "What to $(if ($Update) { 'update' } else { 'install' }) on ${label}:" White
    for ($i = 0; $i -lt $rows.Count; $i++) {
        $r = $rows[$i]
        $box = if ($r.On) { '[x]' } else { '[ ]' }
        $state = if ($r.Version) { "installed $($r.Version)" } else { 'not installed' }
        $hint = if ($Role -and -not ($r.C.Roles -contains $Role)) { ' (not needed on this PC)' } else { '' }
        Write-Host ("  {0} {1}  {2,-14} {3,-26} {4}{5}" -f $box, ($i + 1), $r.C.Name, $state, $r.C.What, $hint)
    }
    if ($Yes -or $Install.Count) { break }
    $a = Read-Host 'Numbers to switch on/off (e.g. "1 3"), Enter to continue, q to quit'
    if ($a -match '^\s*q') { Say 'Nothing changed.'; exit 0 }
    if ([string]::IsNullOrWhiteSpace($a)) { break }
    foreach ($n in ($a -split '[\s,]+')) {
        if ($n -match '^\d+$' -and [int]$n -ge 1 -and [int]$n -le $rows.Count) {
            $rows[[int]$n - 1].On = -not $rows[[int]$n - 1].On
        }
    }
}

$todo = @($rows | Where-Object { $_.On })
if ($todo.Count -eq 0) { Say 'Nothing selected.'; exit 0 }

Write-Host ''
Say 'Licences:' DarkGray
foreach ($r in $todo) { Say ("  {0,-14} {1}" -f $r.C.Name, $r.C.License) DarkGray }
if (-not (Ask 'Continue?' $true)) { Say 'Nothing changed.'; exit 0 }

$failed = @()
foreach ($r in $todo) {
    Step $r.C.Name
    try {
        if ($r.C.Kind -eq 'hidway') { Install-HIDway } else { Install-WithWinget $r.C }
        switch ($r.C.Key) {
            'Tailscale' { After-Tailscale }
            'Moonlight' { After-Moonlight }
            'Sunshine' { After-Sunshine }
        }
    } catch {
        Say "  $($_.Exception.Message)" Red
        $failed += $r.C.Name
    }
}

Write-Host ''
if ($failed.Count) {
    Say "Done, with problems: $($failed -join ', '). Run Setup again to retry." Yellow
    exit 1
}
Say 'Done.' Green
if ($Role -eq 'gaming') {
    Say 'Next: plug the Pico into a rear USB port of this PC (see the HIDway README).' DarkGray
} else {
    Say 'Next: open HIDway; the Raspberry Pi and the Pico are set up from there (see the README).' DarkGray
}
exit 0
