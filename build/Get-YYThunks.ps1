<#
.SYNOPSIS
    Fetches the pinned YY-Thunks release into build\YY-Thunks\<version>\.

.DESCRIPTION
    YY-Thunks (https://github.com/Chuyu-Team/YY-Thunks, MIT) is what lets the
    v143 CRT run on Windows XP - see build\Compatibility.props for why. It is a
    build-time dependency only: the obj file is linked into each binary, so
    nothing extra ships.

    Only the files the build uses are kept: the two WinXP objs, the XP export
    lists that build\Test-XPImports.ps1 checks against, and the licence. The
    archive is verified against a pinned SHA-256 before anything is extracted.

    The version here must match YYThunksVersion in build\Compatibility.props.
#>
[CmdletBinding()]
param(
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

$Version = '1.2.3'
$Sha256  = '4DC68EC8784E0AFED7284E4393AEC672873608161C3A5AE52FAF618875D95B81'
$Url     = "https://github.com/Chuyu-Team/YY-Thunks/releases/download/v$Version/YY-Thunks-Objs.zip"

$Keep = @(
    'objs/x86/YY_Thunks_for_WinXP.obj',
    'objs/x64/YY_Thunks_for_WinXP.obj',
    'Config/x86/5.1.2600.txt',
    'Config/x64/5.2.3790.txt',
    'LICENSE'
)

$dest = Join-Path $PSScriptRoot "YY-Thunks\$Version"

$missing = $Keep | Where-Object { -not (Test-Path (Join-Path $dest $_)) }
if (-not $Force -and -not $missing) {
    Write-Host "YY-Thunks $Version already present at $dest"
    return
}

$zip = Join-Path ([IO.Path]::GetTempPath()) "YY-Thunks-Objs-$Version.zip"

Write-Host "Downloading $Url"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
Invoke-WebRequest -Uri $Url -OutFile $zip -UseBasicParsing

# Hashed through .NET rather than Get-FileHash, which is missing when Windows
# PowerShell inherits a PowerShell 7 PSModulePath (e.g. launched from Git Bash).
$sha    = [Security.Cryptography.SHA256]::Create()
$stream = [IO.File]::OpenRead($zip)
try     { $actual = [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '') }
finally { $stream.Dispose(); $sha.Dispose() }
if ($actual -ne $Sha256) {
    Remove-Item $zip -Force
    throw "YY-Thunks archive hash mismatch: expected $Sha256, got $actual"
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead($zip)
try {
    foreach ($name in $Keep) {
        $entry = $archive.Entries | Where-Object { $_.FullName.Replace('\', '/') -eq $name } | Select-Object -First 1
        if (-not $entry) { throw "YY-Thunks archive has no entry '$name'" }

        $target = Join-Path $dest $name
        New-Item -ItemType Directory -Force -Path (Split-Path $target) | Out-Null
        [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $target, $true)
    }
}
finally {
    $archive.Dispose()
    Remove-Item $zip -Force
}

Write-Host "YY-Thunks $Version extracted to $dest"
