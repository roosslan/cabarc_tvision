param(
    [Parameter(Mandatory = $true)][ValidatePattern('^\d+(\.\d+){0,3}$')][string]$Version,
    [string]$Bin = 'build/Release',
    [string]$Out = 'dist'
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$binDir = (Resolve-Path $Bin).Path
$outDir = [IO.Path]::GetFullPath($Out)
# A fresh stage prevents files left by a previous build entering the release.
$work = Join-Path $repo ('build/package-' + [Guid]::NewGuid().ToString('N'))
$stage = Join-Path $work 'payload'
New-Item -ItemType Directory -Force $stage, $outDir | Out-Null
foreach ($file in 'cabine.exe', 'cabine-shell.exe') {
    Copy-Item -LiteralPath (Join-Path $binDir $file) -Destination $stage
}
foreach ($file in 'README.md', 'readme.ru.md') {
    Copy-Item -LiteralPath (Join-Path $repo $file) -Destination $stage
}
$licenses = Join-Path $stage 'licenses'
New-Item -ItemType Directory $licenses | Out-Null
$notices = @{
    'tvision.txt' = 'tvision/COPYRIGHT'
    'zlib.txt' = 'third_party/zlib/LICENSE'
    'minizip-ng.txt' = 'third_party/minizip-ng/LICENSE'
    'unrar.txt' = 'third_party/unrar/license.txt'
    'lzma-sdk.txt' = 'third_party/7zip/DOC/7zC.txt'
}
foreach ($name in $notices.Keys) {
    Copy-Item -LiteralPath (Join-Path $repo $notices[$name]) -Destination (Join-Path $licenses $name)
}

$pkg = "cabine-$Version-x64-portable"
$ddf = @(
    '.OPTION EXPLICIT'
    ".Set CabinetNameTemplate=$pkg.cab"
    ".Set DiskDirectoryTemplate=`"$outDir`""
    '.Set Cabinet=on'
    '.Set Compress=on'
    '.Set CompressionType=LZX'
    '.Set CompressionMemory=21'
    '.Set MaxDiskSize=0'
    '.Set RptFileName=NUL'
    '.Set InfFileName=NUL'
)
$files = @(Get-ChildItem -LiteralPath $stage -File -Recurse | Sort-Object FullName)
foreach ($file in $files) {
    $relative = $file.FullName.Substring($stage.Length + 1)
    $parent = Split-Path $relative
    $destination = if ($parent) { "$pkg\$parent" } else { $pkg }
    $ddf += ".Set DestinationDir=`"$destination`""
    $ddf += "`"$($file.FullName)`" `"$($file.Name)`""
}
$ddfPath = Join-Path $work 'portable.ddf'
# makecab reads the DDF in the current Windows ANSI code page.
[IO.File]::WriteAllLines($ddfPath, $ddf, [Text.Encoding]::GetEncoding([Globalization.CultureInfo]::CurrentCulture.TextInfo.ANSICodePage))
& makecab.exe /F $ddfPath
if ($LASTEXITCODE -ne 0) { throw 'makecab failed' }

# Check directory layout and all bytes with the system extractor.
$check = Join-Path $work 'verify'
New-Item -ItemType Directory $check | Out-Null
& expand.exe (Join-Path $outDir "$pkg.cab") '-F:*' $check | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Portable CAB extraction failed' }
foreach ($file in $files) {
    $relative = $file.FullName.Substring($stage.Length + 1)
    $extracted = Join-Path (Join-Path $check $pkg) $relative
    if ((Get-FileHash -LiteralPath $file.FullName).Hash -ne (Get-FileHash -LiteralPath $extracted).Hash) {
        throw "Portable CAB content mismatch: $relative"
    }
}
Write-Output "Payload: $stage"
if ($env:GITHUB_OUTPUT) { "payload=$stage" >> $env:GITHUB_OUTPUT }
