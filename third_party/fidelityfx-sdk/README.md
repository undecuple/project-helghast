# AMD FidelityFX SDK headers

This directory contains the minimal C API headers needed to load the official FidelityFX SDK v1.1.4 DirectX 12
runtime. They are copied unchanged from:

https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/v1.1.4/ffx-api/include/ffx_api

The SDK is MIT-licensed; see `LICENSE.md`. Releases fetch the official v1.1.4 archive by a pinned SHA-256, verify its
signed `PrebuiltSignedDLL/amd_fidelityfx_dx12.dll` by a second pinned hash and Authenticode, and ship that DLL
separately. No SDK binary is committed to this repository.
