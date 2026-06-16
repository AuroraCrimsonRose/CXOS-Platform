param(
    [string]$Boot,
    [string]$Stage2,
    [string]$Kernel,
    [string]$Out
)

$sectorSize = 512
$diskSize = 16MB

New-Item -ItemType Directory -Force -Path (Split-Path $Out) | Out-Null

$disk = New-Object byte[] $diskSize

function Write-At($src, $offset) {
    $bytes = [IO.File]::ReadAllBytes($src)
    [Array]::Copy($bytes, 0, $disk, $offset, $bytes.Length)
}

Write-At $Boot    0
Write-At $Stage2  512
Write-At $Kernel  (18 * $sectorSize)

# --- self-sizing: compute the kernel's sector count and patch it into the
#     loader's kernel_sectors field, located by the "KSNT" magic marker. ---
$kernelBytes   = [IO.File]::ReadAllBytes($Kernel)
$kernelSectors = [int][Math]::Ceiling($kernelBytes.Length / $sectorSize)

# the marker dd KSNT_MAGIC = 0x4B534E54 is stored little-endian: 54 4E 53 4B,
# immediately followed by the 4-byte count we overwrite.
$marker = [byte[]](0x54, 0x4E, 0x53, 0x4B)

# stage2 lives at disk offset 512; scan the stage2 region for the marker.
$searchStart = 512
$searchEnd   = 512 + (16 * $sectorSize)   # stage2 is 16 sectors
$found = -1
for ($i = $searchStart; $i -lt $searchEnd - 4; $i++) {
    if ($disk[$i]   -eq $marker[0] -and
        $disk[$i+1] -eq $marker[1] -and
        $disk[$i+2] -eq $marker[2] -and
        $disk[$i+3] -eq $marker[3]) {
        $found = $i
        break
    }
}

if ($found -lt 0) {
    Write-Error "KSNT marker not found in stage2 - cannot patch kernel sector count!"
    exit 1
}

# write the 4-byte little-endian count right after the marker
$countOffset = $found + 4
$disk[$countOffset]     = [byte]( $kernelSectors        -band 0xFF)
$disk[$countOffset + 1] = [byte](($kernelSectors -shr 8)  -band 0xFF)
$disk[$countOffset + 2] = [byte](($kernelSectors -shr 16) -band 0xFF)
$disk[$countOffset + 3] = [byte](($kernelSectors -shr 24) -band 0xFF)

Write-Host ("Kernel: {0} bytes = {1} sectors (patched at disk offset {2})" -f $kernelBytes.Length, $kernelSectors, $countOffset)

# sanity: warn if the kernel would collide with the 16MB filesystem assumptions
$kernelEndSector = 18 + $kernelSectors
if ($kernelEndSector -gt ($diskSize / $sectorSize)) {
    Write-Error "Kernel too large for the boot image!"
    exit 1
}

[IO.File]::WriteAllBytes($Out, $disk)
Write-Host "Boot image written: $Out"