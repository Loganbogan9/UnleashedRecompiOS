# iOS engineering audit — 2026-10-01

Work branch: `codex/ios-reliability-audit`, starting at fork commit `6d2303e`.
All changes are separated into reviewable commits. No upstream blanket merge,
submodule checkout edits, history rewrite, or push was performed.

## Scope and architecture

The repository, dependency pins, fork history, build scripts, platform source,
installer, kernel/runtime, rendering state, texture uploads, handwritten shaders
and shader generators were inspected. The major subsystems were reviewed in
parallel and consequential changes received a second source review.

| Subsystem | How it works and what was checked |
| --- | --- |
| CPU recompilation | `tools/XenonRecomp` reads the patched Xbox executable and `UnleashedRecompLib/config/SWA.toml`, generating C++ PowerPC functions and dispatch mappings. `cpu/guest_thread.*`, `kernel/function.h` and kernel imports adapt the guest ABI to native code and host services. This is AOT CPU execution. |
| Guest memory/kernel | A 4 GiB anonymous read/write address-space reservation, native guard page and o1heap-backed user allocations support guest addressing. Big-endian fields and guest pointers cross the ABI. Mapping/allocation failure, null conversion, file offsets, waits and thread startup were checked. |
| Shader recompilation | The fork's XenosRecomp produces MSL in addition to HLSL. Native Mac tools compile game shaders to AIR/metallib bytes inside `shader_cache.cpp`; the iOS cross-build consumes generated PPC and resource inputs. Handwritten shaders are compiled and embedded separately. |
| Rendering | `gpu/video.cpp` translates guest resource/state/draw operations, texture formats, vertex data and shared constants through plume. It maintains shader/pipeline/sampler/depth caches, upload allocators and two frame slots, with separate copy submissions and fences. Worker tasks compile pipelines; loading code waits on the main thread. |
| Metal | Pinned plume implements command queues/lists, encoder transitions, descriptors/argument buffers, GPU addresses, texture copies/barriers, depth/sampler/pipeline objects and drawable presentation. Ownership, aligned uploads, descriptor retirement and CPU resource enumeration were checked. |
| Installation/deployment | ISO, STFS and SVOD parsers feed the installer. `main.cpp` loads the installed XEX after installation without restarting the process. UIKit picking, sandbox directories, log persistence, restart requests, Info.plist, Xcode configurations and IPA structure were checked. |
| Other paths | SDL audio/HID, timing/presentation, mod/path lookup, cache trimming and platform/dependency build configuration were inspected. Large speculative audio, scheduler, render-pass and guest floating-point rewrites were avoided. |

This Linux checkout contains neither the user's game inputs nor complete generated
PPC/game shader outputs, and no supplied device crash, console or jetsam reports.
There is no Xcode, Apple SDK, Metal compiler/runtime or physical Apple GPU here.
Consequently this audit cannot establish native linking, MSL compatibility,
in-game visual equivalence, signing validity, SideStore reliability or FPS.

## Upstream comparison

