<#
Copyright 2021 PCSX-Redux authors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the
Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#>

if ((-not (Get-Variable PSVersionTable -Scope Global -ErrorAction SilentlyContinue)) -or ($PSVersionTable.PSVersion.Major -lt 5)) {
    Write-Host "Your version of PowerShell is too old."
    Write-Host "mips-ps requires a least PowerShell 5.0"
    Write-Host "You need to download and install Windows Management Framework 5.0+"
    exit
}

if (-not [Environment]::Is64BitOperatingSystem) {
    Write-Host "The prebuilt mips toolchains can only work on a 64 bits version of Windows,"
    Write-Host "but you're running a 32 bits version. Please install the 64 bits version of"
    Write-Host "Windows and try again."
    exit
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
function Get-AbsolutePath($Path) {
    $Path = [System.IO.Path]::Combine(((pwd).path), ($Path))
    $Path = [System.IO.Path]::GetFullPath($Path)
    return $Path.TrimEnd("\/")
}

# Adds a path to the user's PATH environment variable.
function Add-Path($Path) {
    $UserPath = [environment]::GetEnvironmentVariable("PATH", "User")
    if ($UserPath -eq $NULL) {
        return
    }
    foreach ($Fragment in $UserPath.split(";")) {
        if ($Fragment -like $Path) {
            return
        }
    }
    [environment]::SetEnvironmentVariable("PATH", $UserPath + ";" + $Path, "User")
}

function MkDir-p($Path) {
    $FullPath = "\\?\" + $Path
    if (-not (Test-Path -Path $FullPath)) {
        New-Item -ItemType directory -Path $FullPath | Out-Null
    }
}

function Usage() {
    Write-Host "Usage:"
    Write-Host "  mips install <version>"
    Write-Host "  mips use <version>"
    Write-Host "  mips ls"
    Write-Host "  mips ls-remote"
    Write-Host "  mips version"
    Write-Host "  mips self-install"

    exit
}

function Download-Index($Path) {
    $IndexFile = $Path + "/index.json"
    $FullURL = $MipsBaseURL + "index.json"
    Invoke-WebRequest -UseBasicParsing -ContentType "application/octet-stream" -Uri $FullURL -OutFile $IndexFile
}

function Sort-Versions($Data) {
    $Versions = @()
    ForEach ($Version in $Data) {
        [Int]$Major, [Int]$Minor, [Int]$Patch = $Version.version.TrimStart("v").Split(".")
        $Key = $Major * 10000 + $Minor * 100 + $Patch
        $Version | Add-Member Key $Key
        $Versions += $Version
    }
    return $Versions | Sort-Object -Descending -Property Key
}

# Downloads the index, and assign a version number to it that can be easily sorted.
# This way doing mips install 8 will install the latest version of 8.
function Load-Index($Path) {
    $IndexFile = $Path + "/index.json"
    if (-not (Test-Path $IndexFile)) {
        Download-Index($Path)
    }
    $RawData = (Get-Content $IndexFile) -join "`n" | ConvertFrom-Json
    return Sort-Versions $RawData
}

function New-TempDir {
    $TempDir = [System.IO.Path]::GetTempPath()
    [string]$Random = [System.Guid]::NewGuid()
    $TempDir = Join-Path $TempDir $Random
    New-Item -ItemType Directory -Path $TempDir | Out-Null
    return $TempDir
}

# Downloads the list of releases, and attempts to find a version that matches the
# one specified by $Version. Returns a structure with the information for that specific
# version. The index is this one: https://distrib.app/alias/pcsx-redux/mips-toolchain/index.json
function Locate-Version($Version) {
    $VersionString = "v" + $Version
    $Version = $NULL
    For ($run = 0; $run -le 1; $run++) {
        $Index = Load-Index $cwd
        ForEach ($Iterator in $Index) {
            if ($Iterator.version.StartsWith($VersionString)) {
                $Version = $Iterator
                break
            }
        }
        if ($Version -eq $NULL) {
            Download-Index $cwd
        } else {
            $Version | Add-Member File $File
            return $Version
        }
    }

    Write-Host "Mips toolchain version $VersionString not found"
    return $NULL
}

# Will change the symlink to another version of the toolchain. We're currently not checking if that
# version is indeed installed. TODO: check if the version is actually there.
function Use($Version) {
    [string]$Version = $Version
    if (!$Version.StartsWith("v")) {
        $Installed = Get-ChildItem $VersionsPath | Select-Object Name
        $Versions = @()
        ForEach ($Iterator in $Installed) {
            $Object = New-Object PSObject
            $Object | Add-Member version $Iterator.Name
            $Versions += $Object
        }
        $Versions = Sort-Versions $Versions
        $Version = "v" + $Version
        ForEach ($Iterator in $Versions) {
            if ($Iterator.version.StartsWith($Version)) {
                $Version = $Iterator.version
                break
            }
        }
    }
    $VersionPath = $VersionsPath + "/" + $Version
    if (Test-Path -Path $symlink) {
        Remove-Item -Path $symlink -Recurse -Force
    }
    New-Item -Force -Path $symlink -ItemType Junction -Value $VersionPath
}

# Will attempt to download and install the version of the mips toolchain specified by $Version.
function Install($Version) {
    $Version = Locate-Version $Version
    if ($Version -eq $NULL) {
        return $FALSE
    }

    $TempDir = New-TempDir
    $OutputDir = $cwd + "/versions/" + $Version.version

    $ToolchainURL = $MipsBaseURL + "g++-mipsel-none-elf-" + $Version.version.TrimStart("v") + ".zip"
    $OutputToolchain = $TempDir + "/gcc.zip"
    Write-Host "Downloading toolchain package from $ToolchainURL..."
    Invoke-WebRequest -UseBasicParsing -ContentType "application/octet-stream" -Uri $ToolchainURL -OutFile $OutputToolchain

    $GdbURL = $GdbBaseURL + "gdb-multiarch-" + $Version.gdb + ".zip"
    $OutputGDB = $TempDir + "/gdb.zip"
    Write-Host "Downloading gdb package from $GdbURL..."
    Invoke-WebRequest -UseBasicParsing -ContentType "application/octet-stream" -Uri $GdbURL -OutFile $OutputGDB

    Write-Host "Extracting toolchain package..."
    Expand-Archive -Path $OutputToolchain -DestinationPath $OutputDir -Force
    Write-Host "Extracting gdb package..."
    Expand-Archive -Path $OutputGDB -DestinationPath $OutputDir -Force

    Remove-Item -Path $OutputToolchain -Force | Out-Null
    Remove-Item -Path $OutputGDB -Force | Out-Null
    $VersionString = $Version.version

    Use $VersionString
    return $TRUE
}

$dest = Get-AbsolutePath ([Environment]::GetFolderPath('ApplicationData') + "/mips")
$me = $MyInvocation.MyCommand.Path

$MipsBaseURL = "https://distrib.app/alias/pcsx-redux/mips-toolchain/"
$GdbBaseURL = "https://distrib.app/alias/pcsx-redux/gdb-multiarch-windows/"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$MyURI = "https://raw.githubusercontent.com/pcsx-redux/pcsx-redux/main/mips.ps1"

# Globals for PowerShell behavior
$ProgressPreference = "SilentlyContinue"
$ErrorActionPreference = "stop"

# If we're invoked from the installer shortcut, we're going to redownload ourselves
# and install ourselves. That's a bit redundant, but, sure.
if ($MyInvocation.InvocationName -eq "&") {
    $me = [System.IO.Path]::GetTempFileName()
    Invoke-WebRequest -Uri $MyURI -OutFile $me
    $cmd = "self-install"
    $symlink = $dest + "/mips"
    $VersionsPath = $dest + "/versions"
} else {
    $cwd = Get-AbsolutePath (Split-Path $me)
    $symlink = $cwd + "/mips"
    $VersionsPath = $cwd + "/versions"

    $cmd = $args[0]
    if ($args.Length -eq 0) {
        Add-Type -AssemblyName System.Windows.Forms
        $installform = New-Object system.Windows.Forms.Form
        $installform.Text = "MIPS Toolchain Installer"
        $installform.Width = 400
        $installform.Height = 200
        $installform.StartPosition = "CenterScreen"
        $installform.FormBorderStyle = "FixedDialog"
        $installform.MaximizeBox = $FALSE
        $installform.MinimizeBox = $FALSE
        $installform.TopMost = $TRUE
        $installform.ShowInTaskbar = $FALSE
        $installform.TopLevel = $TRUE
        $label = New-Object System.Windows.Forms.Label
        $label.Text = "Select a version of the toolchain to install:"
        $label.Location = New-Object System.Drawing.Point(10, 10)
        $label.Width = 360
        $installform.Controls.Add($label)
        $list = New-Object System.Windows.Forms.ComboBox
        $list.Location = New-Object System.Drawing.Point(10, 40)
        $list.Width = 360
        $list.DropDownStyle = "DropDownList"
        $installform.Controls.Add($list)
        $acceptButton = New-Object System.Windows.Forms.Button
        $acceptButton.Text = "Install"
        $acceptButton.Location = New-Object System.Drawing.Point(10, 70)
        $acceptButton.Width = 360
        $acceptButtonClick = {
            $installform.Hide()
            $installform.Close() | Out-Null
            $toinstall = $list.Text
            Install $toinstall.TrimStart("v") | Out-Null
        }
        $acceptButton.Add_Click($acceptButtonClick)
        $installform.Controls.Add($acceptButton)
        $cancelButton = New-Object System.Windows.Forms.Button
        $cancelButton.Text =  "Cancel"
        $cancelButton.Location = New-Object System.Drawing.Point(10, 100)
        $cancelButton.Width = 360
        $cancelButtonClick = {
            $installform.Close() | Out-Null
        }
        $cancelButton.Add_Click($cancelButtonClick)
        $installform.Controls.Add($cancelButton)
        Download-Index $cwd
        Load-Index $cwd | Sort-Object -Descending -Property Key | ForEach-Object {
            $list.Items.Add($_.version) | Out-Null
        }
        $list.SelectedIndex = 0
        $installform.ShowDialog() | Out-Null
        return
    }
}

switch ($cmd) {
    "install" {
        if ($args[1] -eq $NULL) {
            Usage
        }
        Install $args[1] | Out-Null
    }
    "use" {
        if ($args[1] -eq $NULL) {
            Usage
        }
        Use $args[1] | Out-Null
    }
    "ls" {
        Get-ChildItem $VersionsPath | Select-Object Name
    }
    "ls-remote" {
        Download-Index $cwd
        Load-Index $cwd | Sort-Object -Property Key | Select-Object version
    }
    "version" {
        Write-Host "v0.2.0"
    }
    "self-install" {
        if ($args[1] -ne $NULL) {
            $dest = $args[1]
            $symlink = $dest + "/mips"
            $VersionsPath = $dest + "/versions"
        }
        if ($cwd -like $dest) {
            Write-Host "This is already installed."
        } else {
            Write-Host "Installing..."
            while ($dest -match " ") {
                Write-Host "The installation path of the mips toolchain contains spaces. This is not supported."
                Write-Host "Please select a different folder for the installation."
                [System.Reflection.Assembly]::LoadWithPartialName("System.windows.forms") | Out-Null

                $folderdialog = New-Object System.Windows.Forms.FolderBrowserDialog
                $folderdialog.Description = "Select an installation folder for the mips tool and toolchains."
                $folderdialog.rootfolder = "MyComputer"
                $folderdialog.SelectedPath = $initialDirectory

                if ($folderdialog.ShowDialog() -eq "OK") {
                    $dest = $folderdialog.SelectedPath
                    $symlink = $dest + "/mips"
                    $VersionsPath = $dest + "/versions"
                } else {
                    Write-Host "Installation aborted."
                    return
                }
            }
            MkDir-p $dest
            MkDir-p $VersionsPath
            Copy-Item -Force $me $dest/mips.ps1
            Set-Content -Path $dest/mips.cmd -Value "@PowerShell -ExecutionPolicy Unrestricted %~dp0/mips.ps1 %*"
            Add-Path $dest
            Add-Path $symlink\bin
            Write-Host "Done. Open a new console and type mips."
        }
    }
    default {
        Write-Host "Unknown command $cmd"
        Usage
    }
}
