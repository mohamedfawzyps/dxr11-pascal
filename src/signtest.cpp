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
// The experiment (per trial):
//   1. Compile a trivial pixel shader with DXC (already produces a signed
//      container, because the compiler runs the validator internally).
//   2. Tamper with one byte, in one of two regions:
//        --target=header    corrupt the header digest, leave the DXIL intact.
//        --target=bytecode  flip one byte inside the DXIL bytecode.
//      --target=both (default) runs both trials back to back.
//   3. Load dxil.dll, create IDxcValidator from it, and call Validate() with
//      in-place edit so it would re-write the digest.
//   4. Report whether validation succeeded (i.e. whether it re-signed).
//
// Expectation: HEADER trials are re-signed (the DXIL still validates, so the
// validator recomputes the digest); BYTECODE trials are rejected (the module no
// longer validates, so nothing is signed). That contrast is the whole point:
// the "signature" is a validity gate, not a keyed lock.
//
// Exit code: 0 = every trial matched expectation, 1 = setup/compile error,
// 2 = dxil.dll missing, 3 = a trial contradicted expectation.
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

enum class Target { Header, Bytecode };

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

// Compile kShaderSrc into a signed DXIL container. Returns false on failure.
static bool CompileShader(IDxcCompiler3* compiler, std::vector<uint8_t>& out) {
    DxcBuffer src{ kShaderSrc, std::strlen(kShaderSrc), DXC_CP_UTF8 };
    const wchar_t* args[] = { L"-E", L"main", L"-T", L"ps_6_0" };

    ComPtr<IDxcResult> result;
    if (FAILED(compiler->Compile(&src, args, _countof(args), nullptr, IID_PPV_ARGS(&result))))
        return Fail("Compile()", E_FAIL);
    HRESULT status = E_FAIL; result->GetStatus(&status);
    if (FAILED(status)) {
        ComPtr<IDxcBlobUtf8> err;
        result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&err), nullptr);
        if (err && err->GetStringLength()) std::printf("compile errors:\n%s\n", err->GetStringPointer());
        return Fail("shader failed to compile", status);
    }
    ComPtr<IDxcBlob> object;
    if (FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr)) || !object)
        return Fail("get object blob", E_FAIL);

    auto* p = reinterpret_cast<uint8_t*>(object->GetBufferPointer());
    out.assign(p, p + object->GetBufferSize());
    return true;
}

// Run one trial: compile fresh, tamper in the given region, ask dxil.dll's
// validator to re-sign, and report. Returns true iff the outcome matched the
// expectation for that region (header -> signed, bytecode -> rejected).
static bool RunTrial(Target target, IDxcCompiler3* compiler, IDxcUtils* utils,
                     IDxcValidator* validator) {
    const char* name = (target == Target::Header) ? "HEADER" : "BYTECODE";
    const bool expectSigned = (target == Target::Header);
    std::printf("\n---- trial: %s (expect %s) ----\n", name,
                expectSigned ? "SIGNED" : "REJECTED");

    std::vector<uint8_t> container;
    if (!CompileShader(compiler, container)) return false;
    std::printf("[ok] compiled: %zu-byte container\n", container.size());

    // Remember the digest the compiler produced so we can confirm a re-sign
    // reproduces it exactly.
    uint8_t original[16];
    std::memcpy(original, reinterpret_cast<ContainerHeader*>(container.data())->digest, 16);
    PrintDigest("     digest as compiled (valid): ", original);

    // Tamper.
    if (target == Target::Bytecode) {
        size_t victim = FindDxilBytecodeByte(container);
        if (!victim) return Fail("could not locate DXIL part", E_FAIL);
        uint8_t before = container[victim];
        container[victim] ^= 0xFF;
        std::printf("[ok] flipped DXIL bytecode at offset %zu: 0x%02x -> 0x%02x\n",
                    victim, before, container[victim]);
    } else {
        std::printf("[ok] left DXIL bytecode intact\n");
    }
    // Corrupt the header digest in both trials, so any "signed" result must come
    // from the validator recomputing it rather than from a stale valid digest.
    std::memset(reinterpret_cast<ContainerHeader*>(container.data())->digest, 0xAA, 16);
    PrintDigest("     digest after tamper (bogus): ",
                reinterpret_cast<ContainerHeader*>(container.data())->digest);

    // Wrap the tampered bytes in a writable blob so in-place edit can re-write
    // the digest, then validate.
    ComPtr<IDxcBlobEncoding> blob;
    if (FAILED(utils->CreateBlob(container.data(), (UINT32)container.size(), DXC_CP_ACP, &blob)))
        return Fail("CreateBlob", E_FAIL);

    ComPtr<IDxcOperationResult> valResult;
    HRESULT hr = validator->Validate(blob.Get(), DxcValidatorFlags_InPlaceEdit, &valResult);
    if (FAILED(hr)) return Fail("Validate() call", hr);
    HRESULT valStatus = E_FAIL; valResult->GetStatus(&valStatus);

    const bool signed_ = SUCCEEDED(valStatus);
    if (signed_) {
        const uint8_t* newDigest =
            reinterpret_cast<uint8_t*>(blob->GetBufferPointer()) + offsetof(ContainerHeader, digest);
        std::printf("[=>] SIGNED: validator accepted the container and re-wrote the digest.\n");
        PrintDigest("     new digest: ", newDigest);
        if (target == Target::Header) {
            std::printf("     matches original compiled digest: %s\n",
                        std::memcmp(newDigest, original, 16) == 0 ? "YES" : "no");
        }
    } else {
        std::printf("[=>] NOT SIGNED: validator rejected the container (status=0x%08lx).\n",
                    static_cast<unsigned long>(valStatus));
        ComPtr<IDxcBlobEncoding> err;
        valResult->GetErrorBuffer(&err);
        if (err && err->GetBufferSize())
            std::printf("     validator says: %.*s\n", (int)err->GetBufferSize(),
                        reinterpret_cast<const char*>(err->GetBufferPointer()));
    }

    const bool matched = (signed_ == expectSigned);
    std::printf("[%s] outcome %s expectation.\n", matched ? "PASS" : "FAIL",
                matched ? "matched" : "CONTRADICTED");
    return matched;
}

