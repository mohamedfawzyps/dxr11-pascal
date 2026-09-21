<#
    patch_sample.ps1 -Source <upstream .cpp> -Dest <generated .cpp>

    Works around an upstream bug shared by the D3D12Raytracing samples.

    D3D12Raytracing<Name>::m_descriptorsAllocated is declared but never
    initialised by the constructor. It is only zeroed in
    ReleaseDeviceDependentResources(), which does not run before the first
    CreateDeviceDependentResources(). So AllocateDescriptor() reads an
    uninitialised member on first init and computes a garbage descriptor index,
    which CreateUnorderedAccessView rejects:

        D3D12 ERROR: ID3D12Device::CreateUnorderedAccessView: Specified CPU
        descriptor handle ... does not refer to a location in a descriptor heap.
        [ EXECUTION ERROR #646: INVALID_DESCRIPTOR_HANDLE ]

    followed by a GPU stall and an access violation. The sample object is a
    local in WinMain, so whether it bites depends on stack contents. It
    reproduces reliably with the MSVC toolchain here, and it reproduces with no
    proxy present, so it is not ours.

    Confirmed present in:
      D3D12RaytracingHelloWorld     (.h:65, zeroed .cpp:568, read .cpp:677)
      D3D12RaytracingSimpleLighting (.h:77, zeroed .cpp:736, read .cpp:848)

    We patch a generated copy so the samples checkout stays pristine, and fail
    loudly if the anchor moves rather than silently producing an unpatched build.
#>
param(
    [Parameter(Mandatory=$true)][string]$Source,
    [Parameter(Mandatory=$true)][string]$Dest
)

$ErrorActionPreference = 'Stop'

$anchor = 'm_raytracingOutputResourceUAVDescriptorHeapIndex(UINT_MAX)'
$lines  = [IO.File]::ReadAllLines($Source)

$idx = -1
for ($i = 0; $i -lt $lines.Length; $i++) {
    if ($lines[$i] -like "*$anchor*") { $idx = $i; break }
}
if ($idx -lt 0) {
    Write-Error "patch anchor not found in ${Source}: '$anchor'"
    exit 1
}

# Already initialised upstream? Then the bug is fixed and we copy through.
$alreadyFixed = $false
for ($i = [Math]::Max(0, $idx - 12); $i -le $idx; $i++) {
    if ($lines[$i] -like '*m_descriptorsAllocated(*') { $alreadyFixed = $true }
}

if ($alreadyFixed) {
    Copy-Item -LiteralPath $Source -Destination $Dest -Force
    Write-Host "[patch] m_descriptorsAllocated already initialised upstream, copied unchanged"
    exit 0
}

# Match the anchor line's indentation so the init list stays tidy.
$indent = ($lines[$idx] -replace '\S.*$', '')
$fix    = "${indent}m_descriptorsAllocated(0),"

$out = New-Object System.Collections.Generic.List[string]
$out.AddRange([string[]]$lines[0..($idx - 1)])
$out.Add($fix)
$out.AddRange([string[]]$lines[$idx..($lines.Length - 1)])

[IO.File]::WriteAllLines($Dest, $out)
Write-Host "[patch] inserted '$($fix.Trim())' before ctor init of $anchor (line $($idx + 1))"
