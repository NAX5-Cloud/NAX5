<#
Runs the NAX5 stream replay matrix against a replay-enabled chiaki.exe
(configured with -DNAX5_STREAM_REPLAY=ON) and fails on any crash, hang or
starved decoder.

  run-stream-replay.ps1 -ChiakiExe <build>\gui\chiaki.exe -MediaRoot <dir from make-replay-media.py>
      [-Scenario name[,name...]] [-Repeat 3] [-DllDir dir[,dir...]] [-OutDir dir]

Every scenario runs in operator mode (separate NAX5-Operator profile) against
a dead API URL, so nothing reaches production. Crash dumps written by the
client's crash handler are collected per run.
#>
param(
    [Parameter(Mandatory = $true)][string]$ChiakiExe,
    [Parameter(Mandatory = $true)][string]$MediaRoot,
    [string[]]$Scenario = @(),
    [int]$Repeat = 1,
    [string[]]$DllDir = @(),
    [string]$OutDir = (Join-Path $env:TEMP ('nax5-replay-' + (Get-Date -Format 'yyyyMMdd-HHmmss')))
)
$ErrorActionPreference = 'Stop'

# name, media, seconds, decoder, mic, echo, loss%, corrupt%, burst every s, stall ms, needs_audio
$matrix = @(
    @{ n='h264-720p60-auto';        m='h264-720p60';     s=20; dec='auto';    mic=0; echo=0 },
    @{ n='h264-720p60-software';    m='h264-720p60';     s=20; dec='none';    mic=0; echo=0 },
    @{ n='h264-720p60-d3d11va';     m='h264-720p60';     s=20; dec='d3d11va'; mic=0; echo=0 },
    @{ n='h264-720p60-vulkan';      m='h264-720p60';     s=20; dec='vulkan';  mic=0; echo=0 },
    @{ n='h264-1080p60-auto';       m='h264-1080p60';    s=20; dec='auto';    mic=0; echo=0 },
    @{ n='h264-720p30-auto';        m='h264-720p30';     s=20; dec='auto';    mic=0; echo=0 },
    @{ n='h265-1080p60-auto';       m='h265-1080p60';    s=20; dec='auto';    mic=0; echo=0 },
    @{ n='h265hdr-1080p60-auto';    m='h265hdr-1080p60'; s=20; dec='auto';    mic=0; echo=0 },
    @{ n='mic-on';                  m='h264-720p60';     s=20; dec='auto';    mic=1; echo=0 },
    @{ n='mic-on-echo';             m='h264-720p60';     s=20; dec='auto';    mic=1; echo=1 },
    @{ n='mic-on-echo-no-playback'; m='h264-720p60';     s=20; dec='auto';    mic=1; echo=1; header_only=1 },
    @{ n='loss-2pct';               m='h264-720p60';     s=20; dec='auto';    mic=0; echo=0; loss=2 },
    @{ n='loss-10pct';              m='h264-720p60';     s=20; dec='auto';    mic=0; echo=0; loss=10 },
    @{ n='corrupt-3pct';            m='h264-720p60';     s=20; dec='auto';    mic=0; echo=0; corrupt=3 },
    @{ n='jitter-bursts';           m='h264-720p60';     s=20; dec='auto';    mic=0; echo=0; burst=2 },
    @{ n='stall-3s';                m='h264-720p60';     s=20; dec='auto';    mic=0; echo=0; stall=3000 },
    @{ n='h265-loss-corrupt';       m='h265-1080p60';    s=20; dec='auto';    mic=0; echo=0; loss=5; corrupt=2 }
)
if ($Scenario.Count -gt 0) { $matrix = $matrix | Where-Object { $Scenario -contains $_.n } }
if (-not $matrix) { throw 'no scenario selected' }

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$chiakiDir = Split-Path -Parent (Resolve-Path $ChiakiExe)
$profileDir = Join-Path $env:APPDATA 'NAX5\NAX5-Operator\log'
$dumpDir = Join-Path $profileDir 'crash-dumps'
$results = @()

