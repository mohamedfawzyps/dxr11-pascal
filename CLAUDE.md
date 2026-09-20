# dxr11-pascal

Exploring whether DXR 1.1 (DirectX Raytracing) can be driven on NVIDIA Pascal
(GTX 10-series) hardware, which lacks vendor DXR support. Work is staged in
phases; each phase is a small, self-contained experiment before committing to a
larger approach.

## Phase 1 - DXIL container signing (current)

Question: the D3D12 runtime rejects shader containers whose 16-byte DXIL digest
does not match. That digest is written by Microsoft's `dxil.dll` validator, not
by us. Before building any custom shader path we need to know: can we take a
DXIL container, modify it, and get `dxil.dll` to re-issue a valid digest?

Test: `src/signtest.cpp` (run with `--target=header|bytecode|both`, default `both`)
1. Compile a trivial pixel shader with DXC (already produces a signed container).
2. Tamper with one byte:
   - `bytecode`: flip a byte inside the DXIL bytecode part.
   - `header`: corrupt the header digest, leave the DXIL intact.
   In both cases the digest is overwritten with a bogus value first, so any
   "signed" result must come from the validator recomputing it.
3. Load `dxil.dll`, create `IDxcValidator`, call `Validate(..., InPlaceEdit)`.
4. Report whether the container gets re-signed. Running `both` shows the two
   outcomes side by side.

Key facts that frame the result:
- The DXIL "signature" is a hash (a tweaked MD5 over the container body), not a
  keyed cryptographic signature. There is no secret. `IDxcValidator` recomputes
  and writes it whenever validation succeeds - re-signing is its intended job.
- Therefore the validator will re-sign ANY container whose DXIL still passes
  validation, but will REFUSE to sign one whose DXIL no longer validates.
- A blind byte-flip in the bytecode almost always corrupts the LLVM bitcode /
  module, so the module fails validation and is NOT re-signed. Flipping a byte
  in the header digest field (or making only edits that stay valid DXIL) is
  re-signed cleanly. So "does it sign?" depends entirely on whether the edit
  leaves valid DXIL behind.

Implication for the project: we cannot hand-patch arbitrary bytes into shader
bytecode and expect a signature. Any custom shader must be emitted as valid
DXIL (or produced through the compiler) and then signed by `dxil.dll`.

## Build / run (Windows x64)

Requires DXC SDK headers plus `dxcompiler.dll` and `dxil.dll` at runtime.

    cmake -B build -DDXC_SDK_DIR=C:/path/to/dxc
    cmake --build build --config Release
    build\Release\signtest.exe                 # runs both trials
    build\Release\signtest.exe --target=bytecode
    # dxcompiler.dll + dxil.dll must be on PATH

Exit codes: 0 = every trial matched expectation (header re-signed, bytecode
rejected), 1 = setup/compile error, 2 = dxil.dll missing, 3 = a trial
contradicted expectation.

## Notes

- This is a Linux dev checkout for editing; the signing path itself is
  Windows-only (`dxil.dll`). Run the test on Windows for empirical results.
