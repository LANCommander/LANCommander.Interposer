<#
.SYNOPSIS
    Checks that PE binaries can load on Windows XP.

.DESCRIPTION
    For each binary, verifies against the real export tables of Windows XP SP3
    (x86, 5.1.2600) or XP x64 / Server 2003 (x64, 5.2.3790), as shipped with
    YY-Thunks (fetch with build\Get-YYThunks.ps1):

      * the subsystem and OS version fields are no higher than 5.01 / 5.02 -
        the XP loader refuses anything higher as "not a valid Win32 application";
      * every module in the import and delay-import tables exists on that OS -
        this is also what catches a dynamic CRT (vcruntime140.dll, the
        api-ms-win-crt-* API sets) sneaking back in;
      * every function imported from it, by name or by ordinal, is exported
        by it - a single miss is a "procedure entry point could not be
        located" dialog at load time.

    It is a static check. It cannot see what a binary resolves at runtime with
    GetProcAddress; those paths must tolerate a null result on their own.

    Exit code is the number of failing binaries.

.PARAMETER Path
    Binaries to check. Architecture is read from each file's header.

.PARAMETER AllowModule
    Imported module names that are not part of Windows and are expected next
    to the binary (e.g. a plugin importing the Interposer itself).
#>
[CmdletBinding()]
param(
    # ValueFromRemainingArguments so that -File callers can pass several paths
    # (a comma list arrives as one string there); wildcards are expanded. Not
    # ValueFromPipeline: under Windows PowerShell 5.1, -File with a redirected
    # stdin binds an empty pipeline and the loop never runs.
    [Parameter(Mandatory, Position = 0, ValueFromRemainingArguments)]
    [string[]]$Path,

    [string[]]$AllowModule = @()
)

$ErrorActionPreference = 'Stop'

# The same YY-Thunks release the build links, as pinned in Get-YYThunks.ps1
# (build\Compatibility.cmake reads it from there too).
$fetchScript = Get-Content -Raw (Join-Path $PSScriptRoot 'Get-YYThunks.ps1')
if ($fetchScript -notmatch "\`$Version = '([0-9.]+)'") { throw "Could not read the YY-Thunks version from Get-YYThunks.ps1" }
$version = $Matches[1]
$configRoot = Get-Item (Join-Path $PSScriptRoot "YY-Thunks\$version") -ErrorAction SilentlyContinue
if (-not $configRoot) {
    throw "YY-Thunks $version not found under $PSScriptRoot\YY-Thunks. Run build\Get-YYThunks.ps1 first."
}

$targets = @{
    0x014C = @{ Name = 'x86'; Os = 'Windows XP SP3 (5.1.2600)';            List = 'Config\x86\5.1.2600.txt'; Major = 5; Minor = 1 }
    0x8664 = @{ Name = 'x64'; Os = 'Windows XP x64 / Server 2003 (5.2.3790)'; List = 'Config\x64\5.2.3790.txt'; Major = 5; Minor = 2 }
}

$exportCache = @{}
function Get-ExportTable([string]$file) {
    if ($exportCache.ContainsKey($file)) { return $exportCache[$file] }

    # [module.dll] headers, then one "ordinal=Name" line per export; an
    # export with no name is "ordinal=".
    $table = @{}
    $current = $null
    foreach ($line in [IO.File]::ReadLines($file)) {
        if ($line.Length -eq 0) { continue }
        if ($line[0] -eq '[') {
            $current = @{ Names = New-Object 'System.Collections.Generic.HashSet[string]'
                          Ordinals = New-Object 'System.Collections.Generic.HashSet[int]' }
            $table[$line.Substring(1, $line.Length - 2).ToLowerInvariant()] = $current
            continue
        }
        $eq = $line.IndexOf('=')
        if ($eq -lt 0 -or -not $current) { continue }
        [void]$current.Ordinals.Add([int]$line.Substring(0, $eq))
        if ($eq + 1 -lt $line.Length) { [void]$current.Names.Add($line.Substring($eq + 1)) }
    }
    $exportCache[$file] = $table
    return $table
}

