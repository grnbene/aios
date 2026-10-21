# The release gate. Refuses (exit 1) unless, at exactly this commit on a clean tree:
#   - no finding under findings\ is open,
#   - the soak has banked at least -MinHours of unrestricted soak time (no --skip-kind / --only),
#   - every check kind and every corpus repository has at least -MinChecks checks,
#   - a --inject-fault run at this commit showed the gate catches a corrupted answer.
# Call it first in any packaging step; a non-zero exit means do not package.
param(
  [double]$MinHours = 8,
  [int]$MinChecks = 100,
  [string]$Root = "C:\Dev\git-soak"
)
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$aios = Resolve-Path (Join-Path $here "..")
$exe = Join-Path $aios "build-win\Release\aios-git-soak.exe"
if (-not (Test-Path $exe)) { Write-Host "[gate] BLOCKED: $exe does not exist"; exit 1 }
$commit = (git -C $aios rev-parse HEAD).Trim()
$dirty = if ((git -C $aios status --porcelain --untracked-files=no -- .).Trim()) { 1 } else { 0 }
& $exe --corpus "$Root\corpus" --corpus-def (Join-Path $aios "tools\proof\git-soak\corpus.tsv") --work "$Root\work" `
  --findings "$Root\findings" --ledger "$Root\ledger" --gate --aios-commit $commit --dirty $dirty `
  --min-hours $MinHours --min-checks $MinChecks
exit $LASTEXITCODE
