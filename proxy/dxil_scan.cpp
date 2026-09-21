#include "dxil_scan.h"

#include <cstring>

namespace {

// DXIL containers are DXBC-shaped: "DXBC", a 16 byte digest, version, total
// size, part count, then a table of part offsets. Each part is a 4 character
// tag, a size, and the data.
#pragma pack(push, 1)
struct ContainerHeader {
    UINT32 magic;
    BYTE   digest[16];
    UINT16 major, minor;
    UINT32 sizeInBytes;
    UINT32 partCount;
};
#pragma pack(pop)

const UINT32 kDxbcMagic = 0x43425844;   // 'DXBC'

}  // namespace

UINT64 Dxr11ContainerFeatureFlags(const void* data, SIZE_T size) {
    if (!data || size < sizeof(ContainerHeader)) return 0;
    const BYTE* base = static_cast<const BYTE*>(data);

    ContainerHeader h;
    std::memcpy(&h, base, sizeof(h));
    if (h.magic != kDxbcMagic) return 0;
    // Trust the caller's size over the header's, and bound the part table.
    if (h.sizeInBytes > size) return 0;
    if (h.partCount > (size - sizeof(ContainerHeader)) / sizeof(UINT32)) return 0;

    const SIZE_T tableAt = sizeof(ContainerHeader);
    for (UINT32 i = 0; i < h.partCount; ++i) {
        UINT32 off = 0;
        std::memcpy(&off, base + tableAt + i * sizeof(UINT32), sizeof(off));
        if (off > size || size - off < 8) continue;

        UINT32 partSize = 0;
        std::memcpy(&partSize, base + off + 4, sizeof(partSize));
        if (std::memcmp(base + off, "SFI0", 4) != 0) continue;
        if (partSize < sizeof(UINT64) || size - off - 8 < sizeof(UINT64)) return 0;

        UINT64 flags = 0;
        std::memcpy(&flags, base + off + 8, sizeof(flags));
        return flags;
    }
    return 0;
}

bool Dxr11ContainerUsesRayQuery(const void* data, SIZE_T size) {
    return (Dxr11ContainerFeatureFlags(data, size)
            & kDxilFeatureRaytracingTier11) != 0;
}
