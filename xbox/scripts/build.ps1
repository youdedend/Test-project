#requires -Version 7.2
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$local = Join-Path $root '.local'
$config = Get-Content -LiteralPath (Join-Path $local 'settings.json') -Raw | ConvertFrom-Json

function Emit-ErrorAnnotation([string]$Title, [string]$Body) {
    if ($env:GITHUB_ACTIONS -eq 'true' -and $Body) {
        $enc = $Body -replace '%', '%25' -replace "`r", '%0D' -replace "`n", '%0A'
        if ($enc.Length -gt 55000) { $enc = $enc.Substring($enc.Length - 55000) }
        Write-Output "::error title=$Title::$enc"
    }
}

if (!(Test-Path -LiteralPath (Join-Path $local 'deps/ffmpeg-install/lib/libavcodec.a'))) {
    & (Join-Path $PSScriptRoot 'build-deps.ps1') -Python $config.python
    if ($LASTEXITCODE) { throw 'Building UWP third-party dependencies failed' }
}

$vs = & "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe" -latest -property installationPath
& "$vs/Common7/Tools/Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
$env:PATH = "$vs/VC/Tools/Llvm/x64/bin;$vs/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin;C:/Program Files/CMake/bin;C:/Program Files/Ninja;$env:PATH"

$build = Join-Path $local 'build'
$options = @("-DBB_MESA_ROOT=$($config.mesa_root)")
foreach ($key in 'dozen_dll', 'mesa_license', 'dxc_root') {
    if ($config.PSObject.Properties[$key] -and $config.$key) {
        $options += "-DBB_$($key.ToUpper())=$($config.$key)"
    }
}

$cfgLog = Join-Path $local 'configure.log'
& cmake -S $root -B $build -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=RelWithDebInfo @options *> $cfgLog
if ($LASTEXITCODE) {
    $tail = (Get-Content $cfgLog -Tail 80) -join "`n"
    Write-Output $tail
    Emit-ErrorAnnotation 'CMake Configure Failed' $tail
    throw 'Xbox UWP CMake configure failed'
}

$buildLog = Join-Path $local 'build.log'
& cmake --build $build --parallel $([Environment]::ProcessorCount) *> $buildLog
if ($LASTEXITCODE) {
    $errs = @(Get-Content $buildLog | Where-Object {
        $_ -notmatch 'clang-cl\.exe\s+/nologo' -and
        $_ -match 'error:|fatal error:|FAILED:|lld-link:|undefined symbol'
    })
    if (!$errs.Count) {
        $errs = @(Get-Content $buildLog | Where-Object { $_ -notmatch 'clang-cl\.exe\s+/nologo' } | Select-Object -Last 60)
    }
    $summary = ($errs | Select-Object -First 60) -join "`n"
    Write-Output $summary
    Emit-ErrorAnnotation 'CMake Build Failed' $summary
    throw 'Xbox UWP build failed; see xbox/.local/build.log'
}
Write-Output 'Bloodborne Xbox UWP build complete; layout staged in xbox/.local/build/layout.'
