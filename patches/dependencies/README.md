# Pinned dependency patches

The pinned submodule checkouts remain unchanged. `cmake/PrepareDependency.cmake`
copies the selected sources into the build tree, applies these patches in order,
and configures that overlay. Reconfiguration incorporates source and patch
changes. Incompatible modifications fail with an explanation rather than being
discarded. Dependencies may be updated later after these adaptations have been
reviewed against the new pin.

## plume Metal

Base: `renderbag/plume` commit `898904e`. Upstream inspected through `d723793`.
Each patch applies after the preceding numbered patches.

| Patch | Origin and reason |
| --- | --- |
| `plume-0001-metal-buffer-bounds.patch` | Backport `89a5037`: allocate the aligned push-constant upload size and use the shared texture base class for drawable barriers. The old allocation could be read past its end by `setBytes`, and the drawable cast used the wrong derived class. |
| `plume-0002-metal-object-lifetimes.patch` | Adapt `bda4e70`: scope autorelease pools around Metal operations, retain names/command buffers/encoders that escape the pool, and release owned objects. Also adapt `d890ac8` to destroy the null buffer before its device. Additional fixes release dispatch data and the copied device array and preserve correct ownership of the default device. Retains the pinned swapchain API. |
| `plume-0003-apple-device-capabilities.patch` | Adapt platform guards from `5360587` for iOS and macOS SDK compatibility. Report GPU address support according to Tier 2 argument buffers and the OS availability of `gpuAddress` (iOS 16/macOS 13), rather than inconsistent Apple-family/Metal-3 checks. |
| `plume-0004-metal-resource-retirement.patch` | Fork fixes: clear retired argument-buffer handles, serialize shared descriptor encoding/resource enumeration, make residency dirty state atomic, reset active pipeline pointers at command-list end, and omit empty semaphore-wait command buffers. Resource hazard tracking and render-pass boundaries remain unchanged. |

The Tier 2 requirement follows Apple's [argument buffer documentation](https://developer.apple.com/documentation/metal/improving-cpu-performance-by-using-argument-buffers)
and [WWDC 2022 presentation](https://developer.apple.com/videos/play/wwdc2022/10101/).
It is broader than the `GPUFamilyMetal3` device family. The game uses GPU
addresses in generated shaders, so the application checks this capability before
creating game resources.

`tests/test_plume_metal.py` verifies clean forward/reverse application and compiles
the actual patched push-constant setters in a portable shell. Sanitizer runs test
non-aligned ranges and partial updates. These are Linux tests, not a Metal build.
Apple builds must validate Objective-C ownership, device capabilities, streaming
descriptors and command execution with Metal API validation enabled.

Several later upstream changes were deliberately not included:

- `dea63d3` changes the public swapchain API and presentation behavior. Porting it
  requires checking the application's frame and drawable lifetime assumptions.
- `6641a15`/`561428b` replace argument-encoder writes with direct Tier 2 layouts.
  The follow-up fixes the fallback path. Profile this separately on Apple devices
  after verifying OS/tier combinations and residency behavior.
- `041e9a4` changes encoder dirty-state handling, and `5a9db05` globally disables
  automatic hazard tracking. They need a synchronization audit and GPU validation
  against Xenos workloads before being adopted together or independently.
- `4f556be` moves residency tracking to device scope. Keep the existing tracking
  until Metal profiling establishes the benefits and validates retirement.
- Shader-helper/precompiled-internal-shader commits `7001205`, `42a09ff` and
  `5e77265` change dependency build requirements. The existing inline helper
  shaders are preserved; the game's own Metal shaders are built for the selected
  Apple target by the application build system.

Other upstream Vulkan/D3D12/build/example changes were inspected but are not
required for these iOS fixes. No blanket upstream merge or unpublished submodule
revision is used.
