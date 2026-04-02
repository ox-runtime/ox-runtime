# ox-runtime

`ox-runtime` is the standalone OpenXR runtime library repo.

It owns the runtime implementation, the runtime manifest, and the driver-oriented test seam used by unit tests. Production code loads a driver library; tests inject a mock driver callback table directly.

## Outputs

Build artifacts are written under `build/<platform>/bin`:

- `ox_runtime.{dll|so|dylib}`
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
4. `ox_ipc_client` as the default production path
