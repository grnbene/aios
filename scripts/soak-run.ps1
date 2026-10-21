# Run the git soak unattended, at the current commit, until -Hours of soak time have been banked.
#
#   scripts/soak-run.ps1 -Hours 8
#   scripts/soak-run.ps1 -Hours 8 -SkipKind diff     # exploring while a known finding is open; never counts for a release
#
# Restarts the soak if it dies (the next start turns the interrupted operation into a "crash"
# finding, so a crash is never silent). Stops at once on a discrepancy (exit 1) or an
# environment failure (exit 3): neither is something to retry. Create $Root\STOP to stop cleanly.
param(
  [double]$Hours = 8,
  [string]$Root = "C:\Dev\git-soak",
  [string[]]$SkipKind = @(),
  [string]$Only = "",
  [int]$InjectFault = 0
)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$aios = Resolve-Path (Join-Path $here "..")
$exe = Join-Path $aios "build-win\Release\aios-git-soak.exe"
if (-not (Test-Path $exe)) { throw "build it first: cmake --build build-win --config Release --target aios-git-soak" }
$commit = (git -C $aios rev-parse HEAD).Trim()
$dirty = if ((git -C $aios status --porcelain --untracked-files=no -- .).Trim()) { 1 } else { 0 }
$def = Join-Path $aios "tools\proof\git-soak\corpus.tsv"
New-Item -ItemType Directory -Force (Join-Path $Root "logs") | Out-Null
$stop = Join-Path $Root "STOP"
Remove-Item $stop -ErrorAction SilentlyContinue
$deadline = (Get-Date).AddHours($Hours)
Write-Host "[soak-run] commit $commit dirty=$dirty, $Hours h, log dir $Root\logs"
while ((Get-Date) -lt $deadline) {
  $left = [math]::Round(($deadline - (Get-Date)).TotalHours, 4)
  $a = @("--corpus", "$Root\corpus", "--corpus-def", $def, "--work", "$Root\work", "--findings", "$Root\findings",
         "--ledger", "$Root\ledger", "--hours", "$left", "--aios-commit", $commit, "--dirty", "$dirty", "--stop-file", $stop)
  foreach ($k in $SkipKind) { $a += @("--skip-kind", $k) }
  if ($Only) { $a += @("--only", $Only) }
  if ($InjectFault) { $a += @("--inject-fault", "$InjectFault") }
  $log = Join-Path $Root ("logs\soak-" + (Get-Date -Format "yyyyMMdd-HHmmss") + ".log")
  & $exe @a *> $log
  $code = $LASTEXITCODE
  Write-Host "[soak-run] soak exited $code (log $log)"
  if ($code -eq 1) { Write-Host "[soak-run] DISCREPANCY: stopping. See $Root\findings"; exit 1 }
  if ($code -eq 3) { Write-Host "[soak-run] ENVIRONMENT FAILURE: stopping."; Get-Content $log -Tail 5; exit 3 }
  if ($code -eq 4) { Write-Host "[soak-run] HARNESS FAULT: stopping."; Get-Content $log -Tail 5; exit 4 }
  if ($code -eq 0 -or (Test-Path $stop)) { break }
  Write-Host "[soak-run] unexpected exit; restarting in 5 s"
  Start-Sleep 5
}
exit 0
