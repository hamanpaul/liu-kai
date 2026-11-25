param([Parameter(Mandatory=$true)][string]$ToolchainBin,
      [Parameter(Mandatory=$true)][string]$WorkDir)
$ErrorActionPreference='Stop'
$compiler=Join-Path (Resolve-Path -LiteralPath $ToolchainBin).Path 'g++.exe'
$work=[IO.Path]::GetFullPath($WorkDir)
if(Test-Path -LiteralPath $work){throw 'Test directory already exists.'}
[IO.Directory]::CreateDirectory($work)|Out-Null
$exe=Join-Path $work 'tsf_uiless_input_tests.exe'
& $compiler -std=c++17 -O2 -static -municode `
    (Join-Path $PSScriptRoot 'tsf_uiless_input_tests.cpp') `
    -lole32 -loleaut32 -luuid -lgdi32 -ladvapi32 -lshlwapi -o $exe
if($LASTEXITCODE -ne 0){throw 'UI-less TSF harness compilation failed.'}

function Invoke-Fixture([string]$Name,[string[]]$ExtraArguments,[string]$ExpectedOutput) {
    $sandbox=Join-Path $work ($Name+'-'+[Guid]::NewGuid().ToString('N'))
    [IO.Directory]::CreateDirectory($sandbox)|Out-Null
    $test=Join-Path $sandbox 'test.exe'
    Copy-Item -LiteralPath $exe -Destination $test
    if((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash -cne
       (Get-FileHash -LiteralPath $test -Algorithm SHA256).Hash){throw 'Harness executable copy mismatch.'}
    $stdout=Join-Path $sandbox 'stdout.txt';$stderr=Join-Path $sandbox 'stderr.txt'
    $arguments=@(('"'+$sandbox+'"'))+$ExtraArguments
    $child=Start-Process -FilePath $test -ArgumentList $arguments -WindowStyle Hidden -Wait -PassThru `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    $output=if(Test-Path -LiteralPath $stdout){Get-Content -LiteralPath $stdout -Raw}else{''}
    if($null -eq $output){$output=''}
    if($child.ExitCode -ne 0 -or $output.Trim() -cne $ExpectedOutput){
        $failure=if(Test-Path -LiteralPath $stderr){Get-Content -LiteralPath $stderr -Raw}else{''}
        throw ('UI-less TSF fixture failed: '+$failure)
    }
    [pscustomobject]@{fixture=$Name;test_sandbox=$sandbox;result=$output.Trim()}
}

$synthetic=Invoke-Fixture 'tsf-uiless-fixture-synthetic' @() `
    'UI-less TSF fixture routed simulated Zhuyin key events, tones, annotations, candidate navigation, repeats and following input; no TIP was registered'
$dictionary=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..\assets\dictionary.tsv')).Path
$supplied=Invoke-Fixture 'tsf-uiless-fixture-supplied' @(('"'+$dictionary+'"')) `
    'UI-less TSF fixture committed two Chinese characters using supplied dictionary; no TIP was registered'
@($synthetic,$supplied)|ConvertTo-Json
