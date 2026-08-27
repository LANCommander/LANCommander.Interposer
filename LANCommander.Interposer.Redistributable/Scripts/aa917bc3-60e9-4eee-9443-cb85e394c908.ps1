# Uninstall
#
# Removes the Interposer DLL and its .interposer directory from the game.
#
# Every candidate file is checked against its embedded ProductName before being
# deleted, so a game that ships its own version.dll, dinput8.dll or dinput.dll is left
# alone even if the load method changed after install.

$RedistributableName = 'LANCommander Interposer'
$Options = Get-RedistributableOptions -Path $InstallDirectory -Id $GameManifest.Id -Name $RedistributableName

function Get-Option {
    param([object]$Root, [string]$Path)

    $node = $Root

    foreach ($part in $Path.Split('.')) {
        if ($null -eq $node) { return $null }

        $property = $node.PSObject.Properties[$part]

        if ($null -eq $property) { return $null }

        $node = $property.Value
    }

    return $node
}

function Get-PrimaryExecutablePath {
    param($Manifest, [string]$InstallDirectory)

    if ($null -eq $Manifest -or $null -eq $Manifest.Actions) { return $null }

    $action = $Manifest.Actions | Where-Object { $_.IsPrimaryAction } | Select-Object -First 1

    if ($null -eq $action) { $action = $Manifest.Actions | Select-Object -First 1 }
    if ($null -eq $action -or [string]::IsNullOrWhiteSpace($action.Path)) { return $null }

    return ([string]$action.Path).Replace('{InstallDir}', $InstallDirectory).Replace('/', '\')
}

function Test-IsInterposer {
    param([string]$Path)

    try {
        return ((Get-Item -LiteralPath $Path).VersionInfo.ProductName -eq 'LANCommander Interposer')
    }
    catch {
        return $false
    }
}

$directories = New-Object System.Collections.Generic.List[string]

$targetDirectory = [string](Get-Option $Options 'Loader.TargetDirectory')

if (-not [string]::IsNullOrWhiteSpace($targetDirectory)) {
    $targetDirectory = $targetDirectory.Trim().Replace('/', '\')

    if (-not [System.IO.Path]::IsPathRooted($targetDirectory)) {
        $targetDirectory = Join-Path $InstallDirectory $targetDirectory
    }

    $directories.Add($targetDirectory)
}

$primaryExecutable = Get-PrimaryExecutablePath $GameManifest $InstallDirectory

if ($primaryExecutable) {
    $directories.Add((Split-Path -Parent $primaryExecutable))
}

$directories.Add($InstallDirectory)

$loaderFiles = @('version.dll', 'dinput8.dll', 'dinput.dll', 'LANCommander.Interposer.asi')
$seen = New-Object System.Collections.Generic.HashSet[string]

foreach ($directory in $directories) {
    if ([string]::IsNullOrWhiteSpace($directory)) { continue }
    if (-not $seen.Add($directory.TrimEnd('\').ToLowerInvariant())) { continue }
    if (-not (Test-Path -LiteralPath $directory)) { continue }

    foreach ($loaderFile in $loaderFiles) {
        $path = Join-Path $directory $loaderFile

        if ((Test-Path -LiteralPath $path) -and (Test-IsInterposer $path)) {
            Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
        }
    }

    $interposerDirectory = Join-Path $directory '.interposer'

    if (Test-Path -LiteralPath $interposerDirectory) {
        Remove-Item -LiteralPath $interposerDirectory -Recurse -Force -ErrorAction SilentlyContinue
    }
}
