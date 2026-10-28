param([switch]$Plain)
$ErrorActionPreference='Stop'
$taskRoot=Split-Path -Parent $PSScriptRoot
$elf=Join-Path $taskRoot 'analysis\executables\pkon.elf'
$codegen=Join-Path $taskRoot 'build\framework\psp_recomp.exe'
$previousPlain=$env:PSPRECOMP_PLAIN_AUTO
if($Plain){$env:PSPRECOMP_PLAIN_AUTO='1'}
$output=Join-Path $taskRoot 'work\recompiled_corrected'
$expected='EDA890135EA4AEE1A8D0A82B2F00A7159060C58498A6705DF7518B36E46B85B1'
if((Get-FileHash -LiteralPath $elf -Algorithm SHA256).Hash -ne $expected){throw 'Unexpected ELF identity'}
& $codegen $elf --auto $output 0x08804000 16384 2>&1 | Tee-Object -FilePath (Join-Path $taskRoot 'analysis\codegen_fixed_progress.log')
$env:PSPRECOMP_PLAIN_AUTO=$previousPlain
if($LASTEXITCODE -ne 0){throw "Automatic generation failed ($LASTEXITCODE)"}
# PC FPS patch: per-frame time step becomes a runtime variable.
$python=if($env:KON_PYTHON){$env:KON_PYTHON}else{(Get-Command python -ErrorAction Stop).Source}
& $python (Join-Path $PSScriptRoot 'apply_logic_rate_patch.py')
if($LASTEXITCODE -ne 0){throw 'Logic rate patch failed'}
$report=Get-Content -LiteralPath (Join-Path $output 'auto_codegen_report.json') -Raw | ConvertFrom-Json
if($report.import_wrappers -ne 238){throw 'Generated import table mismatch'}
$metadata=[ordered]@{
    generated_at=[DateTimeOffset]::Now.ToString('o');elf_sha256=$expected.ToLower()
    framework_revision='f6e7d415c7f447b934cc3865a31eb725f353d659'
    framework_patch='patches/psprecomp/kon-port.patch'
    lowering_optimizations=(-not $Plain);report=$report;playable_port=$false
}
$metadata | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $taskRoot 'analysis\native_codegen_provenance.json') -Encoding utf8
