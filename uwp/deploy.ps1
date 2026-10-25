#Requires -Version 5.1
<#
.SYNOPSIS
    Deploys BBPort.UWP (.msix/.appx) to an Xbox One / Xbox Series X|S in Dev Mode and registers it as a Game.

.DESCRIPTION
    Supports two transport modes:
      1. DevicePortal (default): Uploads the package + VCLibs x64 dependencies via Xbox Device Portal HTTPS REST API
         (port 11443) and automatically switches the package resource mode from 'App' (1 GiB, D3D11-only) to
         'Game' (5 GiB cap, Direct3D 12 Feature Level 12_0 enabled).
      2. WinAppDeployCmd: Uses Windows SDK WinAppDeployCmd.exe with a Visual Studio pairing PIN.

.EXAMPLE
    .\uwp\deploy.ps1 -XboxIp 192.168.1.42 -PortalUser admin -PortalPass admin
    .\uwp\deploy.ps1 -XboxIp 192.168.1.42 -Method WinAppDeployCmd -Pin A1B2C3
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$XboxIp,

    [ValidateSet("DevicePortal", "WinAppDeployCmd")]
    [string]$Method = "DevicePortal",

    [int]$PortalPort = 11443,
    [string]$PortalUser = "",
    [string]$PortalPass = "",
    [string]$Pin = "",
    [string]$PackagePath = ""
)

$ErrorActionPreference = "Stop"
$UwpRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$AppPackagesDir = Join-Path $UwpRoot "AppPackages"

if (-not $PackagePath) {
    $Found = Get-ChildItem -Path $AppPackagesDir -Recurse -Include *.msix, *.appx -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -notmatch "Dependencies" } |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if (-not $Found) {
        throw "No .msix/.appx found under '$AppPackagesDir'. Run .\uwp\build.ps1 first or pass -PackagePath."
    }
    $PackagePath = $Found.FullName
}

if (-not (Test-Path $PackagePath)) {
    throw "Package file does not exist: $PackagePath"
}

$PkgDir = Split-Path -Parent $PackagePath
$DepPackages = @()
$DepsFolder = Join-Path $PkgDir "Dependencies\x64"
if (Test-Path $DepsFolder) {
    $DepPackages = Get-ChildItem -Path $DepsFolder -Include *.appx, *.msix -Recurse | ForEach-Object { $_.FullName }
}

if ($Method -eq "WinAppDeployCmd") {
    $SdkRoot = "${env:ProgramFiles(x86)}\Windows Kits\10\bin"
    $DeployExe = Get-ChildItem -Path $SdkRoot -Filter "WinAppDeployCmd.exe" -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match "\\x64\\|\\x86\\" } |
        Sort-Object FullName -Descending |
        Select-Object -First 1

    if (-not $DeployExe) {
        throw "WinAppDeployCmd.exe not found under '$SdkRoot'. Install the Windows 10/11 SDK or use -Method DevicePortal."
    }

    $CmdArgs = @("install", "-file", $PackagePath, "-ip", $XboxIp)
    if ($Pin) {
        $CmdArgs += @("-pin", $Pin)
    }
    Write-Host "[deploy.ps1] Installing via WinAppDeployCmd -> $XboxIp ..." -ForegroundColor Cyan
    & $DeployExe.FullName @CmdArgs
    if ($LASTEXITCODE -ne 0) {
        throw "WinAppDeployCmd failed with exit code $LASTEXITCODE"
    }
    Write-Host "[deploy.ps1] Installed! IMPORTANT: In Xbox Dev Home, highlight BBPort -> View details -> Change type from 'App' to 'Game' to enable D3D12 FL 12_0 and the 5 GiB memory cap." -ForegroundColor Yellow
    return
}

# Device Portal HTTPS REST deployment (trusts self-signed Xbox Dev Mode TLS cert for this session)
add-type @"
using System.Net;
using System.Security.Cryptography.X509Certificates;
public class XboxPortalTlsPolicy : ICertificatePolicy {
    public bool CheckValidationResult(ServicePoint sp, X509Certificate cert, WebRequest req, int problem) { return true; }
}
"@ -ErrorAction SilentlyContinue
[System.Net.ServicePointManager]::CertificatePolicy = New-Object XboxPortalTlsPolicy
[System.Net.ServicePointManager]::SecurityProtocol = [System.Net.SecurityProtocolType]::Tls12

$BaseUri = "https://${XboxIp}:${PortalPort}"
$CurlArgs = @("--insecure", "--fail", "--show-error")
if ($PortalUser) {
    $CurlArgs += @("-u", "${PortalUser}:${PortalPass}")
}

$PkgFileName = [System.Uri]::EscapeDataString((Split-Path -Leaf $PackagePath))
$FormArgs = @("-F", "package=@`"$PackagePath`"")
foreach ($dep in $DepPackages) {
    $FormArgs += @("-F", "dependency=@`"$dep`"")
}

$UploadUrl = "${BaseUri}/api/app/packagemanager/package?package=${PkgFileName}"
Write-Host "[deploy.ps1] Uploading $PackagePath (+ $($DepPackages.Count) dependency appx files) to $UploadUrl ..." -ForegroundColor Cyan
& curl.exe @CurlArgs -X POST @FormArgs $UploadUrl
if ($LASTEXITCODE -ne 0) {
    throw "Device Portal package upload failed (exit $LASTEXITCODE). Verify Remote Access settings on Xbox Dev Home."
}

# Poll installation status
Start-Sleep -Seconds 3
$StateUrl = "${BaseUri}/api/app/packagemanager/state"
& curl.exe @CurlArgs -s $StateUrl | Out-Host

# Query installed packages to find BBPort.BloodborneUWP PackageFullName and switch App -> Game mode!
Write-Host "[deploy.ps1] Querying PackageFullName to enable Xbox Game Resource Mode (5 GiB + D3D12 FL 12_0)..." -ForegroundColor Cyan
$PkgsJson = & curl.exe @CurlArgs -s "${BaseUri}/api/app/packagemanager/packages" | ConvertFrom-Json
$TargetPkg = $PkgsJson.InstalledPackages | Where-Object { $_.PackageFamilyName -like "BBPort.BloodborneUWP*" } | Select-Object -First 1

if ($TargetPkg -and $TargetPkg.PackageFullName) {
    $EscFull = [System.Uri]::EscapeDataString($TargetPkg.PackageFullName)
    $GameModeUrl = "${BaseUri}/ext/xbox/appdeployment/changetype?PackageFullName=${EscFull}&PackageType=Game"
    & curl.exe @CurlArgs -X PUT -d "" $GameModeUrl
    Write-Host "[deploy.ps1] SUCCESS: Registered $($TargetPkg.PackageFullName) as 'Game' (5 GiB cap + D3D12 FL 12_0 enabled)." -ForegroundColor Green
} else {
    Write-Host "[deploy.ps1] Package uploaded. Verify in Dev Home that App Type is set to 'Game' before launching." -ForegroundColor Yellow
}
