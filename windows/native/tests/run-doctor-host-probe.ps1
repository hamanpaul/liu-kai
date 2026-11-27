param(
    [Parameter(Mandatory=$true)][ValidateSet('Start','Snapshot','Stop','AccessCheck')]
    [string]$Action,
    [Parameter(Mandatory=$true)][string]$SettingsExe,
    [string]$OutputRoot = '',
    [string]$RunDir = '',
    [int]$ProcessId = 0,
    [uint64]$ProcessCreation = 0,
    [string]$AccessOutput = ''
)
$ErrorActionPreference='Stop'
$settings=(Resolve-Path -LiteralPath $SettingsExe).Path
switch($Action){
    'Start' {
        if(-not $OutputRoot){throw 'Start requires OutputRoot.'}
        $root=[IO.Path]::GetFullPath($OutputRoot)
        if(-not (Test-Path -LiteralPath $root)){[IO.Directory]::CreateDirectory($root)|Out-Null}
        $run=Join-Path $root ('run-'+[Guid]::NewGuid().ToString('N'))
        $process=Start-Process -FilePath $settings -ArgumentList @(
            '--doctor-probe','collect','--output',('"' + $run + '"')
        ) -PassThru -WindowStyle Hidden
        Start-Sleep -Milliseconds 500
        if($process.HasExited){throw "Doctor collector exited during start: $($process.ExitCode)"}
        & $settings --doctor-probe snapshot --output $run
        [pscustomobject]@{run_dir=$run;process_id=$process.Id;snapshot=Join-Path $run 'snapshot.json'}|ConvertTo-Json
    }
    'Snapshot' {
        if(-not $RunDir){throw 'Snapshot requires RunDir.'}
        & $settings --doctor-probe snapshot --output $RunDir
        if($LASTEXITCODE -ne 0){throw 'Doctor snapshot failed.'}
        Get-Content -LiteralPath (Join-Path $RunDir 'snapshot.json') -Raw
    }
    'Stop' {
        if(-not $RunDir){throw 'Stop requires RunDir.'}
        & $settings --doctor-probe stop --output $RunDir
        if($LASTEXITCODE -ne 0){throw 'Doctor stop request failed.'}
        if($ProcessId){
            $process=$null
            try{$process=Get-Process -Id $ProcessId -ErrorAction Stop}catch{}
            if($process){
                if(-not $process.WaitForExit(10000)){throw "Doctor collector PID $ProcessId did not stop."}
            }
        }
        $statusPath=Join-Path $RunDir 'status.json'
        $status=Get-Content -LiteralPath $statusPath -Raw
        if($status -notmatch '"collectorAlive":0'){throw 'Collector status did not report stopped.'}
        [pscustomobject]@{run_dir=$RunDir;stopped=$true;status=$statusPath}|ConvertTo-Json
    }
    'AccessCheck' {
        if(-not $ProcessId -or -not $ProcessCreation -or -not $AccessOutput){
            throw 'AccessCheck requires ProcessId, ProcessCreation and AccessOutput.'
        }
        & $settings --doctor-probe access-check `
            --pid $ProcessId --process-creation $ProcessCreation --output $AccessOutput
        if($LASTEXITCODE -ne 0){throw 'Doctor ACL access-check failed.'}
        Get-Content -LiteralPath $AccessOutput -Raw
    }
}
