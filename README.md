# ox-runtime

**ox-runtime** is an implementation of the OpenXR Runtime [specification](https://registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html), built for [ox](https://github.com/ox-runtime/ox).

It implements the logic of an OpenXR runtime (as defined in the spec), and delegates to a [driver implementation](https://github.com/ox-runtime/ox/blob/main/docs/drivers.md) (to talk to the underlying XR hardware).

> [!WARNING]
> **WORK-IN-PROGRESS** - This is still heavily under-development and is not (yet) fully compliant with the OpenXR spec.


## Build

```bash
cmake -B build
cmake --build build --config Release
```

The build artifacts will be written under `build/bin`:

- `ox_runtime.dll`/`libox_runtime.so`/`libox_runtime.dylib`
- `ox_runtime.json`

## Test

```bash
cmake --build build --target runtime_tests --config Release
```

The runtime tests use injected mock driver bindings. They do not require the [ox](https://github.com/ox-runtime/ox) host process or [ipc proxy](https://github.com/ox-runtime/ox-ipc-proxy).

## Runtime Driver Resolution

Driver loading resolves in the following order (at runtime):

1. An already injected test driver
2. `OX_RUNTIME_DRIVER`
3. `OX_USE_SIMULATOR=1` fallback to the [simulator driver](https://github.com/ox-runtime/ox-sim-driver) (installed at `./drivers/simulator`)
4. [ox_ipc_client](https://github.com/ox-runtime/ox-ipc-proxy) as the default driver (which connects to the `ox` process)

## References

- [OpenXR Specification](https://www.khronos.org/registry/OpenXR/)
- [OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK)
- [OpenXR Loader Design](https://github.com/KhronosGroup/OpenXR-SDK-Source/blob/main/src/loader/LoaderDesign.md)
