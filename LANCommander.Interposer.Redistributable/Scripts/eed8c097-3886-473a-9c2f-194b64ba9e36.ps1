$configPath = Join-Path $InstallDirectory ".interposer\Config.yml"

if (Test-Path $configPath) {
    Write-ReplaceContentInFile -Pattern "Username:.*" -Substitution "Username: '$NewPlayerAlias'" -FilePath $configPath
}
