<#
Client <-> backend integration test on a LOCAL backend.

  run-backend-integration.ps1 -BackendDir C:\astro\nax5-backend `
      -IntegrationExe <build>\gui\nax5-integration.exe [-DbPort 55433] [-DllDir C:\msys64\mingw64\bin]

Needs the backend's own PostgreSQL:  $env:POSTGRES_HOST_PORT=55433; docker compose up -d --wait
(in nax5-backend). Seeds two users and one READY console, starts runserver on
127.0.0.1:8765 with short leases, runs the client's real HTTP code against it,
then checks what the backend recorded. A second phase restarts the server with
a minimum client build of 99 to check the update-required path.
#>
param(
    [Parameter(Mandatory = $true)][string]$BackendDir,
    [Parameter(Mandatory = $true)][string]$IntegrationExe,
    [int]$DbPort = 55433,
    [int]$Port = 8765,
    [int]$LeaseSeconds = 6,
    [string[]]$DllDir = @('C:\msys64\mingw64\bin')
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$python = Join-Path $BackendDir '.venv\Scripts\python.exe'
$work = Join-Path $env:TEMP ('nax5-it-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null

$suffix = [Guid]::NewGuid().ToString('N').Substring(0, 8)
$backendEnv = @{
    DATABASE_URL = "postgresql://nax5:nax5_dev_password@127.0.0.1:$DbPort/nax5"
    DEBUG = 'true'; SECURE_SSL_REDIRECT = 'false'
    CONNECTION_MATERIAL_DIR = (Join-Path $work 'material')
    SESSION_RESERVATION_LEASE_SECONDS = "$LeaseSeconds"
    SESSION_CONNECTING_LEASE_SECONDS = "$LeaseSeconds"
    SESSION_ACTIVE_LEASE_SECONDS = "$LeaseSeconds"
    NAX5_IT_USER_A = "it-a-$suffix@example.test"; NAX5_IT_USER_B = "it-b-$suffix@example.test"
    NAX5_IT_PASSWORD = 'It-' + [Guid]::NewGuid().ToString('N')
    NAX5_IT_CONSOLE = 'PS5-IT'
}
foreach ($k in $backendEnv.Keys) { Set-Item "env:$k" $backendEnv[$k] }

function Invoke-Manage([string[]]$ManageArgs) {
    Push-Location $BackendDir
    # Django logs INFO lines to stderr; Windows PowerShell would turn them into errors.
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $out = & $python manage.py @ManageArgs 2>&1 | ForEach-Object { "$_" } | Out-String
        $ErrorActionPreference = $previous
        if ($LASTEXITCODE -ne 0) { throw "manage.py $($ManageArgs -join ' ') failed:`n$out" }
        return $out
    } finally { $ErrorActionPreference = $previous; Pop-Location }
}

function Start-Backend([hashtable]$Extra) {
    foreach ($k in $Extra.Keys) { Set-Item "env:$k" $Extra[$k] }
    $log = Join-Path $work ("runserver-" + (Get-Date -Format 'HHmmss') + ".log")
    $p = Start-Process -FilePath $python -ArgumentList @('manage.py', 'runserver', "127.0.0.1:$Port", '--noreload') `
        -WorkingDirectory $BackendDir -PassThru -RedirectStandardOutput $log -RedirectStandardError "$log.err" -WindowStyle Hidden
    for ($i = 0; $i -lt 60; $i++) {
        Start-Sleep -Milliseconds 500
        try { if ((Invoke-WebRequest "http://127.0.0.1:$Port/health/ready" -UseBasicParsing -TimeoutSec 2).StatusCode -eq 200) { return $p } } catch {}
        if ($p.HasExited) { throw "runserver exited:`n$(Get-Content "$log.err" -Raw)" }
    }
    throw 'runserver did not become ready'
}

