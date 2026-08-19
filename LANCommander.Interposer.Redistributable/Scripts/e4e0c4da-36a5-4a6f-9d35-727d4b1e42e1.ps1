# Install
#
# Copies the selected Interposer build next to the game executable and renders
# .interposer\Config.yml from the redistributable's configured options.
#
# The archive is extracted to <InstallDirectory>\.lancommander\<Id>\Files and
# is laid out as:
#
#   x64\version.dll  x64\dinput8.dll  x64\LANCommander.Interposer.asi
#   x86\version.dll  x86\dinput8.dll  x86\LANCommander.Interposer.asi

$RedistributableName = 'LANCommander Interposer'
$Options = Get-RedistributableOptions -Path $InstallDirectory -Id $GameManifest.Id -Name $RedistributableName

#region Option accessors

# Get-RedistributableOptions omits any option whose resolved value is blank, so
# every read has to tolerate a missing property.
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

function Get-StringOption {
    param([object]$Root, [string]$Path, [string]$Fallback = '')

    $value = Get-Option $Root $Path

    if ($null -eq $value) { return $Fallback }

    return [string]$value
}

function Get-BoolOption {
    param([object]$Root, [string]$Path, [bool]$Fallback)

    $value = Get-Option $Root $Path

    if ($null -eq $value) { return $Fallback }
    if ($value -is [bool]) { return $value }

    switch -Regex ([string]$value) {
        '^\s*(true|1|yes|on)\s*$'   { return $true }
        '^\s*(false|0|no|off)\s*$'  { return $false }
    }

    return $Fallback
}

function Get-IntOption {
    param([object]$Root, [string]$Path, [int]$Fallback)

    $value = Get-Option $Root $Path
    $parsed = 0

    if ($null -ne $value -and [int]::TryParse([string]$value, [ref]$parsed)) { return $parsed }

    return $Fallback
}

function Get-ListOption {
    param([object]$Root, [string]$Path)

    $value = Get-Option $Root $Path

    if ($null -eq $value) { return @() }

    return @($value)
}

#endregion

#region YAML emitters

# Single-quoted scalars pass backslashes through literally, which is what the
# Interposer's config reader expects for Windows paths and regex patterns.
function Format-YamlString {
    param([string]$Value)

    if ($null -eq $Value) { $Value = '' }

    return "'" + $Value.Replace("'", "''") + "'"
}

function Format-YamlBool {
    param([bool]$Value)

    if ($Value) { return 'true' }

    return 'false'
}

function Add-ScalarList {
    param([System.Collections.Generic.List[string]]$Lines, [string]$Indent, [string]$Name, $Items)

    # @($null) is a one-element array, so unset options have to be filtered out
    $items = @($Items | Where-Object { $null -ne $_ })

    if ($items.Count -eq 0) {
        $Lines.Add("$Indent$($Name): []")
        return
    }

    $Lines.Add("$Indent$($Name):")

    foreach ($item in $items) {
        $Lines.Add("$Indent  - $(Format-YamlString ([string]$item))")
    }
}

function Add-CompositeList {
    param(
        [System.Collections.Generic.List[string]]$Lines,
        [string]$Indent,
        [string]$Name,
        $Items,
        [string[]]$Fields,
        [string[]]$IntFields = @()
    )

    # @($null) is a one-element array, so unset options have to be filtered out
    $items = @($Items | Where-Object { $null -ne $_ })

    if ($items.Count -eq 0) {
        $Lines.Add("$Indent$($Name): []")
        return
    }

    $Lines.Add("$Indent$($Name):")

    foreach ($item in $items) {
        $first = $true

        foreach ($field in $Fields) {
            $raw = $null
            $property = $item.PSObject.Properties[$field]

            if ($null -ne $property) { $raw = $property.Value }

            if ($IntFields -contains $field) {
                $parsed = 0

                if (-not [int]::TryParse([string]$raw, [ref]$parsed)) { $parsed = 0 }

                $value = "$parsed"
            }
            else {
                $value = Format-YamlString ([string]$raw)
            }

            if ($first) { $prefix = "$Indent  - " } else { $prefix = "$Indent    " }

            $Lines.Add("$prefix$($field): $value")
            $first = $false
        }
    }
}

#endregion

#region Target resolution

