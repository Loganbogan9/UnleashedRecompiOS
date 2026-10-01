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
| `plume-0005-metal-live-descriptors.patch` | Fork optimization: track live descriptor slots with a compact bitmap, preserving the exact declaration order and usage flags. Sparse sets scan bitmap words/live entries; sets with at least one-quarter occupancy retain the original sequential scan. Bookkeeping is constant time and uses 8 KiB for the 65,536-slot texture set. |

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

`tests/test_metal_descriptor_traversal.py` compiles the actual C++17 resource
bookkeeping and encoder binding methods against portable mocks. It compares
ordered declarations to the original full scan, including mixed-usage aliases,
slot replacement/retirement, partial bitmap words and both density paths. The
optional `--benchmark` compares CPU traversal and update bookkeeping only, with
no Metal API calls. Example on Linux x86-64 (Ryzen AI 9 HX 370, Clang `-O3`):

| Live slots / 65,536 | Full scan | Selected traversal |
| --- | --- | --- |
| 256 | 16.69 microseconds | 0.408 microseconds |
| 1,024 | 17.52 microseconds | 0.929 microseconds |
| 8,192 | 16.32 microseconds | 9.594 microseconds |
| 16,384 or more | Original sequential scan retained | Original sequential scan retained |

Update bookkeeping in that run increased from 0.85 to 1.16 nanoseconds per
operation; locking and Metal argument encoding are excluded. Values vary with
the host and benchmark run. Apple CPU, frame-time and GPU benefits are unmeasured,
and the occupancy threshold should be checked during device profiling.

One existing behavior needs a targeted Apple audit: fallback `useResource`
declarations are currently emitted at encoder end. Apple's
[API documentation](https://developer.apple.com/documentation/metal/mtlrendercommandencoder/useresource%28_%3Ausage%3Astages%3A%29?language=objc)
specifies calling the method before draw calls that access the resource. These
patches preserve existing timing; they do not establish that late declarations
are safe. Test iOS 16/17 (the fallback without residency sets) with API/GPU
validation and a frame capture. A timing fix should track encoder resource
changes before draws rather than redeclaring the entire bindless set per draw.

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

## XenosRecomp AIR host tools

Base: the fork's `squidbus/XenosRecomp` commit `4906992`, including its MSL
translator. Canonical `hedge-dev/XenosRecomp` has no missing commits relative to
this pin at review time. `xenos-air-target.patch` adds a selectable SDK/triple,
emits cache provenance before the generated arrays, inherits the selected host
environment for `xcrun`, handles partial/interrupted file writes and `waitpid`,
and rejects an empty linker result. It preserves the translator's math mode and
MSL generation semantics. The native host still needs Apple's Metal compiler.

`air_command_generation.cpp` compiles the actual patched compiler for macOS and
iPhoneOS targets on Linux with subprocess calls wrapped. It verifies selected
SDK/triple arguments, inherited `DEVELOPER_DIR`, complete source writes after
EINTR/partial writes and temporary-file cleanup. This checks host command logic;
it does not compile a Metal library. iOS rejects unidentified, macOS-targeted,
or too-new game caches and rechecks when the shared cache changes.
