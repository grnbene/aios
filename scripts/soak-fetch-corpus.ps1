# Mirror the git soak's corpus: bare, one pack, no loose objects, each pinned to a commit.
#
#   scripts/soak-fetch-corpus.ps1 [-Corpus C:\Dev\git-soak\corpus] [-Only name]
#
# Reads tools/proof/git-soak/corpus.tsv. A row whose pin is "-" is resolved here, once, to the
# default-branch tip of the mirror and written back to the tsv: commit that change, and every
# finding afterwards replays against the same history. Records the git that baselined the corpus
# in GIT_VERSION; the soak refuses to run under a different git.
param(
  [string]$Corpus = "C:\Dev\git-soak\corpus",
  [string]$Only = ""
)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$def = Join-Path $here "..\tools\proof\git-soak\corpus.tsv" | Resolve-Path
New-Item -ItemType Directory -Force $Corpus | Out-Null
$env:GIT_TERMINAL_PROMPT = "0"
$ver = (git --version).Trim()
$vf = Join-Path $Corpus "GIT_VERSION"
if (Test-Path $vf) {
  $have = (Get-Content $vf -Raw).Trim()
  if ($have -ne $ver) { throw "corpus was baselined on '$have' but this is '$ver'. Review, then delete $vf deliberately." }
} else { Set-Content -NoNewline -Path $vf -Value $ver }

$lines = Get-Content $def
$out = @()
foreach ($line in $lines) {
  if ($line -match '^\s*#' -or $line.Trim() -eq '') { $out += $line; continue }
  $c = $line -split "`t"
  $name = $c[0]; $url = $c[1]; $pin = $c[2]; $variants = if ($c.Count -gt 3) { $c[3] } else { "" }
  if ($Only -and $name -ne $Only) { $out += $line; continue }
  $dst = Join-Path $Corpus "$name.git"
  if (-not (Test-Path $dst)) {
    Write-Host "[corpus] cloning $name from $url"
    git clone --bare --quiet $url $dst
    if ($LASTEXITCODE) { throw "clone of $name failed" }
    git -C $dst config gc.auto 0
    git -C $dst repack -a -d -q
    git -C $dst prune --expire=now
  }
  if ($pin -eq "-") {
    $pin = (git -C $dst rev-parse HEAD).Trim()
    Write-Host "[corpus] $name pinned to $pin"
  }
  git -C $dst cat-file -e "$pin^{commit}"
  if ($LASTEXITCODE) { throw "$name does not contain its pinned commit $pin" }
  git -C $dst update-ref refs/soak/pin $pin
  $n = (git -C $dst rev-list --count $pin).Trim(); $m = (git -C $dst rev-list --merges --count $pin).Trim()
  $loose = (git -C $dst count-objects -v | Select-String '^count:').ToString().Split(':')[1].Trim()
  Write-Host ("[corpus] {0}: {1} commits, {2} merges, {3} loose objects" -f $name, $n, $m, $loose)
  if ($loose -ne "0") { throw "$name has loose objects; the soak needs the master to be a single pack" }
  $out += ($name, $url, $pin, $variants) -join "`t"
}
Set-Content -Path $def -Value $out
