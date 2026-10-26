#requires -Version 7.2
# Builds and signs BloodborneLauncher_<version>_x64.appx + Dependencies/Microsoft.VCLibs.x64.14.00.appx
# into BloodborneXbox-<version>.zip for Xbox Series X|S Developer Mode installation.
[CmdletBinding()]
param([string]$Out = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$local = Join-Path $root '.local'
$layout = Join-Path $local 'build/layout'

& (Join-Path $PSScriptRoot 'check-imports.ps1') -Folder $layout
& (Join-Path $PSScriptRoot 'assets.ps1')

$vs = & "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe" -latest -property installationPath
& "$vs/Common7/Tools/Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
$sdk = Join-Path $env:WindowsSdkVerBinPath 'x64'
$vclibs = "${env:ProgramFiles(x86)}\Microsoft SDKs\Windows Kits\10\ExtensionSDKs\Microsoft.VCLibs\14.0\Appx\Retail\x64\Microsoft.VCLibs.x64.14.00.appx"
if (!(Test-Path -LiteralPath $vclibs)) {
    throw "Missing VCLibs framework package: $vclibs"
}

if (!$Out) {
    $Out = Join-Path $local ('packages/' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'))
}
$stage = Join-Path $Out 'layout'
New-Item -ItemType Directory -Force $stage | Out-Null

foreach ($name in @('AppxManifest.xml', 'bloodborne.exe', 'libwinpthread-1.dll', 'vulkan_dzn.dll', 'dxil.dll',
                    'Assets', 'patches', 'ThirdParty', 'CREDITS.md', 'LICENSE.txt')) {
    Copy-Item -LiteralPath (Join-Path $layout $name) -Destination $stage -Recurse -Force
}

$now = [DateTime]::UtcNow
$version = '1.0.{0}.{1}' -f ($now.Year % 100 * 1000 + $now.DayOfYear), ([int]$now.ToString('HHmm'))
$manifest = Join-Path $stage 'AppxManifest.xml'
$content = (Get-Content -LiteralPath $manifest -Raw).Replace('Version="1.0.0.0"', "Version=`"$version`"")
[IO.File]::WriteAllText($manifest, $content, [Text.UTF8Encoding]::new($false))

$package = Join-Path $Out "BloodborneLauncher_$version`_x64.appx"
& "$sdk/makeappx.exe" pack /d $stage /p $package /o *> (Join-Path $Out 'pack.log')
if ($LASTEXITCODE) {
    $tail = (Get-Content (Join-Path $Out 'pack.log') -Tail 40) -join "`n"
    Write-Output $tail
    if ($env:GITHUB_ACTIONS -eq 'true') {
        $enc = $tail -replace '%', '%25' -replace "`r", '%0D' -replace "`n", '%0A'
        Write-Output "::error title=MakeAppx Failed::$enc"
    }
    throw 'MakeAppx package creation failed'
}

& "$sdk/signtool.exe" sign /fd SHA256 /f (Join-Path $root 'signing/bloodborne_xbox-signing.pfx') $package *> (Join-Path $Out 'sign.log')
if ($LASTEXITCODE) {
    $tail = (Get-Content (Join-Path $Out 'sign.log') -Tail 30) -join "`n"
    Write-Output $tail
    if ($env:GITHUB_ACTIONS -eq 'true') {
        $enc = $tail -replace '%', '%25' -replace "`r", '%0D' -replace "`n", '%0A'
        Write-Output "::error title=SignTool Failed::$enc"
    }
    throw 'SignTool signing failed'
}

Remove-Item -LiteralPath $stage -Recurse -Force
New-Item -ItemType Directory -Force (Join-Path $Out 'Dependencies') | Out-Null
Copy-Item -LiteralPath $vclibs -Destination (Join-Path $Out 'Dependencies')

$zip = Join-Path $Out "BloodborneXbox-$version.zip"
Compress-Archive -Path $package, (Join-Path $Out 'Dependencies') -DestinationPath $zip -Force
New-Item -ItemType Directory -Force $local | Out-Null
[IO.File]::WriteAllText((Join-Path $local 'latest-package.txt'), $package, [Text.UTF8Encoding]::new($false))

Write-Output "Signed Xbox UWP package created: $version"
Write-Output "version=$version"
Write-Output "zip=$zip"