function Read-PeImports([string]$file) {
    $b = [IO.File]::ReadAllBytes($file)
    $pe = [BitConverter]::ToInt32($b, 0x3C)
    if ([BitConverter]::ToUInt32($b, $pe) -ne 0x00004550) { throw "$file is not a PE image" }

    $machine     = [BitConverter]::ToUInt16($b, $pe + 4)
    $numSections = [BitConverter]::ToUInt16($b, $pe + 6)
    $optSize     = [BitConverter]::ToUInt16($b, $pe + 20)
    $opt         = $pe + 24
    $is64        = [BitConverter]::ToUInt16($b, $opt) -eq 0x20B
    $dirs        = $opt + $(if ($is64) { 112 } else { 96 })

    $sections = for ($i = 0; $i -lt $numSections; $i++) {
        $s = $opt + $optSize + 40 * $i
        [pscustomobject]@{
            Va   = [BitConverter]::ToUInt32($b, $s + 12)
            Size = [Math]::Max([BitConverter]::ToUInt32($b, $s + 8), [BitConverter]::ToUInt32($b, $s + 16))
            Raw  = [BitConverter]::ToUInt32($b, $s + 20)
        }
    }
    $toOffset = {
        param([uint32]$rva)
        foreach ($s in $sections) {
            if ($rva -ge $s.Va -and $rva -lt $s.Va + $s.Size) { return [int]($rva - $s.Va + $s.Raw) }
        }
        throw "RVA 0x{0:X} is outside every section of $file" -f $rva
    }
    $readString = {
        param([int]$off)
        $end = $off
        while ($b[$end] -ne 0) { $end++ }
        [Text.Encoding]::ASCII.GetString($b, $off, $end - $off)
    }
    $readThunks = {
        param([uint32]$rva)
        $off = & $toOffset $rva
        while ($true) {
            if ($is64) {
                $v = [BitConverter]::ToUInt64($b, $off); $off += 8
                if ($v -eq 0) { break }
                $byOrdinal = ($v -band 0x8000000000000000) -ne 0
                $low = [uint32]($v -band 0x7FFFFFFF)
            } else {
                $v = [BitConverter]::ToUInt32($b, $off); $off += 4
                if ($v -eq 0) { break }
                $byOrdinal = ($v -band 0x80000000) -ne 0
                $low = [uint32]($v -band 0x7FFFFFFF)
            }
            if ($byOrdinal) { [pscustomobject]@{ Name = $null; Ordinal = [int]($low -band 0xFFFF) } }
            else            { [pscustomobject]@{ Name = (& $readString ((& $toOffset $low) + 2)); Ordinal = $null } }
        }
    }

    $modules = @()

    # Import directory: 20-byte descriptors, terminated by a zero entry.
    $impRva = [BitConverter]::ToUInt32($b, $dirs + 8)
    if ($impRva) {
        $d = & $toOffset $impRva
        while ([BitConverter]::ToUInt32($b, $d + 12) -ne 0) {
            $names = [BitConverter]::ToUInt32($b, $d)
            if (-not $names) { $names = [BitConverter]::ToUInt32($b, $d + 16) }
            $modules += [pscustomobject]@{
                Module = & $readString (& $toOffset ([BitConverter]::ToUInt32($b, $d + 12)))
                Delay  = $false
                Functions = @(& $readThunks $names)
            }
            $d += 20
        }
    }

    # Delay-import directory: 32-byte descriptors, RVA-based (Attributes = 1).
    $delayRva = [BitConverter]::ToUInt32($b, $dirs + 13 * 8)
    if ($delayRva) {
        $d = & $toOffset $delayRva
        while ([BitConverter]::ToUInt32($b, $d + 4) -ne 0) {
            $modules += [pscustomobject]@{
                Module = & $readString (& $toOffset ([BitConverter]::ToUInt32($b, $d + 4)))
                Delay  = $true
                Functions = @(& $readThunks ([BitConverter]::ToUInt32($b, $d + 16)))
            }
            $d += 32
        }
    }

    [pscustomobject]@{
        Machine   = $machine
        OsVersion = [version]::new([BitConverter]::ToUInt16($b, $opt + 40), [BitConverter]::ToUInt16($b, $opt + 42))
        Subsystem = [version]::new([BitConverter]::ToUInt16($b, $opt + 48), [BitConverter]::ToUInt16($b, $opt + 50))
        Modules   = $modules
    }
}

$allowed = @($AllowModule | ForEach-Object { $_.ToLowerInvariant() })
$failedBinaries = 0

$files = foreach ($p in $Path) { Resolve-Path $p | ForEach-Object { $_.Path } }
foreach ($file in $files) {
    $image = Read-PeImports $file
    $target = $targets[[int]$image.Machine]
    if (-not $target) { throw "$file has unsupported machine type 0x{0:X4}" -f $image.Machine }

    $exports = Get-ExportTable (Join-Path $configRoot.FullName $target.List)
    $problems = New-Object System.Collections.Generic.List[string]
    $limit = [version]::new($target.Major, $target.Minor)

    if ($image.Subsystem -gt $limit) { $problems.Add("subsystem version $($image.Subsystem) is above $limit") }
    if ($image.OsVersion -gt $limit) { $problems.Add("operating system version $($image.OsVersion) is above $limit") }

    $functionCount = 0
    foreach ($m in $image.Modules) {
        $key = $m.Module.ToLowerInvariant()
        $kind = if ($m.Delay) { 'delay-imports' } else { 'imports' }
        if ($allowed -contains $key) { continue }

        $table = $exports[$key]
        if (-not $table) {
            $problems.Add("$kind $($m.Module), which does not exist on $($target.Os)")
            continue
        }
        foreach ($f in $m.Functions) {
            $functionCount++
            if ($f.Name) {
                if (-not $table.Names.Contains($f.Name)) { $problems.Add("$kind $($m.Module)!$($f.Name), which $($target.Os) does not export") }
            } elseif (-not $table.Ordinals.Contains($f.Ordinal)) {
                $problems.Add("$kind $($m.Module)!#$($f.Ordinal), which $($target.Os) does not export")
            }
        }
    }

    $label = "[$($target.Name)] $file"
    if ($problems.Count -eq 0) {
        Write-Host "PASS  $label  ($($image.Modules.Count) modules, $functionCount functions, subsystem $($image.Subsystem))"
    } else {
        $failedBinaries++
        Write-Host "FAIL  $label"
        foreach ($problem in $problems) { Write-Host "        $problem" }
    }
}

exit $failedBinaries
