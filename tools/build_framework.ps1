param(
    [ValidateRange(1,16)][int]$Jobs = 2,
    [switch]$SkipTests
)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$sourceDir = Join-Path $taskRoot 'third_party\PSPRecomp'
$buildDir = Join-Path $taskRoot 'build\framework'
$cmake = Join-Path $taskRoot 'third_party\python_build_deps\cmake\data\bin\cmake.exe'
$ctest = Join-Path $taskRoot 'third_party\python_build_deps\cmake\data\bin\ctest.exe'
$ninja = Join-Path $taskRoot 'third_party\python_build_deps\bin\ninja.exe'
$toolchain = Join-Path $taskRoot 'third_party\toolchain\zig-windows.cmake'
$zig = Join-Path $taskRoot 'third_party\toolchain\zig-x86_64-windows-0.15.2\zig.exe'
$python = if ($env:KON_PYTHON) { $env:KON_PYTHON } else { (Get-Command python -ErrorAction Stop).Source }
# PSPRecomp: the pinned upstream revision plus patches/psprecomp/kon-port.patch.
$patchTool = Join-Path $taskRoot 'tools\psprecomp_patch.py'
if (-not (Test-Path -LiteralPath $sourceDir)) {
    & $python $patchTool fetch
    if ($LASTEXITCODE -ne 0) { throw 'Could not fetch PSPRecomp (tools/psprecomp_patch.py fetch)' }
}
& $python $patchTool check
if ($LASTEXITCODE -ne 0) { Write-Warning 'third_party/PSPRecomp has changes that are not in patches/psprecomp/kon-port.patch: run "python tools/psprecomp_patch.py write" before exporting.' }
foreach ($required in @($sourceDir,$cmake,$ctest,$ninja,$toolchain,$zig)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Required local dependency missing: $required" }
}
New-Item -ItemType Directory -Path $buildDir -Force | Out-Null
$oldLocalCache = $env:ZIG_LOCAL_CACHE_DIR
$oldGlobalCache = $env:ZIG_GLOBAL_CACHE_DIR
$oldTemp = $env:TEMP
$oldTmp = $env:TMP
$env:ZIG_LOCAL_CACHE_DIR = Join-Path $taskRoot 'work\zig-cache'
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $taskRoot 'work\zig-global-cache'
$env:TEMP = Join-Path $taskRoot 'work\framework-temp'
$env:TMP = $env:TEMP
New-Item -ItemType Directory -Path $env:ZIG_LOCAL_CACHE_DIR,$env:ZIG_GLOBAL_CACHE_DIR,$env:TEMP -Force | Out-Null
$logPath = Join-Path $buildDir 'build_framework.log'
"Generic PSPRecomp build started at $([DateTimeOffset]::Now.ToString('o'))" | Set-Content -LiteralPath $logPath
function Invoke-Checked {
    param([string]$Executable,[string[]]$Arguments)
    "COMMAND: $Executable $($Arguments -join ' ')" | Tee-Object -FilePath $logPath -Append
    & $Executable @Arguments 2>&1 | Tee-Object -FilePath $logPath -Append
    if ($LASTEXITCODE -ne 0) { throw "Command failed with exit code $LASTEXITCODE; see $logPath" }
}
try {
    # The five scripts that first wrote the port's changes (tools/apply_framework_patches.py,
    # extend_framework.py, fix_import_codegen.py, fix_break_delay_slot.py,
    # add_scratchpad_memory.py) are part of kon-port.patch and later edits; they
    # no longer apply to the current sources and are not run.
    Invoke-Checked $zig @('version')
    Invoke-Checked $cmake @('--version')
    Invoke-Checked $ninja @('--version')
    Invoke-Checked $cmake @('-S',$sourceDir,'-B',$buildDir,'-G','Ninja',"-DCMAKE_MAKE_PROGRAM=$ninja","-DCMAKE_TOOLCHAIN_FILE=$toolchain",'-DCMAKE_BUILD_TYPE=Release','-DCMAKE_CXX_FLAGS=-g0','-DPSPRECOMP_PROFILE=','-DPSPRECOMP_BUILD_TESTS=ON','-DPSPRECOMP_BUILD_PROFILE_TESTS=OFF')
    Invoke-Checked $cmake @('--build',$buildDir,'--parallel',"$Jobs")
    if (-not $SkipTests) {
        Invoke-Checked $ctest @('--test-dir',$buildDir,'--output-on-failure')
    }
    "Generic framework build completed at $([DateTimeOffset]::Now.ToString('o'))" | Tee-Object -FilePath $logPath -Append
} finally {
    $env:ZIG_LOCAL_CACHE_DIR = $oldLocalCache
    $env:ZIG_GLOBAL_CACHE_DIR = $oldGlobalCache
    $env:TEMP = $oldTemp
    $env:TMP = $oldTmp
}


