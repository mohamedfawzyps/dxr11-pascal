// Phase 4 (S1) - stand-in for a DISPATCH_RAYS command signature on Tier 1.0.
//
// A Tier 1.0 driver refuses to build one at all:
//
//     ID3D12Device::CreateCommandSignature: D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS
//     can't be used in a command signature unless the device supports
//     D3D12_RAYTRACING_TIER_1_1+.
//
// so the application cannot even reach ExecuteIndirect. The shim hands back one
// of these instead: a real COM object that carries the requested description and
// nothing else. ExecuteIndirect recognises it and does the work itself.
//
// Deliberately NOT a real command signature built from a substitute argument
// type. That was the other option, and it fails badly: a DISPATCH signature with
// the DISPATCH_RAYS stride would read a shader table GPU address as a thread
// group count, so a leak past our interception would hang the GPU. An object the
// runtime does not recognise fails instead, which is the better failure.
//
// ID3D12CommandSignature adds no methods of its own, so this is only the eight
// inherited from IUnknown, ID3D12Object and ID3D12DeviceChild.

#pragma once

#include <d3d12.h>
#include <vector>

// Private interface, so ExecuteIndirect can recognise one of ours.
// {C41D9A72-8B36-4E5A-93C7-0E2B77A15D48}
extern const GUID IID_Dxr11CommandSignature;

class Dxr11CommandSignature : public ID3D12CommandSignature {
public:
    Dxr11CommandSignature(ID3D12Device* device, const D3D12_COMMAND_SIGNATURE_DESC& desc);
    ~Dxr11CommandSignature();

    UINT ByteStride() const { return m_byteStride; }

    // Returns the stand-in if `maybe` is one of ours, else null. Does not change
    // the reference count of the returned pointer.
    static Dxr11CommandSignature* From(ID3D12CommandSignature* maybe);

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG   STDMETHODCALLTYPE AddRef() override;
    ULONG   STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override;
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR Name) override;
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppvDevice) override;

private:
    ID3D12Device* m_device;      // real device, for GetDevice
    UINT          m_byteStride;
    LONG          m_refs;
};
