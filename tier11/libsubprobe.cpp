// The D3D12 runtime's rules for a DXIL library's OWN subobjects, which the
// shim's association model follows (proxy/geom_index_so.cpp, 0.58.0): what
// an export list includes, renames, associations across libraries, and the
// state object's associations by name. The spec does not say; this asks the
// runtime, and checks every answer against what was measured (identical on
// WARP and the GTX 1070, runtime 1.619). Observable: ClosestHit reads b0
// space1, so a pipeline whose closest-hit gets no local root signature
// covering it fails; LrsBad covers something else.
//
//   libsubprobe.exe [warp] [-v]      exit code: cases that differ
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <dxcapi.h>
#include <wrl/client.h>
#include <cstdio>
#include <string>
#include <vector>
using Microsoft::WRL::ComPtr;

static ComPtr<ID3D12Device5> g_dev;
static bool g_verbose = false;
static int g_differ = 0;

static std::vector<uint8_t> Compile(const char* src) {
    static HMODULE m = LoadLibraryW(L"dxcompiler.dll");
    auto create = (DxcCreateInstanceProc)GetProcAddress(m, "DxcCreateInstance");
    ComPtr<IDxcCompiler3> c;
    create(CLSID_DxcCompiler, IID_PPV_ARGS(&c));
    const wchar_t* args[] = { L"-T", L"lib_6_5" };
    DxcBuffer b{ src, strlen(src), DXC_CP_UTF8 };
    ComPtr<IDxcResult> r;
    c->Compile(&b, args, 2, nullptr, IID_PPV_ARGS(&r));
    HRESULT st; r->GetStatus(&st);
    if (FAILED(st)) {
        ComPtr<IDxcBlobUtf8> e; r->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&e), nullptr);
        std::printf("COMPILE FAILED: %s\n", e ? e->GetStringPointer() : "?");
        return {};
    }
    ComPtr<IDxcBlob> o; r->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&o), nullptr);
    return std::vector<uint8_t>((uint8_t*)o->GetBufferPointer(),
                                (uint8_t*)o->GetBufferPointer() + o->GetBufferSize());
}

static const char* kHead = R"(
RaytracingAccelerationStructure Scene : register(t0);
RWStructuredBuffer<uint4> Out : register(u0);
cbuffer Rec : register(b0, space1) { uint RecTag; };
struct Pay { uint4 v; };
)";
static const char* kShaders = R"(
[shader("raygeneration")] void RayGen() {
    RayDesc r; r.Origin = float3(0,0,1); r.Direction = float3(0,0,-1); r.TMin = 0; r.TMax = 10;
    Pay p; p.v = 0; TraceRay(Scene, 0, 0xFF, 0, 1, 0, r, p); Out[0] = p.v;
}
[shader("miss")] void Miss(inout Pay p) { p.v = 1; }
[shader("closesthit")] void ClosestHit(inout Pay p, BuiltInTriangleIntersectionAttributes a) { p.v = RecTag; }
)";
// A local root signature that does NOT cover b0 space1: if it is the one that
// reaches ClosestHit, creation fails.
static const char* kGood = "LocalRootSignature LrsRec = { \"RootConstants(num32BitConstants=1, b0, space=1)\" };\n";
static const char* kBad  = "LocalRootSignature LrsBad = { \"RootConstants(num32BitConstants=1, b7, space=9)\" };\n";
static const char* kCfg  = "RaytracingShaderConfig Cfg = { 16, 8 };\nRaytracingPipelineConfig Pc = { 1 };\n";
static const char* kHg   = "TriangleHitGroup HitGroup = { \"\", \"ClosestHit\" };\n";

static ComPtr<ID3D12RootSignature> g_global;

static void Drain(ID3D12InfoQueue* q) {
    if (!q) return;
    for (UINT64 i = 0; i < q->GetNumStoredMessages(); ++i) {
        SIZE_T n = 0; q->GetMessage(i, nullptr, &n);
        std::vector<char> b(n); auto* m = (D3D12_MESSAGE*)b.data();
        q->GetMessage(i, m, &n);
        if (m->Severity <= D3D12_MESSAGE_SEVERITY_WARNING) std::printf("      | %.300s\n", m->pDescription);
    }
    q->ClearStoredMessages();
}

