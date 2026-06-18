param(
    [string]$Boot,
    [string]$Stage2,
    [string]$Kernel,
    [string]$Out
)
# /CXK/tools/pad_boot.ps1  -  v5 image: boot + stage 2 + kernel.
# Layout: boot.bin @ LBA 0, stage2.bin @ LBA 1, kernel @ LBA 33 (after the
# 32-sector stage 2 region). Stage 2 loads the kernel from LBA 33.

$sectorSize = 512
$diskSize   = 4MB

New-Item -ItemType Directory -Force -Path (Split-Path $Out) | Out-Null
$disk = New-Object byte[] $diskSize

function Write-At($src, $offset) {
    $bytes = [IO.File]::ReadAllBytes($src)
    [Array]::Copy($bytes, 0, $disk, $offset, $bytes.Length)
}

Write-At $Boot   0
Write-At $Stage2 (1  * $sectorSize)     # stage 2 at LBA 1
Write-At $Kernel (33 * $sectorSize)     # kernel at LBA 33

[IO.File]::WriteAllBytes($Out, $disk)
Write-Host "v5 boot image written: $Out (boot@LBA0, stage2@LBA1, kernel@LBA33)"