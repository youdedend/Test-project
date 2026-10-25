#Requires -Version 5.1
<#
.SYNOPSIS
    Provisions user-supplied decrypted Bloodborne (CUSA03173 v01.09) assets for Xbox Dev Mode UWP.

.DESCRIPTION
    Implements the Section 6 Game Asset Provisioning workflows without requiring broadFileSystemAccess
    or FileOpenPicker dialogs on the console:
      - Mode UsbNtfs: Validates a decrypted CUSA03173 v01.09 dump, copies it to an NTFS-formatted USB
        drive under X:\Bloodborne\CUSA03173, and stamps explicit ALL APPLICATION PACKAGES (*S-1-15-2-1)
        Modify/Read&Execute inheritance ACLs so Xbox UWP AppContainers can read it without E_ACCESSDENIED.
      - Mode Sftp: Transfers the validated CUSA03173 v01.09 dump over OpenSSH SFTP (port 22) directly
        into D:\DevelopmentFiles\Bloodborne\CUSA03173 or the package's LocalState\game\CUSA03173 folder.

.EXAMPLE
    .\uwp\provision.ps1 -SourceDump "C:\Dumps\CUSA03173" -Mode UsbNtfs -UsbDriveLetter "E:"
    .\uwp\provision.ps1 -SourceDump "C:\Dumps\CUSA03173" -Mode Sftp -XboxIp 192.168.1.42 -SftpUser DevToolsUser
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$SourceDump,

    [ValidateSet("UsbNtfs", "Sftp")]
    [string]$Mode = "UsbNtfs",

    [string]$UsbDriveLetter = "",

    [string]$XboxIp = "",
    [int]$SftpPort = 22,
    [string]$SftpUser = "DevToolsUser",
    [string]$RemoteTargetDir = "/D:/DevelopmentFiles/Bloodborne/CUSA03173",

    [switch]$RunPrepareBootImage
)

$ErrorActionPreference = "Stop"
$UwpRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = Split-Path -Parent $UwpRoot

function Test-DecryptedBloodborneDump {
    param([string]$DumpDir)

    if (-not (Test-Path $DumpDir)) {
        throw "Source dump folder does not exist: $DumpDir"
    }

    $SfoPath = Join-Path $DumpDir "sce_sys\param.sfo"
    if (-not (Test-Path $SfoPath)) {
        throw "Missing '$SfoPath'. Provide a complete decrypted PS4 Bloodborne dump (base + v01.09 patch merged)."
    }

    $SfoBytes = [System.IO.File]::ReadAllBytes($SfoPath)
    if ($SfoBytes.Length -lt 20 -or $SfoBytes[0] -ne 0 -or $SfoBytes[1] -ne 0x50 -or $SfoBytes[2] -ne 0x53 -or $SfoBytes[3] -ne 0x46) {
        throw "Invalid PSF header magic in '$SfoPath'."
    }
    $SfoAscii = [System.Text.Encoding]::UTF8.GetString($SfoBytes)
    if ($SfoAscii -notmatch "CUSA\d{5}") {
        throw "param.sfo does not contain a valid Bloodborne CUSA title ID."
    }
    if ($SfoAscii -notmatch "01\.09") {
        Write-Warning "param.sfo does not appear to report APP_VER 01.09. Update 1.09 is strongly required for 60 FPS / resolution patches!"
    }

    $EbootPath = Join-Path $DumpDir "eboot.bin"
    $BootLinked = Join-Path $DumpDir "boot-linked.bin"
    if (Test-Path $EbootPath) {
        $Hdr = New-Object byte[] 16
        $Fs = [System.IO.File]::OpenRead($EbootPath)
        try { [void]$Fs.Read($Hdr, 0, 16) } finally { $Fs.Close() }

        if ($Hdr[0] -eq 0x4F -and $Hdr[1] -eq 0x15 -and $Hdr[2] -eq 0x3D -and $Hdr[3] -eq 0x1D) {
            throw "eboot.bin has encrypted PS4 SELF magic (1D3D154F). Decrypt eboot.bin and sce_module\*.prx on your PS4 before provisioning!"
        }
        if (-not ($Hdr[0] -eq 0x7F -and $Hdr[1] -eq 0x45 -and $Hdr[2] -eq 0x4C -and $Hdr[3] -eq 0x46)) {
            throw "eboot.bin is not a valid 64-bit ELF (\x7FELF)."
        }
    } elseif (-not (Test-Path $BootLinked)) {
        throw "Neither decrypted eboot.bin nor boot-linked.bin found in '$DumpDir'."
    }

    foreach ($req in @("sce_module\libc.prx", "sce_module\libSceFios2.prx", "dvdroot_ps4")) {
        $p = Join-Path $DumpDir $req
        if (-not (Test-Path $p)) {
            throw "Missing required Bloodborne asset path: $p"
        }
    }

    Write-Host "[provision.ps1] Verified decrypted Bloodborne dump at: $DumpDir" -ForegroundColor Green
}

