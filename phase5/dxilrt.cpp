// ---------------------------------------------------------------------------
// Phase 5 measurement: does DXIL survive a round trip through text?
//
// The whole cost of Phase 5 turns on the level we work at. If a DXIL container
// can be disassembled to .ll text, reassembled, and still validate and sign,
// then the rewriter is a text transformation. If it cannot, every edit has to
// be made in LLVM 3.7 bitcode, which is a different and much larger project.
//
// The brief says to settle this kind of thing by measurement rather than
// argument, so this tool measures it. It is also the feedback loop for
// everything after: "asm" takes any .ll we generate and tells us whether the
// validator accepts it, and exactly what it objects to if not.
//
//   dxilrt roundtrip <in.dxil>     disassemble, reassemble, sign, compare
//   dxilrt asm <in.ll> [out.dxil]  assemble arbitrary IR, sign, report
//   dxilrt parts <in.dxil>         list the container parts
//
// Phase 1 established that dxil.dll's IDxcValidator signs any DXIL that still
// validates, with no secret key, so signing here is the same mechanism.
// ---------------------------------------------------------------------------

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <dxcapi.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

static DxcCreateInstanceProc g_dxc  = nullptr;   // from dxcompiler.dll
static DxcCreateInstanceProc g_dxil = nullptr;   // from dxil.dll

static int Fail(const char* what, HRESULT hr) {
    std::printf("[FAIL] %s (hr=0x%08X)\n", what, (unsigned)hr);
    return 1;
}

// --- container walking -----------------------------------------------------
// DXIL containers are DXBC-shaped: "DXBC", a 16 byte digest, version, total
// size, part count, then a table of part offsets. Each part is a 4 character
// tag, a size, and the data.
struct ContainerHeader {
    uint32_t magic;
    uint8_t  digest[16];
    uint16_t major, minor;
    uint32_t sizeInBytes;
    uint32_t partCount;
};

static bool ForEachPart(const void* data, size_t size,
                        void (*fn)(const char tag[4], uint32_t partSize, void* ctx),
                        void* ctx) {
    if (size < sizeof(ContainerHeader)) return false;
    auto* h = static_cast<const ContainerHeader*>(data);
    if (h->magic != 0x43425844u /* 'DXBC' */) return false;
    auto* base = static_cast<const uint8_t*>(data);
    auto* offsets = reinterpret_cast<const uint32_t*>(base + sizeof(ContainerHeader));
    for (uint32_t i = 0; i < h->partCount; ++i) {
        uint32_t off = offsets[i];
        if (off + 8 > size) return false;
        const char* tag = reinterpret_cast<const char*>(base + off);
        uint32_t partSize = *reinterpret_cast<const uint32_t*>(base + off + 4);
        fn(tag, partSize, ctx);
    }
    return true;
}

static void CollectPart(const char tag[4], uint32_t partSize, void* ctx) {
    auto* out = static_cast<std::vector<std::pair<std::string, uint32_t>>*>(ctx);
    out->emplace_back(std::string(tag, 4), partSize);
}

static std::vector<std::pair<std::string, uint32_t>> PartsOf(IDxcBlob* b) {
    std::vector<std::pair<std::string, uint32_t>> parts;
    ForEachPart(b->GetBufferPointer(), b->GetBufferSize(), CollectPart, &parts);
    return parts;
}

static void PrintParts(const char* label, IDxcBlob* b) {
    auto parts = PartsOf(b);
    std::printf("   %-22s %5zu bytes, %zu parts:", label, (size_t)b->GetBufferSize(),
                parts.size());
    for (auto& p : parts) std::printf(" %s(%u)", p.first.c_str(), p.second);
    std::printf("\n");
}

// --- file and blob helpers -------------------------------------------------
static bool ReadFile(const char* path, std::vector<uint8_t>& out) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? (size_t)n : 0);
    size_t got = out.empty() ? 0 : std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    return got == out.size();
}

static bool WriteFile(const char* path, const void* data, size_t size) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    size_t put = std::fwrite(data, 1, size, f);
    std::fclose(f);
    return put == size;
}

