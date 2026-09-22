#include "dxc_host.h"

#include "../dllinfo.h"

#include <dxcapi.h>

#include <windows.h>

#include <cstdio>
#include <mutex>
#include <vector>

namespace dxch {
namespace {

HMODULE g_self = nullptr;
std::once_flag g_once;
bool g_ok = false;
std::string g_why;

HMODULE g_dxcompiler = nullptr;
HMODULE g_dxil = nullptr;
std::string g_versions = "not loaded";
DxcCreateInstanceProc g_createDxc = nullptr;
DxcCreateInstanceProc g_createDxil = nullptr;

// COM without ATL: these objects never leave this file, so a minimal holder is
// less machinery than pulling in ComPtr.
template <class T>
struct Ref {
    T* p = nullptr;
    ~Ref() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

std::wstring HostDirectory() {
    wchar_t path[MAX_PATH] = {};
    HMODULE mod = g_self;
    if (!mod) {
        // Fall back to whichever module this code is linked into.
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&HostDirectory), &mod);
    }
    const DWORD n = GetModuleFileNameW(mod, path, MAX_PATH);
    if (!n || n == MAX_PATH) return L"";
    std::wstring s(path, n);
    const size_t slash = s.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"" : s.substr(0, slash + 1);
}

// Load by FULL PATH. A bare LoadLibraryW would hand back the application's own
// copy if it has one, which may be a different version of DXC entirely.
HMODULE LoadBeside(const std::wstring& dir, const wchar_t* name) {
    if (dir.empty()) return nullptr;
    return LoadLibraryW((dir + name).c_str());
}

void Init() {
    const std::wstring dir = HostDirectory();
    if (dir.empty()) { g_why = "cannot locate the shim's own directory"; return; }

    g_dxcompiler = LoadBeside(dir, L"dxcompiler.dll");
    if (!g_dxcompiler) {
        g_why = "dxcompiler.dll is not next to the shim; the rewriter needs it "
                "to convert a DXIL container to text and back";
        return;
    }
    g_dxil = LoadBeside(dir, L"dxil.dll");
    if (!g_dxil) {
        g_why = "dxil.dll is not next to the shim; without it nothing can be signed";
        return;
    }
    g_createDxc = reinterpret_cast<DxcCreateInstanceProc>(
        GetProcAddress(g_dxcompiler, "DxcCreateInstance"));
    g_createDxil = reinterpret_cast<DxcCreateInstanceProc>(
        GetProcAddress(g_dxil, "DxcCreateInstance"));
    if (!g_createDxc || !g_createDxil) {
        g_why = "DxcCreateInstance missing from dxcompiler.dll or dxil.dll";
        return;
    }
    g_versions = "dxcompiler " + dllinfo::OfFile(dir + L"dxcompiler.dll") +
                 ", dxil " + dllinfo::OfFile(dir + L"dxil.dll");
    g_ok = true;
}

bool Ready(std::string* error) {
    std::call_once(g_once, Init);
    if (!g_ok && error) *error = g_why;
    return g_ok;
}

bool MakeBlob(const void* data, size_t size, IDxcBlob** out, std::string* error) {
    Ref<IDxcUtils> utils;
    if (FAILED(g_createDxc(CLSID_DxcUtils, IID_PPV_ARGS(&utils)))) {
        if (error) *error = "cannot create IDxcUtils";
        return false;
    }
    Ref<IDxcBlobEncoding> enc;
    if (FAILED(utils->CreateBlob(data, static_cast<UINT32>(size), DXC_CP_ACP, &enc))) {
        if (error) *error = "CreateBlob failed";
        return false;
    }
    enc.p->AddRef();
    *out = enc.p;
    return true;
}

std::string ErrorTextOf(IDxcOperationResult* res) {
    Ref<IDxcBlobEncoding> errs;
    if (FAILED(res->GetErrorBuffer(&errs)) || !errs || !errs->GetBufferSize())
        return "";
    return std::string(static_cast<const char*>(errs->GetBufferPointer()),
                       errs->GetBufferSize());
}

}  // namespace

void SetHostModule(HMODULE self) { g_self = self; }

bool Available(std::string* error) { return Ready(error); }

const char* Versions() { return g_versions.c_str(); }

bool Disassemble(const void* container, size_t size, std::string* text,
                 std::string* error) {
    if (!Ready(error)) return false;
    Ref<IDxcBlob> blob;
    if (!MakeBlob(container, size, &blob, error)) return false;

    Ref<IDxcCompiler> compiler;
    if (FAILED(g_createDxc(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)))) {
        if (error) *error = "cannot create IDxcCompiler";
        return false;
    }
    Ref<IDxcBlobEncoding> out;
    if (FAILED(compiler->Disassemble(blob.p, &out)) || !out) {
        if (error) *error = "Disassemble failed; is this a DXIL container?";
        return false;
    }
    text->assign(static_cast<const char*>(out->GetBufferPointer()),
                 out->GetBufferSize());
    return true;
}

bool AssembleAndSign(const std::string& text, std::vector<uint8_t>* out,
                     std::string* error) {
    if (!Ready(error)) return false;
    Ref<IDxcBlob> src;
    if (!MakeBlob(text.data(), text.size(), &src, error)) return false;

    Ref<IDxcAssembler> asmr;
    if (FAILED(g_createDxc(CLSID_DxcAssembler, IID_PPV_ARGS(&asmr)))) {
        if (error) *error = "cannot create IDxcAssembler";
        return false;
    }
    Ref<IDxcOperationResult> ares;
    if (FAILED(asmr->AssembleToContainer(src.p, &ares)) || !ares) {
        if (error) *error = "AssembleToContainer failed";
        return false;
    }
    HRESULT status = E_FAIL;
    ares->GetStatus(&status);
    if (FAILED(status)) {
        if (error) *error = "assembler rejected the rewritten IR: " + ErrorTextOf(ares.p);
        return false;
    }
    Ref<IDxcBlob> built;
    if (FAILED(ares->GetResult(&built)) || !built) {
        if (error) *error = "assembler produced no container";
        return false;
    }

    // InPlaceEdit signs the blob we hand it, the Phase 1 mechanism.
    Ref<IDxcValidator> validator;
    if (FAILED(g_createDxil(CLSID_DxcValidator, IID_PPV_ARGS(&validator)))) {
        if (error) *error = "cannot create IDxcValidator from dxil.dll";
        return false;
    }
    Ref<IDxcOperationResult> vres;
    if (FAILED(validator->Validate(built.p, DxcValidatorFlags_InPlaceEdit, &vres)) ||
        !vres) {
        if (error) *error = "Validate failed to run";
        return false;
    }
    vres->GetStatus(&status);
    if (FAILED(status)) {
        if (error) *error = "validator rejected the rewritten shader: " + ErrorTextOf(vres.p);
        return false;
    }

    const uint8_t* p = static_cast<const uint8_t*>(built->GetBufferPointer());
    out->assign(p, p + built->GetBufferSize());
    return true;
}

bool RewriteContainer(const void* container, size_t size,
                      bool (*xform)(const std::string&, std::string*, std::string*, void*),
                      void* ctx, std::vector<uint8_t>* out, std::string* error) {
    std::string text;
    if (!Disassemble(container, size, &text, error)) return false;
    std::string lowered, why;
    if (!xform(text, &lowered, &why, ctx)) {
        if (error) *error = why;
        return false;
    }
    return AssembleAndSign(lowered, out, error);
}

}  // namespace dxch