int main(int argc, char** argv) {
    // --- parse --target ----------------------------------------------------
    bool doHeader = true, doBytecode = true;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            std::printf("usage: signtest [target]\n"
                        "  target: header | bytecode | both  (default both)\n"
                        "  also accepts --header, --bytecode, --both, --target=<t>\n");
            return 0;
        }
        // Normalize header / --header / --target=header all to "header".
        std::string val = a;
        auto eq = a.find('=');
        if (eq != std::string::npos) val = a.substr(eq + 1);
        else if (val.rfind("--", 0) == 0) val = val.substr(2);

        if (val == "header")        { doHeader = true;  doBytecode = false; }
        else if (val == "bytecode") { doHeader = false; doBytecode = true;  }
        else if (val == "both")     { doHeader = true;  doBytecode = true;  }
        else {
            std::printf("unknown option '%s' (want header|bytecode|both; see --help)\n", a.c_str());
            return 1;
        }
    }

    // --- create DXC compiler + utils ---------------------------------------
    HMODULE hDxc = LoadLibraryW(L"dxcompiler.dll");
    if (!hDxc) { Fail("LoadLibrary(dxcompiler.dll)", HRESULT_FROM_WIN32(GetLastError())); return 1; }
    auto dxcCreate = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(hDxc, "DxcCreateInstance"));
    if (!dxcCreate) { Fail("GetProcAddress(dxcompiler DxcCreateInstance)", E_FAIL); return 1; }

    ComPtr<IDxcCompiler3> compiler;
    ComPtr<IDxcUtils> utils;
    if (FAILED(dxcCreate(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)))) return Fail("create compiler", E_FAIL), 1;
    if (FAILED(dxcCreate(CLSID_DxcUtils, IID_PPV_ARGS(&utils)))) return Fail("create utils", E_FAIL), 1;

    // --- load dxil.dll and create its signing validator --------------------
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

    // --- run the requested trials ------------------------------------------
    bool allMatched = true;
    if (doHeader)   allMatched &= RunTrial(Target::Header,   compiler.Get(), utils.Get(), validator.Get());
    if (doBytecode) allMatched &= RunTrial(Target::Bytecode, compiler.Get(), utils.Get(), validator.Get());

    std::printf("\n==== SUMMARY ====\n%s\n",
                allMatched ? "All trials matched expectation." : "A trial contradicted expectation.");
    return allMatched ? 0 : 3;
}
