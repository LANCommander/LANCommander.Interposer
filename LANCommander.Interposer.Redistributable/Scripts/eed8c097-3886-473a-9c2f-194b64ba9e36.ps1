# Name Change
#
# Points Player.Username at the player's LANCommander alias. Runs immediately
# after the install script and again whenever the alias changes.

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

# Same resolution the install script used, plus a fallback scan in case the
# target directory option changed after the DLL was already dropped somewhere.
$candidates = New-Object System.Collections.Generic.List[string]

$targetDirectory = [string](Get-Option $Options 'Loader.TargetDirectory')

if (-not [string]::IsNullOrWhiteSpace($targetDirectory)) {
    $targetDirectory = $targetDirectory.Trim().Replace('/', '\')

    if (-not [System.IO.Path]::IsPathRooted($targetDirectory)) {
        $targetDirectory = Join-Path $InstallDirectory $targetDirectory
    }

    $candidates.Add($targetDirectory)
}

$primaryExecutable = Get-PrimaryExecutablePath $GameManifest $InstallDirectory

if ($primaryExecutable) {
    $candidates.Add((Split-Path -Parent $primaryExecutable))
}

$candidates.Add($InstallDirectory)

$configPath = $null

foreach ($candidate in $candidates) {
    if ([string]::IsNullOrWhiteSpace($candidate)) { continue }

    $path = Join-Path $candidate '.interposer\Config.yml'

    if (Test-Path -LiteralPath $path) {
        $configPath = $path
        break
    }
}

if (-not $configPath) {
    $found = Get-ChildItem -Path $InstallDirectory -Filter 'Config.yml' -Recurse -Force -ErrorAction SilentlyContinue |
        Where-Object { $_.Directory.Name -eq '.interposer' } |
        Select-Object -First 1

    if ($found) { $configPath = $found.FullName }
}

if ($configPath) {
    # Double single quotes for YAML, then double dollar signs so the regex
    # substitution doesn't read them as group references.
    $alias = ([string]$NewPlayerAlias).Replace("'", "''").Replace('$', '$$')

    Write-ReplaceContentInFile -Pattern "Username:.*" -Substitution "Username: '$alias'" -FilePath $configPath
}
