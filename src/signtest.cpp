// Phase 1 - DXIL container re-signing test.
//
// Goal: find out whether Microsoft's dxil.dll validator will re-"sign" a DXIL
// container that we have tampered with after compilation.
//
// What "signing" means here: a DXIL container is a DXBC-style container whose
// header carries a 16-byte DigestValue. That digest is NOT a cryptographic
// signature with a secret key - it is a hash (a tweaked MD5) computed over the
// container body. IDxcValidator (as provided by dxil.dll) computes and writes
// that digest as a side effect of a *successful* validation. The D3D12 runtime
// refuses to create a pipeline whose digest does not match, so producing a
// valid digest == "signing".
//
// The experiment:
//   1. Compile a trivial pixel shader with DXC (this already produces a signed
//      container, because the compiler runs the validator internally).
//   2. Flip one byte inside the DXIL bytecode part of the container.
//   3. Load dxil.dll, create IDxcValidator from it, and call Validate() with
//      in-place edit so it would re-write the digest.
//   4. Report whether validation succeeded (i.e. whether it re-signed).
//
// Build (Windows, x64): see CMakeLists.txt. Requires dxcompiler.dll + dxil.dll
// from a recent DXC release on PATH / next to the exe.

#include <windows.h>
#include <dxcapi.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <string>
#include <cstring>
#include <cstddef>

using Microsoft::WRL::ComPtr;

static const char* kShaderSrc =
    "float4 main() : SV_Target { return float4(1, 0, 0, 1); }\n";

// DXIL / DXBC container layout ------------------------------------------------
#pragma pack(push, 1)
struct ContainerHeader {
    uint32_t fourCC;          // 'DXBC'
    uint8_t  digest[16];      // the "signature" we care about
    uint16_t versionMajor;
    uint16_t versionMinor;
    uint32_t containerSize;
    uint32_t partCount;
    // uint32_t partOffset[partCount] follows
};
struct PartHeader {
    uint32_t fourCC;
    uint32_t size;            // payload size, not including this header
};
#pragma pack(pop)

static constexpr uint32_t MakeFourCC(char a, char b, char c, char d) {
    return uint32_t(uint8_t(a)) | (uint32_t(uint8_t(b)) << 8) |
           (uint32_t(uint8_t(c)) << 16) | (uint32_t(uint8_t(d)) << 24);
}
static const uint32_t kFourCC_DXBC = MakeFourCC('D', 'X', 'B', 'C');
static const uint32_t kFourCC_DXIL = MakeFourCC('D', 'X', 'I', 'L');

static void PrintDigest(const char* label, const uint8_t* d) {
    std::printf("%s", label);
    for (int i = 0; i < 16; ++i) std::printf("%02x", d[i]);
    std::printf("\n");
}

static bool Fail(const char* what, HRESULT hr) {
    std::printf("[FAIL] %s (hr=0x%08lx)\n", what, static_cast<unsigned long>(hr));
    return false;
}

// Returns the byte offset of a flippable byte inside the DXIL bytecode part,
// or 0 if the part could not be located.
static size_t FindDxilBytecodeByte(const std::vector<uint8_t>& c) {
    if (c.size() < sizeof(ContainerHeader)) return 0;
    const auto* h = reinterpret_cast<const ContainerHeader*>(c.data());
    if (h->fourCC != kFourCC_DXBC) return 0;
    const auto* offs = reinterpret_cast<const uint32_t*>(c.data() + sizeof(ContainerHeader));
    for (uint32_t i = 0; i < h->partCount; ++i) {
        size_t off = offs[i];
        if (off + sizeof(PartHeader) > c.size()) continue;
        const auto* p = reinterpret_cast<const PartHeader*>(c.data() + off);
        if (p->fourCC == kFourCC_DXIL) {
            // Skip the part header and the DxilProgramHeader (0x20 bytes) so we
            // land squarely in the LLVM bitcode payload.
            size_t payload = off + sizeof(PartHeader) + 0x20;
            size_t mid = payload + (p->size > 0x40 ? (p->size - 0x20) / 2 : 4);
            if (mid < c.size()) return mid;
        }
    }
    return 0;
}