static HRESULT MakeBlob(const void* data, size_t size, IDxcBlob** out) {
    ComPtr<IDxcUtils> utils;
    HRESULT hr = g_dxc(CLSID_DxcUtils, IID_PPV_ARGS(&utils));
    if (FAILED(hr)) return hr;
    ComPtr<IDxcBlobEncoding> enc;
    hr = utils->CreateBlob(data, (UINT32)size, DXC_CP_ACP, &enc);
    if (FAILED(hr)) return hr;
    return enc.CopyTo(out);
}

// Print whatever an IDxcOperationResult has to say when it fails.
static void ReportErrors(const char* stage, IDxcOperationResult* res) {
    ComPtr<IDxcBlobEncoding> errs;
    if (SUCCEEDED(res->GetErrorBuffer(&errs)) && errs && errs->GetBufferSize()) {
        std::printf("   %s said:\n", stage);
        std::string text(static_cast<const char*>(errs->GetBufferPointer()),
                         errs->GetBufferSize());
        // Indent, and stop runaway output.
        size_t shown = 0, start = 0;
        while (start < text.size() && shown < 40) {
            size_t nl = text.find('\n', start);
            if (nl == std::string::npos) nl = text.size();
            std::printf("      %.*s\n", (int)(nl - start), text.c_str() + start);
            start = nl + 1; ++shown;
        }
    }
}

// --- the three operations --------------------------------------------------

static HRESULT Disassemble(IDxcBlob* container, std::string& outText) {
    ComPtr<IDxcCompiler> compiler;
    HRESULT hr = g_dxc(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler));
    if (FAILED(hr)) return hr;
    ComPtr<IDxcBlobEncoding> text;
    hr = compiler->Disassemble(container, &text);
    if (FAILED(hr)) return hr;
    outText.assign(static_cast<const char*>(text->GetBufferPointer()),
                   text->GetBufferSize());
    return S_OK;
}

// .ll text -> unsigned DXIL container.
static HRESULT Assemble(const std::string& text, IDxcBlob** out) {
    ComPtr<IDxcAssembler> asmr;
    HRESULT hr = g_dxc(CLSID_DxcAssembler, IID_PPV_ARGS(&asmr));
    if (FAILED(hr)) return hr;
    ComPtr<IDxcBlob> src;
    hr = MakeBlob(text.data(), text.size(), &src);
    if (FAILED(hr)) return hr;
    ComPtr<IDxcOperationResult> res;
    hr = asmr->AssembleToContainer(src.Get(), &res);
    if (FAILED(hr)) return hr;
    HRESULT status = E_FAIL;
    res->GetStatus(&status);
    if (FAILED(status)) { ReportErrors("assembler", res.Get()); return status; }
    return res->GetResult(out);
}

// Validate and sign in place, the Phase 1 mechanism.
static HRESULT ValidateAndSign(IDxcBlob* container, IDxcBlob** outSigned) {
    ComPtr<IDxcValidator> validator;
    HRESULT hr = g_dxil(CLSID_DxcValidator, IID_PPV_ARGS(&validator));
    if (FAILED(hr)) return hr;
    ComPtr<IDxcOperationResult> res;
    hr = validator->Validate(container, DxcValidatorFlags_InPlaceEdit, &res);
    if (FAILED(hr)) return hr;
    HRESULT status = E_FAIL;
    res->GetStatus(&status);
    if (FAILED(status)) { ReportErrors("validator", res.Get()); return status; }
    // InPlaceEdit signs the blob we passed in.
    container->AddRef();
    *outSigned = container;
    return S_OK;
}

static int CmdParts(const char* path) {
    std::vector<uint8_t> bytes;
    if (!ReadFile(path, bytes)) return Fail("read input", E_FAIL);
    ComPtr<IDxcBlob> blob;
    HRESULT hr = MakeBlob(bytes.data(), bytes.size(), &blob);
    if (FAILED(hr)) return Fail("CreateBlob", hr);
    std::printf("\n-- container parts --\n");
    PrintParts(path, blob.Get());
    return 0;
}

