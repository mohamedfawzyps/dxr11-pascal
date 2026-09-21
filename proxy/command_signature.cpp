// Phase 4 (S1) - DISPATCH_RAYS command signature stand-in.
// See command_signature.h for why it is not a real signature.

#include "command_signature.h"
#include "proxy_log.h"

#include <windows.h>

// {C41D9A72-8B36-4E5A-93C7-0E2B77A15D48}
const GUID IID_Dxr11CommandSignature =
    { 0xc41d9a72, 0x8b36, 0x4e5a, { 0x93, 0xc7, 0x0e, 0x2b, 0x77, 0xa1, 0x5d, 0x48 } };

Dxr11CommandSignature::Dxr11CommandSignature(ID3D12Device* device,
                                             const D3D12_COMMAND_SIGNATURE_DESC& desc)
    : m_device(device), m_byteStride(desc.ByteStride), m_refs(1) {
    if (m_device) m_device->AddRef();
}

Dxr11CommandSignature::~Dxr11CommandSignature() {
    if (m_device) m_device->Release();
}

Dxr11CommandSignature* Dxr11CommandSignature::From(ID3D12CommandSignature* maybe) {
    if (!maybe) return nullptr;
    Dxr11CommandSignature* self = nullptr;
    if (FAILED(maybe->QueryInterface(IID_Dxr11CommandSignature, (void**)&self)) || !self)
        return nullptr;
    self->Release();      // caller does not own the returned pointer
    return self;
}

HRESULT STDMETHODCALLTYPE Dxr11CommandSignature::QueryInterface(REFIID riid, void** ppvObject) {
    if (!ppvObject) return E_POINTER;
    if (riid == IID_Dxr11CommandSignature) {
        AddRef(); *ppvObject = this; return S_OK;
    }
    if (riid == __uuidof(IUnknown)          || riid == __uuidof(ID3D12Object) ||
        riid == __uuidof(ID3D12DeviceChild) || riid == __uuidof(ID3D12Pageable) ||
        riid == __uuidof(ID3D12CommandSignature)) {
        AddRef(); *ppvObject = static_cast<ID3D12CommandSignature*>(this); return S_OK;
    }
    *ppvObject = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE Dxr11CommandSignature::AddRef() { return (ULONG)InterlockedIncrement(&m_refs); }
ULONG STDMETHODCALLTYPE Dxr11CommandSignature::Release() {
    LONG n = InterlockedDecrement(&m_refs);
    if (n == 0) delete this;
    return (ULONG)n;
}

// Private data is not implemented. A command signature is a description an app
// creates and passes straight to ExecuteIndirect; none of the test apps, nor
// Unreal, attach anything to one. Say so rather than pretend to store it.
HRESULT STDMETHODCALLTYPE Dxr11CommandSignature::GetPrivateData(REFGUID, UINT* pDataSize, void*) {
    if (pDataSize) *pDataSize = 0;
    return DXGI_ERROR_NOT_FOUND;
}
HRESULT STDMETHODCALLTYPE Dxr11CommandSignature::SetPrivateData(REFGUID, UINT, const void*) {
    ProxyLog("[dxr-tier-11-proxy-log] SetPrivateData on a DISPATCH_RAYS signature stand-in is ignored\n");
    return E_NOTIMPL;
}
HRESULT STDMETHODCALLTYPE Dxr11CommandSignature::SetPrivateDataInterface(REFGUID, const IUnknown*) {
    ProxyLog("[dxr-tier-11-proxy-log] SetPrivateDataInterface on a DISPATCH_RAYS signature stand-in is ignored\n");
    return E_NOTIMPL;
}
HRESULT STDMETHODCALLTYPE Dxr11CommandSignature::SetName(LPCWSTR) { return S_OK; }

HRESULT STDMETHODCALLTYPE Dxr11CommandSignature::GetDevice(REFIID riid, void** ppvDevice) {
    if (!m_device) return E_NOINTERFACE;
    return m_device->QueryInterface(riid, ppvDevice);
}