foreach ($sc in $matrix) {
    foreach ($run in 1..$Repeat) {
        $tag = "$($sc.n)#$run"
        $resultPath = Join-Path $OutDir ("$($sc.n)-$run.json")
        $dumpsBefore = @(if (Test-Path $dumpDir) { Get-ChildItem $dumpDir -Filter *.txt | ForEach-Object Name })
        $started = Get-Date

        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = (Resolve-Path $ChiakiExe).Path
        $psi.WorkingDirectory = $chiakiDir
        $psi.UseShellExecute = $false
        $psi.EnvironmentVariables['PATH'] = (($DllDir + @($chiakiDir, "$env:SystemRoot\System32", $env:SystemRoot)) -join ';')
        $vars = @{
            NAX5_OPERATOR_MODE = '1'; NAX5_API_BASE_URL = 'http://127.0.0.1:9'
            NAX5_REPLAY_DIR = (Join-Path $MediaRoot $sc.m); NAX5_REPLAY_SECONDS = "$($sc.s)"
            NAX5_REPLAY_DECODER = $sc.dec; NAX5_REPLAY_MIC = "$($sc.mic)"; NAX5_REPLAY_ECHO = "$($sc.echo)"
            NAX5_REPLAY_LOSS_PCT = "$([int]$sc.loss)"; NAX5_REPLAY_CORRUPT_PCT = "$([int]$sc.corrupt)"
            NAX5_REPLAY_BURST_EVERY_S = "$([int]$sc.burst)"; NAX5_REPLAY_STALL_MS = "$([int]$sc.stall)"
            NAX5_REPLAY_RESULT = $resultPath
            NAX5_REPLAY_AUDIO_HEADER_ONLY = "$([int]$sc.header_only)"
        }
        foreach ($k in $vars.Keys) { $psi.EnvironmentVariables[$k] = $vars[$k] }

        $proc = [System.Diagnostics.Process]::Start($psi)
        $finished = $proc.WaitForExit(($sc.s + 45) * 1000)
        if (-not $finished) { $proc.Kill(); $proc.WaitForExit(5000) | Out-Null }
        Start-Sleep -Milliseconds 500

        $r = if (Test-Path $resultPath) { Get-Content $resultPath -Raw | ConvertFrom-Json } else { $null }
        $newDumps = @(if (Test-Path $dumpDir) { Get-ChildItem $dumpDir -Filter *.txt | Where-Object { $dumpsBefore -notcontains $_.Name } })
        $log = Get-ChildItem $profileDir -Filter 'chiaki_session_*.log' | Where-Object { $_.LastWriteTime -ge $started } |
            Sort-Object LastWriteTime | Select-Object -Last 1
        $logText = if ($log) { Get-Content $log.FullName -Raw } else { '' }
        $decoderLine = ([regex]::Match($logText, 'Using (hardware decoder "[^"]+"|software decoder[^\r\n]*)')).Value
        $micFailed = $logText -match 'Failed to open (default )?Microphone'

        $problems = @()
        if (-not $finished) { $problems += 'hang' }
        elseif ($proc.ExitCode -ne 0) { $problems += ('exit 0x{0:X8}' -f $proc.ExitCode) }
        if ($newDumps.Count -gt 0) { $problems += "crash dump x$($newDumps.Count)" }
        if (-not $r) { $problems += 'no result' }
        elseif ($r.outcome -ne 'completed') { $problems += "outcome $($r.outcome)" }
        else {
            $expected = [math]::Max(1, $r.video_sent - $r.video_rejected_by_decoder)
            $ratio = $r.decoded_frames / $expected
            $floor = if ($sc.loss -or $sc.corrupt) { 0.5 } else { 0.95 }
            if ($ratio -lt $floor) { $problems += ('decoded {0:P0} of sent' -f $ratio) }
            if (($r.audio_sent -gt 0 -or $sc.header_only) -and $logText -notmatch "Audio Device '.*' opened") { $problems += 'audio device not opened' }
            if ($sc.mic -eq 1 -and -not $micFailed -and $r.mic_frames -lt 100) { $problems += "mic frames $($r.mic_frames)" }
        }
        foreach ($d in $newDumps) {
            Copy-Item $d.FullName -Destination (Join-Path $OutDir "$($sc.n)-$run-$($d.Name)")
            $dmp = [IO.Path]::ChangeExtension($d.FullName, '.dmp')
            if (Test-Path $dmp) { Move-Item $dmp -Destination (Join-Path $OutDir "$($sc.n)-$run-$([IO.Path]::GetFileName($dmp))") -Force }
            Remove-Item $d.FullName -Force
        }
        if ($log) { Copy-Item $log.FullName -Destination (Join-Path $OutDir "$($sc.n)-$run.log") }

        $row = [pscustomobject]@{
            scenario = $tag
            result = if ($problems) { 'FAIL' } else { 'PASS' }
            sent = $r.video_sent; decoded = $r.decoded_frames; dropped = $r.video_dropped
            audio = $r.audio_sent; mic = if ($sc.mic -eq 1 -and $micFailed) { 'no-device' } else { $r.mic_frames }
            decoder = $decoderLine -replace 'Using ', ''
            note = if ($sc.dec -notin @('auto', 'none') -and $decoderLine -notmatch [regex]::Escape($sc.dec)) { "requested $($sc.dec) unavailable here, fell back" } else { '' }
            problems = ($problems -join '; ')
        }
        $results += $row
        Write-Host ('{0,-28} {1}  sent={2} decoded={3} audio={4} mic={5} {6} {7} {8}' -f $row.scenario, $row.result, $row.sent, $row.decoded, $row.audio, $row.mic, $row.decoder, $row.problems, $row.note)
    }
}

$results | ConvertTo-Json -Depth 3 | Set-Content (Join-Path $OutDir 'summary.json') -Encoding utf8
$failed = @($results | Where-Object result -eq 'FAIL').Count
Write-Host "`n$($results.Count - $failed)/$($results.Count) passed. Artifacts: $OutDir"
if ($failed -gt 0) { exit 1 }