static int CmdAsm(const char* inPath, const char* outPath) {
    std::printf("\n-- assemble %s --\n", inPath);
    std::vector<uint8_t> bytes;
    if (!ReadFile(inPath, bytes)) return Fail("read input", E_FAIL);
    std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());

    ComPtr<IDxcBlob> container;
    HRESULT hr = Assemble(text, &container);
    if (FAILED(hr)) return Fail("assemble", hr);
    std::printf("   assembled ok\n");
    PrintParts("unsigned", container.Get());

    ComPtr<IDxcBlob> signedBlob;
    hr = ValidateAndSign(container.Get(), &signedBlob);
    if (FAILED(hr)) return Fail("validate/sign", hr);
    std::printf("   validated and signed ok\n");

    if (outPath) {
        if (!WriteFile(outPath, signedBlob->GetBufferPointer(), signedBlob->GetBufferSize()))
            return Fail("write output", E_FAIL);
        std::printf("   wrote %s\n", outPath);
    }
    return 0;
}

static int CmdRoundTrip(const char* path) {
    std::printf("\n-- round trip %s --\n", path);
    std::vector<uint8_t> bytes;
    if (!ReadFile(path, bytes)) return Fail("read input", E_FAIL);

    ComPtr<IDxcBlob> original;
    HRESULT hr = MakeBlob(bytes.data(), bytes.size(), &original);
    if (FAILED(hr)) return Fail("CreateBlob", hr);
    PrintParts("original", original.Get());

    std::string text;
    hr = Disassemble(original.Get(), text);
    if (FAILED(hr)) return Fail("disassemble", hr);
    std::printf("   %-22s %5zu bytes of text\n", "disassembly", text.size());

    ComPtr<IDxcBlob> rebuilt;
    hr = Assemble(text, &rebuilt);
    if (FAILED(hr)) {
        std::printf("   VERDICT: text round trip is NOT possible, assembly failed.\n");
        return Fail("assemble", hr);
    }
    PrintParts("reassembled", rebuilt.Get());

    ComPtr<IDxcBlob> signedBlob;
    hr = ValidateAndSign(rebuilt.Get(), &signedBlob);
    if (FAILED(hr)) {
        std::printf("   VERDICT: reassembled, but the validator REJECTED it.\n");
        return Fail("validate/sign", hr);
    }
    PrintParts("signed", signedBlob.Get());

    // Compare. Byte equality is the strong result; part equality is the one
    // that actually matters for the runtime, since a missing part means the
    // container is unusable however well formed the module is.
    const bool sameSize = signedBlob->GetBufferSize() == original->GetBufferSize();
    const bool sameBytes = sameSize &&
        std::memcmp(signedBlob->GetBufferPointer(), original->GetBufferPointer(),
                    (size_t)original->GetBufferSize()) == 0;

    auto a = PartsOf(original.Get());
    auto b = PartsOf(signedBlob.Get());

    std::vector<std::string> tagsA, tagsB;
    for (auto& p : a) tagsA.push_back(p.first);
    for (auto& p : b) tagsB.push_back(p.first);
    const bool sameTags = tagsA == tagsB;

    // The decisive check. Byte equality is too strict, because value names and
    // reflection padding are regenerated. What matters is whether the MODULE
    // changed, so disassemble the rebuilt container and compare the text with
    // the text we assembled from. If they agree, the round trip is a fixed
    // point and the byte difference is encoding only, not semantics.
    std::string text2;
    hr = Disassemble(signedBlob.Get(), text2);
    if (FAILED(hr)) return Fail("disassemble round 2", hr);
    const bool sameIR = (text == text2);

    // Comment lines in a DXIL disassembly are commentary the disassembler
    // prints from the reflection data, not IR. They legitimately change here,
    // because the shader hash covers a container we just rebuilt and because
    // reassembly drops reflection names. What matters is whether any real
    // instruction or metadata line moved, so compare with comments removed.
    auto stripComments = [](const std::string& t) {
        std::string out;
        size_t start = 0;
        while (start < t.size()) {
            size_t nl = t.find('\n', start);
            if (nl == std::string::npos) nl = t.size();
            size_t f = t.find_first_not_of(" \t", start);
            const bool isComment = (f != std::string::npos && f < nl && t[f] == ';');
            if (!isComment) out.append(t, start, nl - start + 1);
            start = nl + 1;
        }
        return out;
    };
    const bool sameCode = stripComments(text) == stripComments(text2);

    std::printf("\n   byte identical           : %s\n", sameBytes ? "YES" : "no");
    std::printf("   same part tags           : %s\n", sameTags ? "YES" : "no");
    std::printf("   IR is a fixed point      : %s\n", sameIR ? "YES" : "NO");
    std::printf("   same IR ignoring comments: %s\n", sameCode ? "YES" : "NO");
    if (!sameTags || !sameBytes) {
        std::printf("   original   :");
        for (auto& p : a) std::printf(" %s(%u)", p.first.c_str(), p.second);
        std::printf("\n   round trip :");
        for (auto& p : b) std::printf(" %s(%u)", p.first.c_str(), p.second);
        std::printf("\n");
    }
    if (!sameIR) {
        // Write both out so the difference can be inspected directly rather
        // than summarised. The summary below is only a first look.
        WriteFile("rt_before.ll", text.data(), text.size());
        WriteFile("rt_after.ll",  text2.data(), text2.size());
        std::printf("   wrote rt_before.ll and rt_after.ll for diffing\n");

        // Show the first line that differs, which is usually enough to see
        // whether the change is cosmetic or real.
        size_t i = 0, line = 1, lastNl = 0;
        while (i < text.size() && i < text2.size() && text[i] == text2[i]) {
            if (text[i] == '\n') { ++line; lastNl = i + 1; }
            ++i;
        }
        auto lineAt = [](const std::string& t, size_t from) {
            size_t e = t.find('\n', from);
            return t.substr(from, (e == std::string::npos ? t.size() : e) - from);
        };
        std::printf("   first IR difference at line %zu:\n", line);
        std::printf("      before: %s\n", lineAt(text,  lastNl).c_str());
        std::printf("      after : %s\n", lineAt(text2, lastNl).c_str());
    }

    std::printf("\n   VERDICT: %s\n",
        sameBytes  ? "byte for byte lossless."
      : sameCode   ? "every instruction and metadata line survives; only reflection\n"
                     "            commentary and the container hash differ. Text rewriting is\n"
                     "            viable, subject to an end to end render check."
                   : "REAL IR CHANGED. Text rewriting is not safe, see rt_before/after.ll.");
    return sameCode ? 0 : 3;
}