function Invoke-Client([string]$Phase) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = (Resolve-Path $IntegrationExe).Path
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.EnvironmentVariables['PATH'] = (($DllDir + @((Split-Path $psi.FileName), "$env:SystemRoot\System32")) -join ';')
    $psi.EnvironmentVariables['NAX5_API_BASE_URL'] = "http://127.0.0.1:$Port"
    $psi.EnvironmentVariables['NAX5_IT_PHASE'] = $Phase
    $psi.EnvironmentVariables['NAX5_IT_LEASE_SECONDS'] = "$LeaseSeconds"
    foreach ($k in 'NAX5_IT_USER_A', 'NAX5_IT_USER_B', 'NAX5_IT_PASSWORD', 'NAX5_IT_CONSOLE') { $psi.EnvironmentVariables[$k] = $backendEnv[$k] }
    $p = [System.Diagnostics.Process]::Start($psi)
    $out = $p.StandardOutput.ReadToEnd()
    $p.WaitForExit()
    Write-Host $out
    return @{ code = $p.ExitCode; out = $out }
}

$failures = @()
$server = $null
try {
    Invoke-Manage @('migrate', '--noinput') | Out-Null
    $seed = Join-Path $here 'seed_backend.py'
    Write-Host (Invoke-Manage @('shell', '-c', "exec(open(r'$seed', encoding='utf-8').read())")).Trim()

    $server = Start-Backend @{ NAX5_MINIMUM_CLIENT_BUILD = '10' }
    $main = Invoke-Client 'main'
    if ($main.code -ne 0) { $failures += 'client main phase' }

    $idA1 = ([regex]::Match($main.out, 'SESSION_A1=([0-9a-f-]{36})')).Groups[1].Value
    $idB = ([regex]::Match($main.out, 'SESSION_B=([0-9a-f-]{36})')).Groups[1].Value
    $env:NAX5_IT_SESSIONS = (@($idA1, $idB) | Where-Object { $_ }) -join ','
    $check = Join-Path $here 'check_backend.py'
    $checkOut = Invoke-Manage @('shell', '-c', "exec(open(r'$check', encoding='utf-8').read())")
    $json = ([regex]::Match($checkOut, 'CHECK_JSON=(.*)')).Groups[1].Value | ConvertFrom-Json
    $a1 = if ($idA1) { $json.sessions.$idA1 } else { $null }; $b = if ($idB) { $json.sessions.$idB } else { $null }
    $checks = @(
        @{ ok = ($a1.status -eq 'ENDED' -and $a1.played_seconds -gt 0); name = "A's played session is ENDED with played time"; detail = "$($a1.status) $($a1.played_seconds)s" },
        @{ ok = ($b.status -eq 'EXPIRED' -and $b.end_reason_code -eq 'HEARTBEAT_LOST'); name = "B's dropped session is EXPIRED as HEARTBEAT_LOST"; detail = "$($b.status) reason=$($b.end_reason_code)" },
        @{ ok = ($b.played_seconds -ge 1 -and $b.played_seconds -le 4); name = "B's play time stops at its last heartbeat (~2 s), not at expiry"; detail = "$($b.played_seconds)s" },
        @{ ok = ($json.play_requested_events -ge 1); name = 'client event stored by the backend'; detail = "$($json.play_requested_events)" },
        @{ ok = ($null -ne $json.crash_report -and $json.crash_report.is_archive); name = 'crash report stored as archive'; detail = '' },
        @{ ok = ($json.crash_report.preview_has_summary -and $json.crash_report.preview_has_dump_placeholder); name = 'admin preview shows crash summary and dump placeholder'; detail = '' }
    )
    foreach ($c in $checks) {
        Write-Host ('{0} {1} {2}' -f ($(if ($c.ok) { 'ok' } else { 'FAIL' }), $c.name, $(if ($c.detail) { "-- $($c.detail)" } else { '' })))
        if (-not $c.ok) { $failures += $c.name }
    }

    Stop-Process -Id $server.Id -Force; $server = $null
    $server = Start-Backend @{ NAX5_MINIMUM_CLIENT_BUILD = '99' }
    $update = Invoke-Client 'update-required'
    if ($update.code -ne 0) { $failures += 'client update-required phase' }
} finally {
    if ($server -and -not $server.HasExited) { Stop-Process -Id $server.Id -Force }
    Write-Host "backend logs: $work"
}
if ($failures.Count -gt 0) { Write-Host "`nFAILED: $($failures -join '; ')"; exit 1 }
Write-Host "`nintegration: all checks passed"
