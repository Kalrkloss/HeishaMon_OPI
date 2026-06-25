param(
  [Parameter(Mandatory = $true)]
  [ValidateSet("devkitc-opi", "zero-qspi")]
  [string]$Board,

  [string]$Port,

  [switch]$Upload,

  [switch]$VerboseBuild
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoDir = Resolve-Path (Join-Path $scriptDir "..")
$sketchDir = Join-Path $repoDir "HeishaMon"
$sketchFile = Join-Path $sketchDir "HeishaMon.ino"
$partitionsCsv = Join-Path $sketchDir "partitions.csv"

if (-not (Get-Command arduino-cli -ErrorAction SilentlyContinue)) {
  throw "arduino-cli not found in PATH. Install Arduino CLI first."
}

if (-not (Test-Path $sketchFile)) {
  throw "Sketch not found: $sketchFile"
}

switch ($Board) {
  "devkitc-opi" {
    $boardName = "ESP32-S3-DevKit-C (16MB QSPI Flash, 8MB OPI PSRAM)"
    $fqbn = "esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashMode=qio,FlashSize=16M,PSRAM=opi,PartitionScheme=custom"
    $partitionSource = Join-Path $sketchDir "partitions_16m_opi.csv"
  }
  "zero-qspi" {
    $boardName = "ESP32-S3 Zero (4MB QSPI Flash, 2MB QSPI PSRAM)"
    $fqbn = "esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashMode=qio,FlashSize=4M,PSRAM=enabled,PartitionScheme=custom"
    $partitionSource = Join-Path $sketchDir "partitions_4m_qspi.csv"
  }
}

if (-not (Test-Path $partitionSource)) {
  throw "Partition file not found: $partitionSource"
}

Write-Host "Target: $boardName"
Write-Host "FQBN:   $fqbn"

$copiedPartition = $false
try {
  Copy-Item -Path $partitionSource -Destination $partitionsCsv -Force
  $copiedPartition = $true

  $compileArgs = @(
    "compile"
    "--output-dir", "."
    "--warnings=none"
    "-b", $fqbn
    "HeishaMon.ino"
  )

  if ($VerboseBuild) {
    $compileArgs += "--verbose"
  }

  Push-Location $sketchDir
  try {
    & arduino-cli @compileArgs
    if ($LASTEXITCODE -ne 0) {
      throw "Compile failed with exit code $LASTEXITCODE"
    }

    if ($Upload) {
      if ([string]::IsNullOrWhiteSpace($Port)) {
        throw "-Port is required when -Upload is set"
      }

      $uploadArgs = @(
        "upload"
        "-b", $fqbn
        "-p", $Port
        "--input-dir", "."
      )

      & arduino-cli @uploadArgs
      if ($LASTEXITCODE -ne 0) {
        throw "Upload failed with exit code $LASTEXITCODE"
      }
    }
  }
  finally {
    Pop-Location
  }
}
finally {
  if ($copiedPartition -and (Test-Path $partitionsCsv)) {
    Remove-Item -Path $partitionsCsv -Force
  }
}

if ($Upload) {
  Write-Host "Done: build + upload successful for $Board on $Port"
}
else {
  Write-Host "Done: build successful for $Board"
}
