param([ValidateRange(1,8)][int]$Jobs=2,[switch]$FullAot)
$ErrorActionPreference='Stop'
$taskRoot=Split-Path -Parent $PSScriptRoot
$taskBuild=Join-Path $taskRoot ($(if($FullAot){'build\native_full'}else{'build\native'}))
$cmake=Join-Path $taskRoot 'third_party\python_build_deps\cmake\data\bin\cmake.exe'
$ctest=Join-Path $taskRoot 'third_party\python_build_deps\cmake\data\bin\ctest.exe'
$ninja=Join-Path $taskRoot 'third_party\python_build_deps\bin\ninja.exe'
$toolchain=Join-Path $taskRoot 'third_party\toolchain\zig-windows.cmake'
$codegen=Join-Path $taskRoot 'build\framework\psp_recomp.exe'
$python=if($env:KON_PYTHON){$env:KON_PYTHON}else{(Get-Command python -ErrorAction Stop).Source}
$oldLocal=$env:ZIG_LOCAL_CACHE_DIR;$oldGlobal=$env:ZIG_GLOBAL_CACHE_DIR
$oldTemp=$env:TEMP;$oldTmp=$env:TMP
$env:ZIG_LOCAL_CACHE_DIR=Join-Path $taskRoot 'work\zig-cache'
$env:ZIG_GLOBAL_CACHE_DIR=Join-Path $taskRoot 'work\zig-global-cache'
$env:TEMP=Join-Path $taskRoot 'work\native-temp';$env:TMP=$env:TEMP
New-Item -ItemType Directory -Path $taskBuild,$env:TEMP -Force | Out-Null
$logPath=Join-Path $taskBuild 'build_native.log'
function Invoke-Checked {param([string]$Executable,[string[]]$Arguments)
    & $Executable @Arguments 2>&1 | Tee-Object -FilePath $logPath -Append
    if($LASTEXITCODE -ne 0){throw "Build step failed ($LASTEXITCODE): $Executable"}
}
try {
    "Native diagnostic build started $([DateTimeOffset]::Now.ToString('o'))" | Set-Content -LiteralPath $logPath
    # Refresh the imported runtime library after local memory/runtime changes.
    Invoke-Checked $cmake @('--build',(Join-Path $taskRoot 'build\framework'),'--target','psprecomp_core','--parallel',"$Jobs")
    Invoke-Checked $python @((Join-Path $taskRoot 'tools\create_instruction_fixture.py'))
    Invoke-Checked $codegen @((Join-Path $taskRoot 'work\instruction_fixture\fixture.elf'),(Join-Path $taskRoot 'work\instruction_fixture\functions.csv'),(Join-Path $taskRoot 'work\instruction_fixture\guest.cpp'))
    if(-not $FullAot){Invoke-Checked $codegen @((Join-Path $taskRoot 'analysis\executables\pkon.elf'),(Join-Path $taskRoot 'tools\bootstrap_functions.csv'),(Join-Path $taskRoot 'work\bootstrap\guest.cpp'))}
    Invoke-Checked $python @((Join-Path $taskRoot 'tools\create_import_fixture.py'))
    $fullOption=$(if($FullAot){'ON'}else{'OFF'})
    Invoke-Checked $cmake @('-S',$taskRoot,'-B',$taskBuild,'-G','Ninja',"-DCMAKE_MAKE_PROGRAM=$ninja","-DCMAKE_TOOLCHAIN_FILE=$toolchain",'-DCMAKE_BUILD_TYPE=Release',"-DKON_FULL_AOT=$fullOption","-DPython3_EXECUTABLE=$python")
    Invoke-Checked $cmake @('--build',$taskBuild,'--parallel',"$Jobs")
    Invoke-Checked $ctest @('--test-dir',$taskBuild,'--output-on-failure')
} finally {
    $env:ZIG_LOCAL_CACHE_DIR=$oldLocal;$env:ZIG_GLOBAL_CACHE_DIR=$oldGlobal
    $env:TEMP=$oldTemp;$env:TMP=$oldTmp
}
