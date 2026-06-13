<#
  VoiceChat v2 - one-shot build + deploy.

  Builds the three native targets (relay .exe, SA-MP .dll, client .asi), compiles
  the test filterscript, and deploys everything to the local Test-Server + each
  GTA SA install (with timestamped .bak-<stamp> backups of whatever it overwrites).

  Usage (from the repo root):
      powershell -ExecutionPolicy Bypass -File .\build.ps1            # build + deploy
      powershell -ExecutionPolicy Bypass -File .\build.ps1 -NoDeploy  # build only
      powershell -ExecutionPolicy Bypass -File .\build.ps1 -Configure # force a fresh cmake configure
      powershell -ExecutionPolicy Bypass -File .\build.ps1 -NoPawn    # skip the filterscript

  Edit the CONFIG block below if your paths differ. Locked .dll/.exe/.asi (server
  or GTA running) are skipped with a warning, not a hard error.
#>
[CmdletBinding()]
param(
    [switch]$Configure,   # force a fresh cmake configure before building
    [switch]$NoDeploy,    # build only; don't copy to the server/game folders
    [switch]$NoPawn       # skip compiling the test filterscript
)
$ErrorActionPreference = 'Stop'

# ---- CONFIG (edit for your machine) -----------------------------------------
$Root      = $PSScriptRoot
$BuildDir  = Join-Path $Root 'build-deploy'
$Generator = 'Visual Studio 18 2026'

$CMake = 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not (Test-Path $CMake)) { $CMake = (Get-Command cmake -ErrorAction SilentlyContinue).Source }
if (-not $CMake) { throw 'cmake not found - set $CMake in build.ps1' }

$TestServer = 'C:\Users\Staark\Desktop\Test-Server'
$Pawncc     = Join-Path $TestServer 'qawno\pawncc.exe'
$PawnInc    = Join-Path $TestServer 'qawno\include'
$FsSource   = Join-Path $Root 'examples\voicechat_test.pwn'

# Every GTA SA install / staging folder that should get the fresh .asi.
$AsiTargets = @(
  'C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto San Andreas\VoiceChat.asi',
  'C:\Users\Staark\Desktop\lostsand-launcher\mods\asi\VoiceChat.asi',
  'C:\Users\Staark\Desktop\voicechat-deploy\PC-Simona (192.168.0.184)\VoiceChat.asi',
  'C:\Users\Staark\Desktop\voicechat-deploy\PC-Staark (192.168.0.178)\VoiceChat.asi',
  'C:\Users\Staark\Desktop\voicechat-deploy\v2-build\client\VoiceChat.asi'
)
# -----------------------------------------------------------------------------

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$script:Locked = 0

function Deploy($src, $dst) {
    if (-not (Test-Path $src)) { Write-Warning "  missing source: $src"; return }
    $dir = Split-Path $dst
    if (-not (Test-Path $dir)) { Write-Host "  skip (no dir): $dst" -ForegroundColor DarkGray; return }
    try {
        if (Test-Path $dst) { Copy-Item -LiteralPath $dst -Destination "$dst.bak-$stamp" -Force -ErrorAction Stop }
        Copy-Item -LiteralPath $src -Destination $dst -Force -ErrorAction Stop
        Write-Host "  -> $dst"
    } catch {
        # .dll/.exe are locked while omp-server (or GTA) runs - skip, don't abort.
        $script:Locked++
        Write-Warning "  LOCKED (in use, skipped): $dst"
    }
}

# ---- 1. Configure (first run, or -Configure) --------------------------------
if ($Configure -or -not (Test-Path (Join-Path $BuildDir 'CMakeCache.txt'))) {
    Write-Host '== cmake configure (Win32) ==' -ForegroundColor Cyan
    & $CMake -S $Root -B $BuildDir -G $Generator -A Win32
    if ($LASTEXITCODE) { throw 'cmake configure failed' }
}

# ---- 2. Build the native targets --------------------------------------------
foreach ($t in @('voice_relay', 'VoiceChatSamp', 'VoiceChatASI')) {
    Write-Host "== build $t ==" -ForegroundColor Cyan
    & $CMake --build $BuildDir --target $t --config Release | Select-Object -Last 1
    if ($LASTEXITCODE) { throw "build $t failed (re-run without the pipe to see the errors)" }
}

# ---- 3. Compile the test filterscript ---------------------------------------
if (-not $NoPawn -and (Test-Path $Pawncc)) {
    Write-Host '== pawncc voicechat_test ==' -ForegroundColor Cyan
    Copy-Item (Join-Path $Root 'samp\include\voicechat.inc')    (Join-Path $PawnInc 'voicechat.inc')    -Force
    Copy-Item (Join-Path $Root 'samp\include\voicechat_ui.inc') (Join-Path $PawnInc 'voicechat_ui.inc') -Force
    $fsOut = Join-Path $TestServer 'filterscripts\voicechat_test.amx'
    if (Test-Path $fsOut) { Copy-Item $fsOut "$fsOut.bak-$stamp" -Force }
    & $Pawncc $FsSource "-i$PawnInc" "-o$fsOut" -d3 | Select-Object -Last 1
    if ($LASTEXITCODE) { throw 'pawncc failed' }
    Write-Host "  -> $fsOut"
}

# ---- 4. Deploy --------------------------------------------------------------
if (-not $NoDeploy) {
    $dll = Join-Path $BuildDir 'samp\Release\VoiceChat.dll'
    $exe = Join-Path $BuildDir 'relay\Release\voice_relay.exe'
    $asi = Join-Path $BuildDir 'client\Release\VoiceChat.asi'

    Write-Host '== deploy: server ==' -ForegroundColor Cyan
    Deploy $dll (Join-Path $TestServer 'plugins\VoiceChat.dll')
    Deploy $exe (Join-Path $TestServer 'components\voice_relay.exe')
    Deploy (Join-Path $Root 'samp\include\voicechat.inc')    (Join-Path $TestServer 'qawno\include\voicechat.inc')
    Deploy (Join-Path $Root 'samp\include\voicechat_ui.inc') (Join-Path $TestServer 'qawno\include\voicechat_ui.inc')
    Deploy (Join-Path $Root 'samp\include\voicechat.inc')    (Join-Path $TestServer 'pawno\include\voicechat.inc')
    Deploy (Join-Path $Root 'samp\include\voicechat_ui.inc') (Join-Path $TestServer 'pawno\include\voicechat_ui.inc')
    Deploy $dll 'C:\Users\Staark\Desktop\voicechat-deploy\v2-build\VoiceChat.dll'
    Deploy $exe 'C:\Users\Staark\Desktop\voicechat-deploy\v2-build\voice_relay.exe'

    Write-Host '== deploy: client .asi ==' -ForegroundColor Cyan
    foreach ($t in $AsiTargets) { Deploy $asi $t }
}

Write-Host "`nDONE ($stamp)." -ForegroundColor Green
if ($script:Locked -gt 0) {
    Write-Host "$($script:Locked) file(s) were LOCKED and skipped - omp-server.exe (and/or GTA) is running." -ForegroundColor Red
    Write-Host 'Stop them and re-run build.ps1 to deploy the .dll/relay/.asi that were in use.' -ForegroundColor Red
}
Write-Host 'Reminder: restart omp-server (loads new .dll/relay + voice.ini), then RCON reloadfs voicechat_test.' -ForegroundColor Yellow
