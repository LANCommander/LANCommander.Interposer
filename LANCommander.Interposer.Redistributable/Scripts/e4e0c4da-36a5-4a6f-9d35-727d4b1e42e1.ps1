# Create .interposer directory structure
$interposerDir = Join-Path $InstallDirectory ".interposer"
$logsDir = Join-Path $interposerDir "Logs"

New-Item -ItemType Directory -Force -Path $interposerDir | Out-Null
New-Item -ItemType Directory -Force -Path $logsDir | Out-Null

# Create default Config.yml if it doesn't exist
$configPath = Join-Path $interposerDir "Config.yml"

if (-not (Test-Path $configPath)) {
    $defaultConfig = @"
Logging:
  Files: false
  Registry: false
  Downloads: false

Player:
  Username: ''

FastDL:
  Enabled: false
  BaseUrl: ''
  AllowedExtensions: []
  UseDownloadDirectory: true
  DownloadDirectory: ''
  BlockSensitiveFiles: true
  Paths: []
"@
    Set-Content -Path $configPath -Value $defaultConfig -Encoding UTF8
}