int main() {
    // --- 1. Compile the shader with DXC ------------------------------------
    HMODULE hDxc = LoadLibraryW(L"dxcompiler.dll");
    if (!hDxc) { Fail("LoadLibrary(dxcompiler.dll)", HRESULT_FROM_WIN32(GetLastError())); return 1; }
    auto dxcCreate = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(hDxc, "DxcCreateInstance"));
    if (!dxcCreate) { Fail("GetProcAddress(dxcompiler DxcCreateInstance)", E_FAIL); return 1; }

    ComPtr<IDxcCompiler3> compiler;
    ComPtr<IDxcUtils> utils;
    if (FAILED(dxcCreate(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)))) return Fail("create compiler", E_FAIL), 1;
    if (FAILED(dxcCreate(CLSID_DxcUtils, IID_PPV_ARGS(&utils)))) return Fail("create utils", E_FAIL), 1;

    DxcBuffer src{ kShaderSrc, std::strlen(kShaderSrc), DXC_CP_UTF8 };
    const wchar_t* args[] = { L"-E", L"main", L"-T", L"ps_6_0" };

    ComPtr<IDxcResult> compileResult;
    if (FAILED(compiler->Compile(&src, args, _countof(args), nullptr, IID_PPV_ARGS(&compileResult))))
        return Fail("Compile()", E_FAIL), 1;
    HRESULT cstatus = E_FAIL; compileResult->GetStatus(&cstatus);
    if (FAILED(cstatus)) {
        ComPtr<IDxcBlobUtf8> err;
        compileResult->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&err), nullptr);
        if (err && err->GetStringLength()) std::printf("compile errors:\n%s\n", err->GetStringPointer());
        return Fail("shader failed to compile", cstatus), 1;
    }

    ComPtr<IDxcBlob> object;
    if (FAILED(compileResult->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr)) || !object)
        return Fail("get object blob", E_FAIL), 1;

    std::vector<uint8_t> container(
        reinterpret_cast<uint8_t*>(object->GetBufferPointer()),
        reinterpret_cast<uint8_t*>(object->GetBufferPointer()) + object->GetBufferSize());
    std::printf("[ok] compiled: %zu-byte container\n", container.size());
    PrintDigest("     digest as compiled (valid): ",
                reinterpret_cast<ContainerHeader*>(container.data())->digest);

    // --- 2. Tamper: flip one byte inside the DXIL bytecode -----------------
    size_t victim = FindDxilBytecodeByte(container);
    if (!victim) return Fail("could not locate DXIL part", E_FAIL), 1;
    uint8_t before = container[victim];
    container[victim] ^= 0xFF;
    std::printf("[ok] flipped byte at offset %zu: 0x%02x -> 0x%02x\n",
                victim, before, container[victim]);
    // Zero the digest too, so any "signed" outcome must come from the validator.
    std::memset(reinterpret_cast<ContainerHeader*>(container.data())->digest, 0, 16);

    // --- 3. Load dxil.dll and ask its validator to re-sign -----------------
    HMODULE hDxil = LoadLibraryW(L"dxil.dll");
    if (!hDxil) {
        std::printf("[note] dxil.dll not found - the signing validator is unavailable.\n");
        return Fail("LoadLibrary(dxil.dll)", HRESULT_FROM_WIN32(GetLastError())), 2;
    }
    auto dxilCreate = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(hDxil, "DxcCreateInstance"));
    if (!dxilCreate) return Fail("GetProcAddress(dxil DxcCreateInstance)", E_FAIL), 1;

    ComPtr<IDxcValidator> validator;
    if (FAILED(dxilCreate(CLSID_DxcValidator, IID_PPV_ARGS(&validator))))
        return Fail("create validator from dxil.dll", E_FAIL), 1;

    // Wrap the tampered bytes in a writable blob so in-place edit can re-write
    // the digest, then validate.
    ComPtr<IDxcBlobEncoding> blob;
    if (FAILED(utils->CreateBlob(container.data(), (UINT32)container.size(), DXC_CP_ACP, &blob)))
        return Fail("CreateBlob", E_FAIL), 1;

    ComPtr<IDxcOperationResult> valResult;
    HRESULT hr = validator->Validate(blob.Get(), DxcValidatorFlags_InPlaceEdit, &valResult);
    if (FAILED(hr)) return Fail("Validate() call", hr), 1;

    HRESULT valStatus = E_FAIL; valResult->GetStatus(&valStatus);

    // --- 4. Report ---------------------------------------------------------
    std::printf("\n==== RESULT ====\n");
    if (SUCCEEDED(valStatus)) {
        std::printf("SIGNED: validator accepted the tampered container and re-wrote the digest.\n");
        PrintDigest("     new digest: ",
                    reinterpret_cast<uint8_t*>(blob->GetBufferPointer()) + offsetof(ContainerHeader, digest));
    } else {
        std::printf("NOT SIGNED: validator rejected the tampered container (status=0x%08lx).\n",
                    static_cast<unsigned long>(valStatus));
        ComPtr<IDxcBlobEncoding> err;
        valResult->GetErrorBuffer(&err);
        if (err && err->GetBufferSize())
            std::printf("validator says:\n%.*s\n", (int)err->GetBufferSize(),
                        reinterpret_cast<const char*>(err->GetBufferPointer()));
    }
    return SUCCEEDED(valStatus) ? 0 : 3;
}