struct Lib { std::vector<uint8_t> code; std::vector<D3D12_EXPORT_DESC> ex; };

static void Try(const char* expect, const char* label, std::vector<Lib> libs, const std::vector<const wchar_t*>& ask,
                bool collection = false, bool soHitGroup = false, const wchar_t* soHgClosest = nullptr,
                const wchar_t* dxilAssocOf = nullptr, std::vector<const wchar_t*> dxilAssocTo = {}) {
    (void)ask;   // the identifiers asked for are the ones `expect` names
    std::vector<D3D12_STATE_SUBOBJECT> s;
    std::vector<D3D12_DXIL_LIBRARY_DESC> ld(libs.size());
    for (size_t i = 0; i < libs.size(); ++i) {
        ld[i].DXILLibrary = { libs[i].code.data(), libs[i].code.size() };
        ld[i].NumExports = (UINT)libs[i].ex.size();
        ld[i].pExports = libs[i].ex.empty() ? nullptr : libs[i].ex.data();
        s.push_back({ D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &ld[i] });
    }
    D3D12_GLOBAL_ROOT_SIGNATURE g{ g_global.Get() };
    s.push_back({ D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &g });
    D3D12_HIT_GROUP_DESC hg{};
    hg.HitGroupExport = L"SoHitGroup"; hg.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
    hg.ClosestHitShaderImport = soHgClosest;
    if (soHitGroup) s.push_back({ D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hg });
    D3D12_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION da{ dxilAssocOf, (UINT)dxilAssocTo.size(),
                                                   dxilAssocTo.empty() ? nullptr : dxilAssocTo.data() };
    if (dxilAssocOf) s.push_back({ D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &da });
    ComPtr<ID3D12InfoQueue> q; g_dev.As(&q);
    if (q) q->ClearStoredMessages();
    D3D12_STATE_OBJECT_DESC d{ collection ? D3D12_STATE_OBJECT_TYPE_COLLECTION
                                          : D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE,
                               (UINT)s.size(), s.data() };
    ComPtr<ID3D12StateObject> so;
    HRESULT hr = g_dev->CreateStateObject(&d, IID_PPV_ARGS(&so));
    if (SUCCEEDED(hr) && collection) {
        // Link it into a pipeline, as Unreal does.
        D3D12_EXISTING_COLLECTION_DESC ec{ so.Get(), 0, nullptr };
        D3D12_STATE_SUBOBJECT s2[2] = { { D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION, &ec },
                                        { D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &g } };
        D3D12_STATE_OBJECT_DESC d2{ D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, 2, s2 };
        ComPtr<ID3D12StateObject> p;
        hr = g_dev->CreateStateObject(&d2, IID_PPV_ARGS(&p));
        so = p;
    }
    // `expect`: "fail", or "ok" then any "Name:yes" / "Name:no" identifiers.
    std::string got = SUCCEEDED(hr) ? "ok" : "fail";
    if (SUCCEEDED(hr)) {
        ComPtr<ID3D12StateObjectProperties> pr; so.As(&pr);
        std::string e = expect;
        for (size_t at = e.find(' '); at != std::string::npos; at = e.find(' ', at + 1)) {
            const size_t colon = e.find(':', at);
            const std::string name = e.substr(at + 1, colon - at - 1);
            const std::wstring w(name.begin(), name.end());
            got += " " + name + (pr->GetShaderIdentifier(w.c_str()) ? ":yes" : ":no");
        }
    }
    const bool same = got == expect;
    if (!same) ++g_differ;
    std::printf("  %-8s %-58s %s%s\n", same ? "as" : "DIFFERS", label, got.c_str(),
                same ? "" : (std::string("  (measured: ") + expect + ")").c_str());
    if (!same || g_verbose) Drain(q.Get());
    else if (q) q->ClearStoredMessages();
}

