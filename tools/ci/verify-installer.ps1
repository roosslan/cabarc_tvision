param(
    [Parameter(Mandatory = $true)][string]$Installer,
    [Parameter(Mandatory = $true)][string]$Payload,
    [Parameter(Mandatory = $true)][string]$Version
)
$ErrorActionPreference = 'Stop'
$payloadDir = (Resolve-Path $Payload).Path
$data = [IO.File]::ReadAllBytes((Resolve-Path $Installer).Path)
$offset = -1
# XPSetup appends one CAB to its executable. Ignore signatures inside code/resources.
for ($i = 0; $i -le $data.Length - 36; $i++) {
    if ($data[$i] -eq 77 -and $data[$i + 1] -eq 83 -and $data[$i + 2] -eq 67 -and $data[$i + 3] -eq 70 -and
        $i + [BitConverter]::ToUInt32($data, $i + 8) -eq $data.Length) {
        $offset = $i
        break
    }
}
if ($offset -lt 0) { throw 'XPSetup payload CAB not found' }
$work = Join-Path (Split-Path $payloadDir) ('installer-check-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $work | Out-Null
$cab = Join-Path $work 'outer.cab'
$stream = [IO.File]::Create($cab)
try { $stream.Write($data, $offset, $data.Length - $offset) } finally { $stream.Dispose() }
$outer = Join-Path $work 'outer'
$inner = Join-Path $work 'inner'
New-Item -ItemType Directory $outer, $inner | Out-Null
& expand.exe $cab '-F:*' $outer | Out-Null
if ($LASTEXITCODE) { throw 'Cannot extract XPSetup payload' }
$sif = Get-Content -LiteralPath (Join-Path $outer 'setup.sif') -Raw
if ($sif -notmatch ('(?m)^Version\s*=\s*"?' + [regex]::Escape($Version) + '"?\s*$')) {
    throw 'Installer version does not match Cabine version'
}
& expand.exe (Join-Path $outer 'cabine.cab') '-F:*' $inner | Out-Null
if ($LASTEXITCODE) { throw 'Cannot extract installer component' }
$files = @(Get-ChildItem -LiteralPath $payloadDir -File -Recurse)
if (@(Get-ChildItem -LiteralPath $inner -File -Recurse).Count -ne $files.Count) {
    throw 'Installer file count does not match portable payload'
}
foreach ($file in $files) {
    $relative = $file.FullName.Substring($payloadDir.Length + 1)
    if ((Get-FileHash -LiteralPath $file.FullName).Hash -ne (Get-FileHash -LiteralPath (Join-Path $inner $relative)).Hash) {
        throw "Installer content mismatch: $relative"
    }
}
Write-Output "Installer verified: $($files.Count) files, version $Version"
