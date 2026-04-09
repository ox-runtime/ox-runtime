# ox-runtime

**WORK-IN-PROGRESS** - This is still a prototype and is not (yet) fully compliant with the OpenXR spec.

This is a runtime implementation of the OpenXR spec.

## Outputs

Build artifacts are written under `build/<platform>/bin`:

- `ox_runtime.dll`/`libox_runtime.so`/`libox_runtime.dylib}`
- `ox_runtime.json`

## Build

```bash
cmake -S . -B build/win-x64
cmake --build build/win-x64 --config Release
```

## Test

```bash
cmake --build build/win-x64 --target runtime_tests --config Release
```

The runtime tests use injected mock driver bindings. They do not require `ox.exe` or `ox_ipc_server`.

## Runtime Driver Resolution

At runtime, driver loading resolves in this order:

1. An already injected test driver
2. `OX_RUNTIME_DRIVER`
3. `OX_USE_SIMULATOR=1` fallback to the local simulator driver folder
4. `ox_ipc_client` as the default path
