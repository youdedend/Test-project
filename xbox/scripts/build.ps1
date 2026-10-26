#requires -Version 7.2
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$local = Join-Path $root '.local'
$config = Get-Content -LiteralPath (Join-Path $local 'settings.json') -Raw | ConvertFrom-Json

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

& cmake -S $root -B $build -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=RelWithDebInfo @options
if ($LASTEXITCODE) { throw 'Xbox UWP CMake configure failed' }

& cmake --build $build --parallel $([Environment]::ProcessorCount) *> (Join-Path $local 'build.log')
if ($LASTEXITCODE) {
    Get-Content (Join-Path $local 'build.log') -Tail 80
    throw 'Xbox UWP build failed; see xbox/.local/build.log'
}
Write-Output 'Bloodborne Xbox UWP build complete; layout staged in xbox/.local/build/layout.'