int main(int argc, char** argv) {
    bool warp = false;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "warp")) warp = true;
        if (!strcmp(argv[i], "-v")) g_verbose = true;
    }
    ComPtr<ID3D12Debug> dbg;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) dbg->EnableDebugLayer();
    ComPtr<IDXGIFactory6> f; CreateDXGIFactory2(0, IID_PPV_ARGS(&f));
    ComPtr<IDXGIAdapter1> a;
    if (warp) f->EnumWarpAdapter(IID_PPV_ARGS(&a));
    else f->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&a));
    if (FAILED(D3D12CreateDevice(a.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g_dev)))) { std::printf("no device\n"); return 100; }
    std::printf("device: %s, debug layer %s\n", warp ? "WARP" : "hardware", dbg ? "on" : "OFF");
    if (!dbg) std::printf("(without the debug layer the failures still show, their reasons do not)\n");

    D3D12_ROOT_PARAMETER p[2] = {};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_ROOT_SIGNATURE_DESC rd{ 2, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ComPtr<ID3DBlob> blob, err;
    D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
    g_dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_global));

    std::string all = std::string(kHead) + kGood + kBad + kCfg + kHg +
        "SubobjectToExportsAssociation AssocRec = { \"LrsRec\", \"HitGroup\" };\n"
        "SubobjectToExportsAssociation AssocBad = { \"LrsBad\", \"RayGen;Miss\" };\n" + kShaders;
    Lib LA{ Compile(all.c_str()) };
    auto E = [](const wchar_t* n, const wchar_t* from = nullptr) { return D3D12_EXPORT_DESC{ n, from, D3D12_EXPORT_FLAG_NONE }; };
    const std::vector<const wchar_t*> ask = { L"RayGen", L"HitGroup", L"HG2" };

    std::printf("One library with its own local root signatures, associations, hit group and configs:\n");
    Try("ok HitGroup:yes", "no export list", { LA }, ask);
    Lib l = LA; l.ex = { E(L"RayGen"), E(L"Miss"), E(L"ClosestHit") };
    Try("fail", "exports RayGen Miss ClosestHit", { l }, ask);
    l.ex.push_back(E(L"HitGroup"));
    Try("fail", "exports + HitGroup", { l }, ask);
    for (auto* n : { L"LrsRec", L"AssocRec", L"Cfg", L"Pc", L"LrsBad", L"AssocBad" }) l.ex.push_back(E(n));
    Try("ok HitGroup:yes", "exports + HitGroup + every subobject name", { l }, ask);
    l.ex = { E(L"RayGen"), E(L"Miss"), E(L"ClosestHit"), E(L"Cfg"), E(L"Pc") };
    Try("fail", "exports RayGen Miss ClosestHit Cfg Pc", { l }, ask);
    l.ex = { E(L"RayGen"), E(L"Miss"), E(L"ClosestHit"), E(L"HitGroup"), E(L"Cfg"), E(L"Pc") };
    Try("fail", "exports + HitGroup Cfg Pc", { l }, ask);
    l.ex = { E(L"RayGen"), E(L"Miss"), E(L"ClosestHit"), E(L"HitGroup"), E(L"Cfg"), E(L"Pc"), E(L"LrsRec") };
    Try("ok HitGroup:yes", "exports + HitGroup Cfg Pc LrsRec", { l }, ask);
    l.ex = { E(L"RayGen"), E(L"Miss"), E(L"ClosestHit"), E(L"HitGroup"), E(L"Cfg"), E(L"Pc"), E(L"AssocRec") };
    Try("fail", "exports + HitGroup Cfg Pc AssocRec", { l }, ask);
    l.ex = { E(L"RayGen"), E(L"Miss"), E(L"ClosestHit"), E(L"HitGroup"), E(L"Cfg"), E(L"Pc"), E(L"LrsRec"), E(L"AssocRec") };
    Try("ok HitGroup:yes", "exports + HitGroup Cfg Pc LrsRec AssocRec", { l }, ask);
    l.ex = { E(L"RayGen"), E(L"Miss"), E(L"ClosestHit"), E(L"HG2", L"HitGroup"), E(L"Cfg"), E(L"Pc"), E(L"LrsRec"), E(L"AssocRec") };
    Try("fail", "... HitGroup renamed HG2", { l }, ask);
    l.ex = { E(L"RayGen"), E(L"Miss"), E(L"CH2", L"ClosestHit"), E(L"HitGroup"), E(L"Cfg"), E(L"Pc"), E(L"LrsRec"), E(L"AssocRec") };
    Try("fail", "... ClosestHit renamed CH2", { l }, ask);
    l.ex = { E(L"RayGen"), E(L"Miss"), E(L"ClosestHit"), E(L"HitGroup") };
    Try("fail", "collection: exports + HitGroup", { l }, ask, true);
    l.ex = { E(L"RayGen"), E(L"Miss"), E(L"ClosestHit"), E(L"HitGroup"), E(L"Cfg"), E(L"Pc"), E(L"LrsRec"), E(L"AssocRec") };
    Try("ok HitGroup:yes", "collection: exports + all but LrsBad AssocBad", { l }, ask, true);

    // Association naming a closest-hit, which the export list renames; hit
    // group declared at state object scope over the new name.
    std::string byCh = std::string(kHead) + kGood + kCfg +
        "SubobjectToExportsAssociation AssocCh = { \"LrsRec\", \"ClosestHit\" };\n" + kShaders;
    Lib LC{ Compile(byCh.c_str()) };
    std::printf("Library associates LrsRec with ClosestHit by name; state object hit group:\n");
    LC.ex = {};
    Try("ok SoHitGroup:yes", "no export list, SO hit group over ClosestHit", { LC }, { L"SoHitGroup" }, false, true, L"ClosestHit");
    LC.ex = { E(L"RayGen"), E(L"Miss"), E(L"CH2", L"ClosestHit"), E(L"Cfg"), E(L"Pc"), E(L"LrsRec"), E(L"AssocCh") };
    Try("fail", "ClosestHit renamed CH2, SO hit group over CH2", { LC }, { L"SoHitGroup" }, false, true, L"CH2");
    LC.ex = { E(L"RayGen"), E(L"Miss"), E(L"ClosestHit"), E(L"CH2", L"ClosestHit"), E(L"Cfg"), E(L"Pc"), E(L"LrsRec"), E(L"AssocCh") };
    Try("fail", "exported twice, ClosestHit and CH2; SO hit group over CH2", { LC }, { L"SoHitGroup" }, false, true, L"CH2");

    // Across libraries: A declares the signature only, B associates it.
    std::string onlyRs = std::string(kGood);
    std::string usesIt = std::string(kHead) + kCfg + kHg +
        "SubobjectToExportsAssociation AssocX = { \"LrsRec\", \"HitGroup\" };\n" + kShaders;
    Lib A{ Compile(onlyRs.c_str()) }, B{ Compile(usesIt.c_str()) };
    std::printf("Library B associates LrsRec, declared in library A:\n");
    if (!A.code.empty() && !B.code.empty()) {
        Try("ok HitGroup:yes", "A and B", { A, B }, ask);
        Try("fail", "B only (control: must fail)", { B }, ask);
        Lib A2 = A; A2.ex = { E(L"LrsRec") };
        Try("ok HitGroup:yes", "A with export list {LrsRec}, B", { A2, B }, ask);
        Try("ok HitGroup:yes", "collection: A and B", { A, B }, ask, true);
    }
    // A signature nothing associates is a DEFAULT: does library A's default
    // reach library B's exports? (the spec says a default reaches its own
    // scope only)
    std::string usesDefault = std::string(kHead) + kCfg + kHg + kShaders;
    Lib Bd{ Compile(usesDefault.c_str()) };
    std::printf("Library A's unassociated LrsRec (a default), B has no association:\n");
    Try("fail", "A and B (spec: A's default reaches A only)", { A, Bd }, ask);

    std::printf("Renaming subobjects:\n");
    l.ex = { E(L"RayGen"), E(L"Miss"), E(L"ClosestHit"), E(L"HitGroup"), E(L"Cfg"), E(L"Pc"), E(L"LrsNew", L"LrsRec"), E(L"AssocRec") };
    Try("fail", "LrsRec renamed LrsNew, AssocRec included", { l }, ask);
    l.ex = { E(L"RayGen"), E(L"Miss"), E(L"ClosestHit"), E(L"HitGroup"), E(L"Cfg"), E(L"Pc"), E(L"LrsNew", L"LrsRec") };
    Try("ok HitGroup:yes", "LrsRec renamed LrsNew, no association (a default)", { l }, ask);
    l.ex = { E(L"RayGen"), E(L"Miss"), E(L"ClosestHit"), E(L"HG2", L"HitGroup"), E(L"Cfg"), E(L"Pc"), E(L"LrsRec") };
    Try("ok HitGroup:no HG2:yes", "HitGroup renamed HG2, LrsRec a default", { l }, ask);
    l.ex = { E(L"RayGen"), E(L"Miss"), E(L"ClosestHit"), E(L"HitGroup"), E(L"HG2", L"HitGroup"), E(L"Cfg"), E(L"Pc"), E(L"LrsRec"), E(L"AssocRec") };
    Try("ok HitGroup:yes HG2:yes", "HitGroup exported twice, HitGroup and HG2", { l }, ask);
    l.ex = { E(L"RayGen"), E(L"Miss"), E(L"ClosestHit"), E(L"HitGroup"), E(L"Cfg"), E(L"Pc"), E(L"AssocRec"), E(L"LrsRec"), E(L"AssocRec") };
    Try("ok HitGroup:yes", "AssocRec listed twice", { l }, ask);
    std::printf("State object DXIL association against the library's own (LrsBad does not cover b0 space1):\n");
    Try("fail", "SO explicit {LrsBad -> HitGroup} vs lib AssocRec", { LA }, ask, false, false, nullptr, L"LrsBad", { L"HitGroup" });
    Try("fail", "SO explicit {LrsBad -> ClosestHit} vs lib AssocRec->HitGroup", { LA }, ask, false, false, nullptr, L"LrsBad", { L"ClosestHit" });
    Try("fail", "SO default {LrsBad} vs lib AssocRec", { LA }, ask, false, false, nullptr, L"LrsBad", {});
    Try("ok HitGroup:yes", "SO explicit {LrsRec -> HitGroup} (same as lib)", { LA }, ask, false, false, nullptr, L"LrsRec", { L"HitGroup" });
    Lib Bd2 = Bd;
    Try("ok HitGroup:yes", "A + Bd, SO default {LrsRec} (A's signature)", { A, Bd2 }, ask, false, false, nullptr, L"LrsRec", {});
    Try("ok HitGroup:yes", "A + Bd, SO explicit {LrsRec -> HitGroup}", { A, Bd2 }, ask, false, false, nullptr, L"LrsRec", { L"HitGroup" });
    // Library default association (empty export list) naming another library's signature.
    std::string defX = std::string(kHead) + kCfg + kHg +
        "SubobjectToExportsAssociation AssocD = { \"LrsRec\", \"\" };\n" + kShaders;
    Lib Bx{ Compile(defX.c_str()) };
    if (!Bx.code.empty()) {
        Try("ok HitGroup:yes", "A + B' where B' default-associates A's LrsRec", { A, Bx }, ask);
        Try("ok HitGroup:yes", "A + B' + C (C: unassociated default LrsBad in its own scope)", { A, Bx, Lib{ Compile(kBad) } }, ask);
    }
    // Two libraries, both explicit for HitGroup, different signatures: conflict?
    std::string conf = std::string(kBad) + "SubobjectToExportsAssociation AssocC = { \"LrsBad\", \"HitGroup\" };\n";
    Lib Cc{ Compile(conf.c_str()) };
    if (!Cc.code.empty()) Try("fail", "A + B + C (C explicit LrsBad -> HitGroup): conflict", { A, B, Cc }, ask);
    std::printf("%d case(s) differ from what was measured\n", g_differ);
    return g_differ;
}