Test-DecryptedBloodborneDump -DumpDir $SourceDump

# Optional: Pre-link boot-linked.bin on the host using scripts/prepare.py so the console skips link time
if ($RunPrepareBootImage) {
    $PreparePy = Join-Path $RepoRoot "scripts\prepare.py"
    $OutBoot = Join-Path $SourceDump "boot-linked.bin"
    if (Test-Path $PreparePy) {
        Write-Host "[provision.ps1] Pre-linking boot-linked.bin via scripts/prepare.py..." -ForegroundColor Cyan
        & python $PreparePy $SourceDump $OutBoot
    }
}

if ($Mode -eq "UsbNtfs") {
    if (-not $UsbDriveLetter) {
        throw "Pass -UsbDriveLetter (for example -UsbDriveLetter 'E:') when using -Mode UsbNtfs."
    }
    $DriveRoot = ($UsbDriveLetter.TrimEnd('\', ':')) + ":\"
    $DriveInfo = Get-Volume -DriveLetter $UsbDriveLetter.TrimEnd('\', ':')
    if ($DriveInfo.FileSystem -ne "NTFS") {
        throw "Drive $DriveRoot is formatted as '$($DriveInfo.FileSystem)'. Xbox UWP AppContainers REQUIRE NTFS with ALL APPLICATION PACKAGES (S-1-15-2-1) ACLs (exFAT/FAT32 lack ACLs and fail on files >4 GB or sandbox checks)."
    }

    $DestDir = Join-Path $DriveRoot "Bloodborne\CUSA03173"
    New-Item -ItemType Directory -Force -Path $DestDir | Out-Null

    Write-Host "[provision.ps1] Copying decrypted CUSA03173 assets to $DestDir via robocopy..." -ForegroundColor Cyan
    & robocopy $SourceDump $DestDir /MIR /MT:8 /R:2 /W:2 /NFL /NDL
    if ($LASTEXITCODE -ge 8) {
        throw "robocopy failed with exit code $LASTEXITCODE"
    }

    # Grant explicit inheritance ACLs to ALL APPLICATION PACKAGES (*S-1-15-2-1), ALL RESTRICTED APPLICATION PACKAGES (*S-1-15-2-2), Users, and Administrators
    Write-Host "[provision.ps1] Applying ALL APPLICATION PACKAGES (*S-1-15-2-1) NTFS ACLs on $DriveRoot ..." -ForegroundColor Cyan
    & icacls.exe $DriveRoot /grant "*S-1-15-2-1:(OI)(CI)(F)" /grant "*S-1-15-2-2:(OI)(CI)(RX)" /T /C /Q
    if ($LASTEXITCODE -ne 0) {
        throw "icacls failed with exit code $LASTEXITCODE. Run PowerShell as Administrator to stamp root USB ACLs."
    }

    Write-Host "[provision.ps1] USB provisioning complete!" -ForegroundColor Green
    Write-Host "CRITICAL XBOX DATA-INTEGRITY RULE: When transferring from Xbox back to PC or removing USB from Xbox, perform a FULL CONSOLE SHUTDOWN (Settings -> Power -> Shut down now, wait until power LED stops blinking) before unplugging USB to prevent NTFS journal corruption!" -ForegroundColor Yellow
    return
}

# Mode == Sftp (Fallback when USB 3.0 drive is unavailable or Linux NTFS ACL tools fail)
if (-not $XboxIp) {
    throw "Pass -XboxIp <console-ip> when using -Mode Sftp."
}
$SftpExe = Get-Command sftp.exe -ErrorAction SilentlyContinue
if (-not $SftpExe) {
    throw "OpenSSH sftp.exe not found in PATH. Enable 'OpenSSH Client' in Windows Optional Features or use WinSCP/FileZilla on port $SftpPort."
}

$BatchFile = Join-Path $env:TEMP "bbport_sftp_batch.txt"
@"
-mkdir /D:/DevelopmentFiles/Bloodborne
-mkdir $RemoteTargetDir
put -r "$SourceDump\*" "$RemoteTargetDir"
bye
"@ | Set-Content -Path $BatchFile -Encoding ASCII

Write-Host "[provision.ps1] Uploading assets over SFTP (${SftpUser}@${XboxIp}:${SftpPort} -> ${RemoteTargetDir})..." -ForegroundColor Cyan
Write-Host "When prompted for password, enter the Visual Studio Pairing PIN from Xbox Dev Home (for DevToolsUser) or your Device Portal password." -ForegroundColor Yellow
& $SftpExe.Source -P $SftpPort -b $BatchFile "${SftpUser}@${XboxIp}"
Remove-Item -Path $BatchFile -ErrorAction SilentlyContinue
if ($LASTEXITCODE -ne 0) {
    throw "SFTP upload exited with code $LASTEXITCODE"
}
Write-Host "[provision.ps1] SFTP provisioning complete! Launch BBPort.UWP on Xbox and select 'Re-Scan LocalState'." -ForegroundColor Green