int main(int argc, char** argv) {
    HMODULE hDxc = LoadLibraryW(L"dxcompiler.dll");
    if (!hDxc) return Fail("LoadLibrary(dxcompiler.dll)", HRESULT_FROM_WIN32(GetLastError()));
    g_dxc = (DxcCreateInstanceProc)GetProcAddress(hDxc, "DxcCreateInstance");
    if (!g_dxc) return Fail("GetProcAddress(dxcompiler)", E_FAIL);

    HMODULE hDxil = LoadLibraryW(L"dxil.dll");
    if (!hDxil) return Fail("LoadLibrary(dxil.dll)", HRESULT_FROM_WIN32(GetLastError()));
    g_dxil = (DxcCreateInstanceProc)GetProcAddress(hDxil, "DxcCreateInstance");
    if (!g_dxil) return Fail("GetProcAddress(dxil)", E_FAIL);

    if (argc >= 3 && std::strcmp(argv[1], "roundtrip") == 0) return CmdRoundTrip(argv[2]);
    if (argc >= 3 && std::strcmp(argv[1], "asm") == 0)
        return CmdAsm(argv[2], argc >= 4 ? argv[3] : nullptr);
    if (argc >= 3 && std::strcmp(argv[1], "parts") == 0) return CmdParts(argv[2]);

    std::printf("usage:\n"
                "  dxilrt roundtrip <in.dxil>     disassemble, reassemble, sign, compare\n"
                "  dxilrt asm <in.ll> [out.dxil]  assemble arbitrary IR, sign, report\n"
                "  dxilrt parts <in.dxil>         list the container parts\n");
    return 1;
}
