#Requires -Version 5.1
<#
.SYNOPSIS
    Builds the Bloodborne Xbox Dev Mode UWP package (x64 Release or Debug .msix/.appx).

.DESCRIPTION
    Locates MSBuild via vswhere.exe, generates a local self-signed Appx signing certificate
    (excluded from git via .gitignore) if none exists, optionally compiles the in-process C11
    PS4 runtime with ClangCL (-UseClangCL) for GNU sysv_abi support, and outputs the signed
    Appx/MSIX package under uwp\AppPackages\.

.EXAMPLE
    .\uwp\build.ps1 -Configuration Release -UseClangCL
#>
[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",

    [switch]$UseClangCL = $true,

    [string]$CertSubject = "CN=BBPortDev",

    [switch]$SkipCertGeneration
)

$ErrorActionPreference = "Stop"
$UwpRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$SolutionPath = Join-Path $UwpRoot "BBPort.UWP.sln"
$SigningDir = Join-Path $UwpRoot "BBPort.UWP\Signing"
$PfxPath = Join-Path $SigningDir "BBPort_TemporaryKey.pfx"
$CerPath = Join-Path $SigningDir "BBPort_TemporaryKey.cer"
$AppPackagesDir = Join-Path $UwpRoot "AppPackages"

if (-not (Test-Path $SolutionPath)) {
    throw "UWP solution not found at: $SolutionPath"
}

# 1. Locate MSBuild.exe via vswhere.exe
$VsWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $VsWhere)) {
    throw "vswhere.exe not found at '$VsWhere'. Install Visual Studio 2022 with 'Universal Windows Platform development' and 'C++ Clang tools for Windows'."
}

$MsBuildPath = & $VsWhere -latest -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1
if (-not $MsBuildPath -or -not (Test-Path $MsBuildPath)) {
    throw "MSBuild.exe not found. Ensure Visual Studio 2022 UWP C++ workload (v143, Windows 10 SDK 10.0.22621.0+) is installed."
}
Write-Host "[build.ps1] Using MSBuild: $MsBuildPath" -ForegroundColor Cyan

# 2. Generate unpassworded self-signed Appx development certificate if absent
New-Item -ItemType Directory -Force -Path $SigningDir | Out-Null
if (-not $SkipCertGeneration -and -not (Test-Path $PfxPath)) {
    Write-Host "[build.ps1] Generating self-signed SHA-256 Appx signing certificate ($CertSubject)..." -ForegroundColor Yellow
    $Cert = New-SelfSignedCertificate `
        -Type Custom `
        -Subject $CertSubject `
        -KeyUsage DigitalSignature `
        -FriendlyName "BBPort Xbox Dev Mode Temporary Signing Cert" `
        -CertStoreLocation "Cert:\CurrentUser\My" `
        -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3", "2.5.29.19={text}")

    Export-Certificate -Cert $Cert -FilePath $CerPath -Force | Out-Null
    # Export unencrypted PFX for MSBuild AppxPackageSigningEnabled (dev-only; gitignored!)
    $CertBytes = $Cert.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Pkcs12)
    [System.IO.File]::WriteAllBytes($PfxPath, $CertBytes)
    Remove-Item -Path "Cert:\CurrentUser\My\$($Cert.Thumbprint)" -ErrorAction SilentlyContinue
}

# 3. Run MSBuild for x64 UWP
New-Item -ItemType Directory -Force -Path $AppPackagesDir | Out-Null
$ClangProp = if ($UseClangCL) { "true" } else { "false" }

$MsBuildArgs = @(
    $SolutionPath,
    "/m",
    "/t:Build",
    "/p:Configuration=$Configuration",
    "/p:Platform=x64",
    "/p:UseClangCL=$ClangProp",
    "/p:UapAppxPackageBuildMode=SideloadOnly",
    "/p:AppxBundle=Never",
    "/p:AppxPackageDir=$AppPackagesDir\",
    "/p:GenerateAppxPackageOnBuild=true"
)

if (Test-Path $PfxPath) {
    $MsBuildArgs += "/p:PackageCertificateKeyFile=$PfxPath"
    $MsBuildArgs += "/p:AppxPackageSigningEnabled=true"
} else {
    $MsBuildArgs += "/p:AppxPackageSigningEnabled=false"
}

Write-Host "[build.ps1] Building BBPort.UWP ($Configuration|x64, UseClangCL=$ClangProp)..." -ForegroundColor Green
& $MsBuildPath @MsBuildArgs
if ($LASTEXITCODE -ne 0) {
    throw "MSBuild failed with exit code $LASTEXITCODE"
}

$Pkg = Get-ChildItem -Path $AppPackagesDir -Recurse -Include *.msix, *.appx |
    Where-Object { $_.FullName -notmatch "Dependencies" } |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1

if ($Pkg) {
    Write-Host "[build.ps1] SUCCESS: Built UWP package -> $($Pkg.FullName)" -ForegroundColor Green
} else {
    Write-Host "[build.ps1] Build succeeded (binaries in uwp\BBPort.UWP\x64\$Configuration\)." -ForegroundColor Green
}
