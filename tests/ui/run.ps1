# Тесты интерфейса Cabine: сценарии tests/ui/scenarios/*.scn выполняются драйвером
# cabine-uidriver в псевдоконсоли (ConPTY).
#
#   powershell -ExecutionPolicy Bypass -File tests\ui\run.ps1 -Bin build\Release [-Only 05]
#
# Подстановки в сценариях: {CABINE}, {SFX} — программы; {WORK} — рабочая директория
# прогона; {DATA} — tests\ui\data; {TESTDATA} — tests\data.
# Служебные строки (аргументы разделяются символом |), выполняются до запуска:
#   #copy ИСТОЧНИК | НАЗНАЧЕНИЕ      #copydir ИСТОЧНИК | НАЗНАЧЕНИЕ     #remove ПУТЬ
#   #random ПУТЬ | БАЙТ              #zip ДИРЕКТОРИЯ | ФАЙЛ.zip
#   #targz РОДИТЕЛЬ | ИМЯ | ФАЙЛ.tar.gz   (системный tar.exe)
#   #concat ФАЙЛ | ФАЙЛ | РЕЗУЛЬТАТ       (склейка, например программа + архив = SFX)
# и после завершения сценария:
#   #expect-file ПУТЬ                #expect-same ДИРЕКТОРИЯ | ДИРЕКТОРИЯ
#   #expect-contains ФАЙЛ | ТЕКСТ
# Настройки программы хранятся в {WORK}\settings (переменная CABINE_SETTINGS_DIR),
# настройки пользователя не затрагиваются.
param(
    [Parameter(Mandatory = $true)][string]$Bin,
    [string]$Only = '',
    [switch]$Keep
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$bin = (Resolve-Path $Bin).Path
$cabine = Join-Path $bin 'cabine.exe'
$sfx = Join-Path $bin 'cabine-sfx.exe'
$driver = Join-Path $bin 'cabine-uidriver.exe'
foreach ($f in $cabine, $sfx, $driver) {
    if (-not (Test-Path $f)) { Write-Error "Не найден $f" }
}

$work = Join-Path ([IO.Path]::GetTempPath()) ("cabine-ui-" + [Guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory $work | Out-Null
$env:CABINE_SETTINGS_DIR = Join-Path $work 'settings'
$env:CABINE_UI_WORK = $work
# Редактор по F4: вместо vim — заменитель, дописывающий строку в файл.
$fakevim = Join-Path $bin 'cabine-fakevim.exe'
if (Test-Path $fakevim) {
    $vimDir = Join-Path $work 'vim'
    New-Item -ItemType Directory $vimDir | Out-Null
    Copy-Item $fakevim (Join-Path $vimDir 'vim.exe')
    $env:PATH = "$vimDir;$env:PATH"
}
$vars = @{
    '{CABINE}' = $cabine; '{SFX}' = $sfx; '{WORK}' = $work
    '{DATA}' = (Join-Path $root 'data'); '{TESTDATA}' = (Join-Path (Split-Path $root) 'data')
}

function Expand([string]$s) {
    foreach ($k in $vars.Keys) { $s = $s.Replace($k, $vars[$k]) }
    return $s
}

function Args([string]$s) { return @($s -split '\s*\|\s*' | ForEach-Object { $_.Trim() }) }

function EnsureParent([string]$path) {
    $dir = Split-Path $path
    if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
}

function Setup([string]$cmd, [string[]]$a) {
    switch ($cmd) {
        'copy' { EnsureParent $a[1]; Copy-Item -LiteralPath $a[0] -Destination $a[1] -Force }
        'copydir' {
            if (Test-Path -LiteralPath $a[1]) { Remove-Item -LiteralPath $a[1] -Recurse -Force }
            EnsureParent $a[1]; Copy-Item -LiteralPath $a[0] -Destination $a[1] -Recurse
        }
        'remove' { if (Test-Path -LiteralPath $a[0]) { Remove-Item -LiteralPath $a[0] -Recurse -Force } }
        'concat' {
            EnsureParent $a[2]
            $bytes = [IO.File]::ReadAllBytes($a[0]) + [IO.File]::ReadAllBytes($a[1])
            [IO.File]::WriteAllBytes($a[2], [byte[]]$bytes)
        }
        'random' {
            EnsureParent $a[0]
            $bytes = New-Object byte[] ([int]$a[1])
            (New-Object Random 12345).NextBytes($bytes)
            [IO.File]::WriteAllBytes($a[0], $bytes)
        }
        'zip' {
            EnsureParent $a[1]
            if (Test-Path -LiteralPath $a[1]) { Remove-Item -LiteralPath $a[1] -Force }
            # Средствами .NET: командлеты модулей PowerShell могут не загрузиться,
            # если скрипт запущен из PowerShell 7 (так на раннерах GitHub).
            Add-Type -AssemblyName System.IO.Compression.FileSystem
            [IO.Compression.ZipFile]::CreateFromDirectory($a[0], $a[1])
        }
        'targz' {
            EnsureParent $a[2]
            & (Join-Path $env:SystemRoot 'System32\tar.exe') -czf $a[2] -C $a[0] $a[1]
            if ($LASTEXITCODE -ne 0) { throw "tar.exe: код $LASTEXITCODE" }
        }
        default { return $false }
    }
    return $true
}

$sha = [Security.Cryptography.SHA256]::Create()
function FileHash([string]$path) {
    return [BitConverter]::ToString($sha.ComputeHash([IO.File]::ReadAllBytes($path)))
}

function SameTree([string]$a, [string]$b) {
    if (-not (Test-Path -LiteralPath $b)) { return "нет $b" }
    $la = @(Get-ChildItem -LiteralPath $a -Recurse -File | ForEach-Object { $_.FullName.Substring($a.Length) } | Sort-Object)
    $lb = @(Get-ChildItem -LiteralPath $b -Recurse -File | ForEach-Object { $_.FullName.Substring($b.Length) } | Sort-Object)
    if (($la -join '|') -ne ($lb -join '|')) { return "разный состав файлов: $($la -join ', ') / $($lb -join ', ')" }
    foreach ($rel in $la) {
        $ha = FileHash ($a + $rel)
        $hb = FileHash ($b + $rel)
        if ($ha -ne $hb) { return "различается $rel" }
    }
    return ''
}

function Check([string]$cmd, [string[]]$a) {
    switch ($cmd) {
        'expect-file' { if (-not (Test-Path -LiteralPath $a[0])) { return "нет файла $($a[0])" } }
        'expect-same' { return (SameTree $a[0] $a[1]) }
        'expect-contains' {
            if (-not (Test-Path -LiteralPath $a[0])) { return "нет файла $($a[0])" }
            $text = Get-Content -LiteralPath $a[0] -Raw -Encoding Default
            if ($text -notlike "*$($a[1])*") { return "в $($a[0]) нет «$($a[1])»" }
        }
    }
    return ''
}

$scenarios = Get-ChildItem (Join-Path $root 'scenarios') -Filter '*.scn' | Sort-Object Name
if ($Only) { $scenarios = $scenarios | Where-Object { $_.Name -like "$Only*" } }
$failed = 0
foreach ($s in $scenarios) {
    $name = [IO.Path]::GetFileNameWithoutExtension($s.Name)
    $lines = Get-Content -LiteralPath $s.FullName -Encoding UTF8
    $script = New-Object System.Collections.Generic.List[string]
    $checks = @()
    $problems = @()
    try {
        foreach ($line in $lines) {
            $l = Expand $line
            if ($l -match '^#([a-z-]+)\s+(.*)$') {
                $cmd = $Matches[1]; $a = Args $Matches[2]
                if ($cmd -like 'expect-*') { $checks += , @($cmd, $a) }
                else { [void](Setup $cmd $a) }
            }
            else { $script.Add($l) }
        }
    }
    catch { $problems += "подготовка: $_" }

    $scn = Join-Path $work "$name.scn"
    [IO.File]::WriteAllLines($scn, $script, (New-Object Text.UTF8Encoding $false))
    if (-not $problems) {
        & $driver $scn
        $rc = $LASTEXITCODE
        $log = Get-Content -LiteralPath "$scn.log" -Encoding UTF8
        if ($rc -ne 0) {
            $problems += "драйвер: код $rc"
            $problems += $log | Where-Object { $_ -match 'НЕ НАЙДЕНО|НЕ завершился|ожидался код|принудительно|неизвестная|CreateProcess' }
        }
        foreach ($c in $checks) {
            try {
                $r = Check $c[0] $c[1]
                if ($r) { $problems += $r }
            }
            catch { $problems += "проверка $($c[0]): $_" }
        }
    }
    if ($problems) {
        $failed++
        Write-Output "ОШИБКА $name"
        $problems | ForEach-Object { Write-Output "    $_" }
    }
    else { Write-Output "ok     $name" }
}

Write-Output ""
Write-Output ("Сценариев: {0}, с ошибками: {1}. Рабочая директория: {2}" -f @($scenarios).Count, $failed, $work)
if ($failed -eq 0 -and -not $Keep) { Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue }
exit $failed