Canonical application history was fetched from
[`hedge-dev/UnleashedRecomp`](https://github.com/hedge-dev/UnleashedRecomp).
At the starting commit the fork was 9 commits ahead and 5 behind upstream
`cf829a9`. The iOS/Metal changes were preserved.

| Upstream change | Decision |
| --- | --- |
| [`5e8695a`](https://github.com/hedge-dev/UnleashedRecomp/commit/5e8695a) Werehog wall rotation at high frame rates | Ported the original hook/configuration and normalization fix. It corrects frame-rate-dependent game behavior; validate wall exits at multiple frame rates on-device. |
| [`77f68d2`](https://github.com/hedge-dev/UnleashedRecomp/commit/77f68d2) controller rumble multiplication | Ported removal of inappropriate frame-rate multiplication. Static comparison completed; validate vibration on actual controllers. |
| [`6b95a6d`](https://github.com/hedge-dev/UnleashedRecomp/commit/6b95a6d) native Linux sound | Adapted PulseAudio dependency/build configuration, preserving this fork's Apple workflow. Full native Linux game/audio execution was unavailable without game inputs. |
| [`f3a9f0f`](https://github.com/hedge-dev/UnleashedRecomp/commit/f3a9f0f) Flatpak runtime 24.08 | Ported the runtime/SDK update. Configuration inspected; Flatpak packaging was not executed. |
| `cf829a9` credits username | Left out of this functional audit; no runtime/build benefit. |

Dependency histories were also fetched. XenonRecomp's only missing commit through
`ddd128b` changes its README; no missing runtime fix was found. The fork's
`squidbus/XenosRecomp` pin `4906992` is one MSL-support commit ahead of canonical
XenosRecomp, with no missing canonical commits. Its translator is retained.

The plume pin `898904e` was 23 commits behind upstream `d723793`. Selected fixes
for push-constant bounds, drawable barrier casts, autorelease/object ownership,
null-buffer teardown and Apple SDK/device capability guards were adapted in
build-tree overlays. Exact origins and deferred changes are listed in
[dependency patch provenance](../patches/dependencies/README.md).

The complete plume update was intentionally rejected: swapchain API/presentation
changes, direct argument-buffer encoding, device-wide residency and disabling
automatic resource hazard tracking require further synchronization and hardware
validation. Their effects on iOS memory, tile passes and frame pacing cannot be
established here. Neither existing CPU strict floating-point flags nor generated
shader math/precision policy was made more aggressive.

## Implemented changes and validation

“Portable tested” below means Linux compilation of production portable code or
actual extracted dependency functions, with explicitly limited host/PPC stubs.
It does not mean the complete game was built.

| Change | Evidence and validation |
| --- | --- |
| Mapping/heap/null ABI correctness | Check `MAP_FAILED` for both mappings; retain failed API/native error for persistent startup logging; clean up failed guard protection; validate allocation sizes/alignment; preserve realloc's original allocation on failure; translate guest/host null pointers consistently. Production memory/heap/ABI tests exercise constrained address space and failure paths under ASan/UBSan. The application startup logger call is statically reviewed. |
| XEX validation | Validate headers, optional/resource extents, compression blocks and complete source/destination spans before writing guest memory. Reject overlap with the actual native guard page, including 16 KiB pages. Portable tests cover truncation, overflow, unsupported formats and byte mutations. Actual installed game images still require testing. |
| Installer container bounds | Reject truncated or cyclic ISO directory trees, STFS table/block chains and SVOD extents; avoid unaligned big-endian metadata access. Production parser tests use synthetic images and both memory-mapped/stream fallback paths under sanitizers. |
| Guest file I/O | Correct high/low file-size words, report actual bytes written, handle seek failure, and compose 64-bit offsets without signed-overflow errors. Production-source portable tests include sparse large files, partial/error cases and read/write/seek behavior. |
| Native thread/wait stability | Propagate pthread initialization/start errors, initialize IDs, serialize repeat/concurrent joins, clear guest TLS on exit and cache stack settings without a race. Production tests cover failure injection and waits; the existing iOS 16 MiB stack policy is exercised on Linux, and the std::thread branch is compile-checked. Imports' failure-status plumbing is statically reviewed. |
| Semaphore polling | Retry CAS failures while permits remain instead of reporting false timeout. Actual semaphore logic passed 16-contender/256-round tests; the previous implementation fails the same test on Linux. Finite timeout semantics remain unchanged. |
| BC/DDS texture correctness | BC2/BC3 always use four-color interpolation; BC1 transparency remains intact. Preserve fallback sRGB and component mapping, decode bounded/aligned DDS headers, validate all normal-path array/mip payloads before GPU allocations and check RGBA upload footprints/dimension limits. Fixtures, randomized blocks and 1,000 valid upload layouts test bounds and equivalence with original valid-file placement arithmetic. Final Metal texture creation/visual output requires Apple testing. |
| Movie/motion-blur shaders | Movie sampler indices now start at host byte offset 192, instead of reading byte 64 in the 3D texture array. HLSL/MSL and host offsets are checked by compiling the actual host constant-buffer declaration. MSL motion blur uses explicit level(0) and both versions guard zero sample-count division. MSL compilation and visual comparison remain unperformed. |
| Metal object/upload lifetime | Aligned push-constant allocation fixes host out-of-bounds uploads; portable probes compile actual patched setter bodies. Scope autorelease pools and retain/release escaping names, command buffers, encoders, dispatch data and device arrays correctly. Full Metal compilation and ownership/memory-pressure validation are required. |
| GPU retirement/concurrency | Retire pipelines through the existing frame-fence slots instead of destroying already-recorded pipelines; invalidate cached pipeline selection. Clear retired texture descriptors and encoded nil handles, serialize shared argument encoder/resource enumeration and make residency dirty state atomic. Static cross-review confirms retirement occurs after the slot fence. Native Metal validation is required, especially streaming/slot reuse. |
| Sparse Metal descriptor traversal | Track live slots in an 8 KiB bitmap for a 65,536-slot set, preserving exact declaration order and O(1) updates. Keep the original scan at 25% occupancy or higher. Actual C++17 bookkeeping/encoder methods pass randomized replacement/retirement/alias equivalence tests under sanitizers. Host traversal timings are below; native Metal effects require profiling. |
| iOS loading responsiveness | Pump SDL/UIKit events every approximately 10 ms while waiting for pipelines; log pending counts, elapsed time and memory every 5 seconds. Desktop atomic waits remain. This addresses a plausible watchdog hazard; only detached device testing can establish the effect. |
| Restart/picker/sandbox/logging | Persist bounded one-shot manual-relaunch actions; stop attempting iOS fork/exec; require ARC for the picker and balance security scopes; ignore portable bundle paths and recover from missing HOME using the native preference directory. Bound logs with flushes/rotation and add startup/debugger/memory breadcrumbs. Marker/log helpers are portable tested; UIKit/Mach/sandbox integration is static only. |
| Apple build/distribution | Explicit device SDK/triples, fresh build-directory Metal embeds, shader/include dependencies, game-cache provenance revalidation, actionable missing pre-generated inputs, numeric bundle versions and explicit iPhoneOS metadata. Correct selected-configuration Debug defines and Release NDEBUG; raise presets to iOS 16 for gpuAddress and test Tier 2 capability. CMake/fake-tool/metadata probes pass on Linux; complete native archive/export remains required. |
| Development validation | Add a game-input-free CMake test mode and Linux sanitizer CI, pinned fmt for generator tests, and an app/IPA structural checker. The checker covers ARM64 device Mach-O, metadata, permissions, load-command/signature extents and embedded-library paths; it does not perform cryptographic/profile verification. CI is checked in but has not run on GitHub. |

## SideStore/IPA reliability

The reported failure is **not proven fixed**. Source establishes AOT execution
without executable guest memory or a CPU JIT entitlement, and no current
installer process/Metal-device restart. These findings rule out those specific
source assumptions; they do not rule out final signing/profile differences.

Concrete distribution problems found include stale/wrong-platform shader embeds,
macOS-targeted game caches, release assertions/debug-mode differences, unsupported
process restart attempts, ARC/security-scope lifetime issues and main-thread
pipeline waits. The build now rejects mismatched caches and checks package
structure; the runtime records precise startup/loading checkpoints and memory.

Apple documents that an Xcode debugger disables watchdog termination and normal
suspension in its [release testing guide](https://developer.apple.com/documentation/xcode/testing-a-release-build).
This makes main-thread starvation a testable explanation for a debugger-only
success. The source fix is conservative, but a device backtrace and watchdog
report are still needed to prove causality. Virtual reservation size alone is
not evidence of resident memory pressure or an increased-memory entitlement need.

Follow the [controlled deployment investigation](IOS_DEPLOYMENT.md). Compare the
same optimized executable first attached/detached in the same Xcode installation,
then after SideStore re-signing. Preserve profiles, entitlements, bundle/platform
checks, session logs and crash/watchdog/jetsam reports. A new bundle identifier
may have a different sandbox and installed data. Other signing methods belong in
the same comparison matrix; no source evidence singles out SideStore itself.

## Performance and frame pacing

The correctness/stability work takes priority over speculative speed changes.
No measured game FPS, GPU time, frame pacing or device memory improvement is
claimed. Expected structural benefits are fewer transient object leaks, bounded
log growth, retired descriptor resources becoming releasable, avoiding an empty
semaphore-wait command buffer and responsive pipeline-loading waits. Some fixes,
such as retaining pipelines through fences and locking descriptor updates, can
increase short-lived memory or CPU work while removing unsafe behavior.

A reproducible traversal-only microbenchmark on Linux x86-64, AMD Ryzen AI 9
HX 370, Clang 23.1.1 `-O3 -UNDEBUG`, 4,000 scans per occupancy, measured:

| Live slots / 65,536 | Previous full scan | Selected traversal | Host ratio |
| --- | --- | --- | --- |
| 256 | 16.55 µs | 0.356 µs | 46.5× |
| 1,024 | 17.16 µs | 0.928 µs | 18.5× |
| 8,192 | 15.79 µs | 9.325 µs | 1.69× |
| 16,384 | 18.46 µs | 18.28 µs | Original scan; variation |
| 32,768 | 25.87 µs | 25.91 µs | Original scan; variation |
| 65,536 | 48.40 µs | 47.94 µs | Original scan; variation |

The benchmark excludes Metal API calls and mutex cost. It compares traversal
loops with identical resource/type checksums; actual extracted encoder methods
are independently tested for exact ordered declaration equivalence. Update
bookkeeping increased from 0.81 to 1.12 ns per operation in this run (~0.31 ns),
excluding argument encoding/locking. The initial bitmap-only approach regressed
dense cases, so it was amended to retain the original dense scan. Device CPU
behavior and the threshold still require profiling. These are host loop timings,
not Metal, frame-time or FPS measurements.

Reproduce with:

```bash
python3 tests/test_metal_descriptor_traversal.py "$PWD" clang++ -O3 -UNDEBUG --benchmark
```

The two frame slots and copy-fence policy, drawable acquisition/presentation,
render-pass boundaries, load/store actions, precision and hardware hazard tracking
were preserved. For Apple profiling, compare render/compute/blit encoder counts,
submission counts, argument-buffer resource declarations, CPU/GPU frame times,
1% low/frame-time distribution, peak footprint and cold/warm pipeline load time.

## Remaining opportunities

- Validate argument-buffer mutation across in-flight frames: the CPU mutex removes
  races between encoding/enumeration but does not itself provide GPU lifetime or
  per-frame argument-buffer isolation. Audit encoder resource-declaration timing
  and residency before adopting direct layouts or disabling hazard tracking.
  In particular, fallback `useResource` declarations currently occur at encoder
  end, whereas Apple documents declarations before accessing draw calls; see
  the exact API reference and validation caveat in the dependency provenance.
  Existing timing is preserved and remains an unresolved accuracy/stability risk.
- Profile Xenos resolve/copy render-pass boundaries on tile GPUs. Consider load/
  store actions, memoryless attachments and fewer copies only after establishing
  attachment reuse and exact depth/MSAA semantics.
- Profile pipeline specialization/cache behavior and cap cache growth using real
  stage workloads. Changing shader math/precision without visual baselines is
  inappropriate. Recompile all modified MSL with the actual Apple compiler.
- Check native DDS array-layer and 3D texture limits in addition to the tested
  payload/footprint bounds. Profile unconditional render-target shader-write
  usage and compression eligibility before changing generic resolve access.
- Finish BC7 support and signed BC4/BC5 handling; verify complete mip chains,
  cube/array/volume fallback behavior and supported GPU format combinations.
  Existing base-mip fallback limitations were not solved by the decoder fix.
- Audit oversized allocations in fixed 16 MiB upload allocators and retain/trim
  behavior under memory pressure; changing buffer sizing needs workload tests.
- Investigate guest handle ownership for close-while-waiting/self-thread cases,
  finite/absolute timeout conversion and unsupported wait behavior. Verify the
  `XSetFilePointerEx` guest ABI width before changing its import signature.
- Audit SDL audio callback unregister/re-registration and shutdown ownership,
  mod/path-cache invalidation/bounds and pre-existing submodule mapped-file close
  lifetime. No demonstrated game-triggered issue justified a speculative rewrite.
- Collect detached background/foreground traces during installation, shader
  compilation and gameplay; distinguish watchdog, deadlock, dyld and jetsam.
- The handwritten shader outputs are platform-separated, but generated game
  caches/PPC/resources remain shared source-tree outputs. Separate checkouts are
  needed for concurrent Mac/iOS generation; a complete output-directory migration
  is additional architectural work.

## Apple-side validation checklist

1. Regenerate iPhoneOS game shaders and all inputs using the documented native
   host artifact target. Build Debug and Release Xcode configurations, compile
   every modified MSL file, archive Release, retain dSYM and check the final IPA.
2. Run the same installation attached and detached, then SideStore from the same
   archive. Compare startup/checkpoint logs, bundle ID/entitlements/profile,
   watchdog/jetsam reports and cold/warm pipeline-loading responsiveness.
3. Test repeated pick/cancel operations and security-scope release, fresh install,
   settings/DLC manual relaunch exactly once, missing/corrupt game files and
   background/foreground transitions.
4. Enable Metal validation for descriptor slot reuse/streaming and cache trimming
   after draws have been recorded. Repeat stage/menu cycles and record footprint,
   encoder/submission counts, CPU/GPU frame time and cold/warm pipeline timings.
5. Compare cutscenes/movie planes/samplers, reversed-endpoint BC2/BC3 textures,
   sRGB output, motion blur with zero samples, depth/MSAA resolves, Werehog wall
   exits at 30/60/high frame rates and controller vibration against prior builds.
6. Exercise save-file reads/writes/seeks and threaded loading on ARM64. Check
   malformed-container/XEX diagnostics instead of crashes and confirm no import
   failure-status regression with actual guest-generated code.

## Linux validation results

Environment: Linux x86-64; CMake 4.4.3, Ninja, Python 3.14.7, Clang 23.1.1
and GCC 16.2.1. Final results:

- **20/20 CTest cases passed** with Clang Debug, AddressSanitizer and
  UndefinedBehaviorSanitizer, with recovery disabled (4.02 seconds).
- **20/20 passed** in a separately configured optimized Clang Release build
  (2.77 seconds). Test assertions remain enabled with `-UNDEBUG`.
- **13/13 independent cases passed** using GCC Release (2.81 seconds): CMake
  embedding/dependencies/metadata/provenance, package fixtures, shader layout,
  BC/DDS helpers, actual C++17 plume methods, bounded logs/relaunch requests and
  real AIR host command code with subprocess wrappers.
- The attempted **full GCC build did not compile**: pinned XenonUtils `xbox.h`
  uses anonymous aggregates containing constructed `be<>` members accepted by
  the project's Clang toolchain. The affected runtime/XEX targets were verified
  with Clang instead; no GCC support claim is made for those targets.
- Production memory/heap, installer parsers, guest file I/O, pthread lifecycle
  and semaphore code compile with minimal host/PPC stubs. pthread creation,
  stack setup and join failures are injected; the iOS stack-policy variant is
  run on Linux, not on an iPhone. The std::thread branch also compile-checks.
- AIR command tests compile the actual patched compiler with bundled fmt
  11.0.2; wrappers simulate process/write interruptions and linker output.
  Fake CMake `xcrun` tests verify platform bytes and rebuilding after shader
  includes change. No actual AIR/MSL compilation occurs.
- Descriptor tests cover 12,000 randomized mutations, retired resource lifetime,
  mixed read/write aliases, partial words, sparse/dense thresholds and exact
  declaration order. Texture tests include 5,000 randomized block decodes,
  1,000 original-layout equivalence fixtures, truncations and footprint overflow.
- JSON presets, SWA TOML, plist/XML templates, portable workflow YAML and Python
  scripts parsed/compiled successfully. Source whitespace checks passed,
  excluding unified patch context lines; patches apply/reverse cleanly in tests.
  All original dependency submodule worktrees remain unchanged.

Reproduction commands are in [BUILDING.md](BUILDING.md). Required future testing
is the Apple checklist above. Portable CI is committed but not remotely run.

No test in this report invokes Xcode, an Apple SDK/Metal compiler, an Apple GPU,
code signing, SideStore or an actual IPA installation. Fake `xcrun` and synthetic
IPA fixtures test dependency/embedding and parser logic, not native acceptance.