function Get-PrimaryExecutablePath {
    param($Manifest, [string]$InstallDirectory)

    if ($null -eq $Manifest -or $null -eq $Manifest.Actions) { return $null }

    $action = $Manifest.Actions | Where-Object { $_.IsPrimaryAction } | Select-Object -First 1

    if ($null -eq $action) { $action = $Manifest.Actions | Select-Object -First 1 }
    if ($null -eq $action -or [string]::IsNullOrWhiteSpace($action.Path)) { return $null }

    return ([string]$action.Path).Replace('{InstallDir}', $InstallDirectory).Replace('/', '\')
}

# Reads IMAGE_FILE_HEADER.Machine straight out of the PE header.
function Get-PeArchitecture {
    param([string]$Path)

    try {
        $stream = [System.IO.File]::OpenRead($Path)

        try {
            $reader = New-Object System.IO.BinaryReader($stream)

            $stream.Position = 0x3C
            $stream.Position = $reader.ReadInt32()

            if ($reader.ReadUInt32() -ne 0x00004550) { return $null }

            switch ($reader.ReadUInt16()) {
                0x8664  { return 'x64' }
                0x014C  { return 'x86' }
            }

            return $null
        }
        finally {
            $stream.Dispose()
        }
    }
    catch {
        return $null
    }
}

#endregion

$primaryExecutable = Get-PrimaryExecutablePath $GameManifest $InstallDirectory

# Load method -> file shipped in the archive
$loaderFiles = @{
    'Proxy'   = 'version.dll'
    'DInput8' = 'dinput8.dll'
    'ASI'     = 'LANCommander.Interposer.asi'
}

$loaderMethod = Get-StringOption $Options 'Loader.Method'

if ([string]::IsNullOrWhiteSpace($loaderMethod)) { $loaderMethod = 'Proxy' }

if (-not $loaderFiles.ContainsKey($loaderMethod)) {
    Write-Warning "Unknown Interposer load method '$loaderMethod'; falling back to Proxy"
    $loaderMethod = 'Proxy'
}

$loaderFile = $loaderFiles[$loaderMethod]

$architecture = Get-StringOption $Options 'Loader.Architecture'

if ([string]::IsNullOrWhiteSpace($architecture)) { $architecture = 'Auto' }

if ($architecture -eq 'Auto') {
    $detected = $null

    if ($primaryExecutable -and (Test-Path -LiteralPath $primaryExecutable)) {
        $detected = Get-PeArchitecture $primaryExecutable
    }

    if ($detected) {
        $architecture = $detected
    }
    else {
        Write-Warning "Could not determine the architecture of '$primaryExecutable'; defaulting to x86"
        $architecture = 'x86'
    }
}

# Where the DLL goes. The Interposer resolves .interposer\ relative to its own
# directory, so the config has to land alongside the DLL.
$targetDirectory = Get-StringOption $Options 'Loader.TargetDirectory'

if (-not [string]::IsNullOrWhiteSpace($targetDirectory)) {
    $targetDirectory = $targetDirectory.Trim().Replace('/', '\')

    if (-not [System.IO.Path]::IsPathRooted($targetDirectory)) {
        $targetDirectory = Join-Path $InstallDirectory $targetDirectory
    }
}
elseif ($primaryExecutable) {
    $targetDirectory = Split-Path -Parent $primaryExecutable
}

if ([string]::IsNullOrWhiteSpace($targetDirectory)) { $targetDirectory = $InstallDirectory }

$sourceRoot = Join-Path $InstallDirectory ".lancommander\$($RedistributableManifest.Id)\Files"

if (-not (Test-Path -LiteralPath $sourceRoot)) { $sourceRoot = (Get-Location).Path }

$sourceFile = Join-Path $sourceRoot "$architecture\$loaderFile"

if (-not (Test-Path -LiteralPath $sourceFile)) {
    throw "Interposer payload is missing '$architecture\$loaderFile' (looked in '$sourceRoot')"
}

New-Item -ItemType Directory -Force -Path $targetDirectory | Out-Null

Copy-Item -LiteralPath $sourceFile -Destination (Join-Path $targetDirectory $loaderFile) -Force

$interposerDirectory = Join-Path $targetDirectory '.interposer'

New-Item -ItemType Directory -Force -Path $interposerDirectory | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $interposerDirectory 'Logs') | Out-Null

#region Render Config.yml

$lines = New-Object System.Collections.Generic.List[string]

$lines.Add('# Generated by LANCommander from the Interposer redistributable options.')
$lines.Add('# Edits are overwritten the next time the game is installed - change the')
$lines.Add('# options on the redistributable in LANCommander instead.')
$lines.Add('')

$lines.Add('Player:')
$lines.Add("  Username: $(Format-YamlString (Get-StringOption $Options 'Player.Username'))")
$lines.Add("  ComputerName: $(Format-YamlString (Get-StringOption $Options 'Player.ComputerName'))")
$lines.Add('')

$loggingDefaults = [ordered]@{
    Files        = $false
    Registry     = $false
    Downloads    = $true
    Plugins      = $false
    Identity     = $false
    RichPresence = $false
    DnsRedirects = $true
    Network      = $false
}

$lines.Add('Logging:')

foreach ($key in $loggingDefaults.Keys) {
    $value = Get-BoolOption $Options "Logging.$key" $loggingDefaults[$key]
    $lines.Add("  $($key): $(Format-YamlBool $value)")
}

$lines.Add('')

Add-CompositeList $lines '' 'FileRedirects' (Get-ListOption $Options 'FileRedirects') @('Pattern', 'Replacement')
$lines.Add('')

Add-CompositeList $lines '' 'DnsRedirects' (Get-ListOption $Options 'DnsRedirects') @('Pattern', 'Replacement')
$lines.Add('')

$lines.Add('NetworkAdapters:')
$lines.Add("  Enabled: $(Format-YamlBool (Get-BoolOption $Options 'NetworkAdapters.Enabled' $false))")
Add-ScalarList $lines '  ' 'Subnets' (Get-ListOption $Options 'NetworkAdapters.Subnets')
Add-ScalarList $lines '  ' 'Names' (Get-ListOption $Options 'NetworkAdapters.Names')
Add-ScalarList $lines '  ' 'MACs' (Get-ListOption $Options 'NetworkAdapters.MACs')
$lines.Add('')

$lines.Add('FastDL:')
$lines.Add("  Enabled: $(Format-YamlBool (Get-BoolOption $Options 'FastDL.Enabled' $false))")
$lines.Add("  BaseUrl: $(Format-YamlString (Get-StringOption $Options 'FastDL.BaseUrl'))")
$lines.Add("  ProbeConnections: $(Format-YamlBool (Get-BoolOption $Options 'FastDL.ProbeConnections' $false))")
$lines.Add("  ProbePort: $(Get-IntOption $Options 'FastDL.ProbePort' 80)")
$lines.Add("  ProbePath: $(Format-YamlString (Get-StringOption $Options 'FastDL.ProbePath' '/'))")
$lines.Add("  ProbeTimeout: $(Get-IntOption $Options 'FastDL.ProbeTimeout' 2000)")

# An admin clearing this list means "filter nothing", which is different from
# the option being missing entirely - only the latter falls back to the DLL's
# built-in GameSpy browser range.
if ($null -eq (Get-Option $Options 'FastDL.FilteredPorts')) {
    $filteredPorts = @([pscustomobject]@{ Min = 23000; Max = 23009 })
}
else {
    $filteredPorts = Get-ListOption $Options 'FastDL.FilteredPorts'
}

Add-CompositeList $lines '  ' 'FilteredPorts' $filteredPorts @('Min', 'Max') @('Min', 'Max')
Add-ScalarList $lines '  ' 'AllowedExtensions' (Get-ListOption $Options 'FastDL.AllowedExtensions')
$lines.Add("  UseDownloadDirectory: $(Format-YamlBool (Get-BoolOption $Options 'FastDL.UseDownloadDirectory' $true))")
$lines.Add("  DownloadDirectory: $(Format-YamlString (Get-StringOption $Options 'FastDL.DownloadDirectory'))")
$lines.Add("  BlockSensitiveFiles: $(Format-YamlBool (Get-BoolOption $Options 'FastDL.BlockSensitiveFiles' $true))")
Add-CompositeList $lines '  ' 'Paths' (Get-ListOption $Options 'FastDL.Paths') @('Local', 'Remote')
$lines.Add('')

$richPresenceFields = @(
    'Name'
    'Details'
    'DetailsUrl'
    'State'
    'StateUrl'
    'LargeImage'
    'LargeImageText'
    'SmallImage'
    'SmallImageText'
    'Button1Text'
    'Button1Url'
    'Button2Text'
    'Button2Url'
)

$lines.Add('RichPresence:')
$lines.Add("  Type: $(Get-IntOption $Options 'RichPresence.Type' 0)")

foreach ($field in $richPresenceFields) {
    $lines.Add("  $($field): $(Format-YamlString (Get-StringOption $Options "RichPresence.$field"))")
}

$lines.Add('')
$lines.Add('  Discord:')
$lines.Add("    Enabled: $(Format-YamlBool (Get-BoolOption $Options 'RichPresence.Discord.Enabled' $false))")
$lines.Add("    ApplicationId: $(Format-YamlString (Get-StringOption $Options 'RichPresence.Discord.ApplicationId'))")
$lines.Add('')

# Plugins register their own defaults under this key at runtime.
$lines.Add('Plugins: {}')

$configPath = Join-Path $interposerDirectory 'Config.yml'

[System.IO.File]::WriteAllLines($configPath, $lines, (New-Object System.Text.UTF8Encoding($false)))

#endregion

Write-Output "Installed Interposer ($loaderMethod, $architecture) to $targetDirectory"
