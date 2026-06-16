param(
    [string]$Out,
    [int]$SizeMB = 16
)

# Don't overwrite an existing filesystem image (it may hold data).
if (Test-Path $Out) {
    Write-Host "Filesystem image already exists, leaving it untouched: $Out"
    exit 0
}

New-Item -ItemType Directory -Force -Path (Split-Path $Out) | Out-Null

$size = $SizeMB * 1MB
$buf  = New-Object byte[] $size      # zero-filled
[IO.File]::WriteAllBytes($Out, $buf)

Write-Host "Created empty $SizeMB MB filesystem image: $Out"