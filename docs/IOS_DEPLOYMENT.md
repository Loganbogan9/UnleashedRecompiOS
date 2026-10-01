# iOS deployment and startup investigation

The reported IPA/SideStore failure remains **unproven and not device-validated**.
No application crash report, jetsam report, or device console capture was supplied
in the repository. These changes remove concrete hazards and make the next test
informative; they do not establish that SideStore caused the failure.

## What the source establishes

- CPU execution is ahead-of-time recompilation. XenonRecomp generates native C++
  and the iOS executable links that code statically. The 4 GiB guest address space
  uses anonymous **read/write** memory with a guard page, not executable memory.
  No game-runtime `MAP_JIT`, `PROT_EXEC`, or dynamic native-code generation was
  found. Metal shader compilation is separate from CPU JIT privileges.
- The current `main.cpp` continues directly after the installer and retains the
  graphics device. There is no installer-to-game process restart. The README's
  old Metal “soft restart” theory does not describe this startup path.
- `App::Restart` was a different problem: settings changes and the DLC menu used
  `fork`/`exec` and exited without checking whether a replacement process started.
  Normal iOS apps cannot use this as a supported relaunch mechanism. iOS now
  records a small, one-shot installer action, informs the player to relaunch from
  the home screen, and never attempts to spawn another process. Desktop restart
  behavior is unchanged.
- The picker used `alloc`, `new`, `copy` and Objective-C objects passed through
  `__block` variables, but its source did not enable ARC. This could leak picker
  objects or allow autoreleased errors/results to expire before the worker read
  them. The source now requires ARC, the build enables it for this file, and
  selecting an already-tracked URL does not acquire a second security scope.
  Apple's [security-scope documentation](https://developer.apple.com/documentation/foundation/nsurl/startaccessingsecurityscopedresource())
  requires successful acquisitions to be balanced to avoid leaking kernel
  resources.
- The app requests no custom iOS JIT or memory-limit entitlement in source.
  The final embedded profile and signature are produced by the installation or
  export workflow; inspect those artifacts rather than assuming source settings
  describe the final IPA. Dependencies are configured as static libraries; the
  macOS MoltenVK bundle-copy path does not apply to the iOS Metal target.
- User/game/save/config data live under the app's writable user directory,
  normally `Documents/UnleashedRecomp`. iOS ignores `portable.txt`, because signed
  bundles and their parent directories cannot serve as writable installation
  roots. An absent/empty `HOME` now uses SDL's native preference-directory fallback
  rather than the mobile account's non-container home. Logs use the same resolved
  user directory as configuration and game data.

## Why Xcode and home-screen launches can differ

Apple's [release-build testing guide](https://developer.apple.com/documentation/xcode/testing-a-release-build)
states that Xcode's debugger disables watchdog terminations and prevents normal
app suspension. A startup or loading wait that starves UIKit can therefore appear
to work under Xcode and fail when launched without the debugger. The pipeline
loading path previously pumped SDL once and then waited indefinitely for worker
completion. The iOS loading wait now services UIKit periodically through SDL's
event pump, with progress breadcrumbs; this is a source-supported watchdog
candidate, not a confirmed explanation for the reported IPA failure.

Debug/Release optimization, Metal validation, shader caches, app-container paths,
bundle identifiers, signing profiles, memory pressure and background transitions
also differ between workflows. Startup logs record debugger status, SDK/bundle
identity, page size, selected Metal environment variables, elapsed time and
memory at important boundaries. They do not dump credentials or provisioning
profile contents.

The 4 GiB guest reservation is virtual address space, not 4 GiB of immediately
resident memory. Read the footprint measurements and actual allocation failures
before concluding that a larger-memory entitlement is necessary. The logged
`available` value is the current **process memory budget**, not free device RAM,
and is advisory; see Apple's
[API documentation](https://developer.apple.com/documentation/os/os_proc_available_memory).

## Logs and interpretation

Retrieve `unleashedrecomp.log` and `unleashedrecomp.log.previous` from the app's
`UnleashedRecomp` folder through Files/Finder file sharing. Newly written log
segments are capped at 4 MiB, retaining one previous segment. A large log created
by an older version is preserved once during migration. Each completed message
is flushed, and each new launch has a session separator and debugger metadata.
Diagnostics run at startup boundaries rather than every frame.

The last `Startup [...]` breadcrumb brackets configuration, host startup,
installer cleanup, mods, persistent storage, guest-heap initialization, module
loading and entry into guest execution. Corrupted/truncated XEX headers and
image extents now produce an `Invalid module` message before guest memory is
modified. Memory reservation, allocation and thread errors should be correlated
with these boundaries.

| Observation | Next evidence to collect |
| --- | --- |
| No new log session | Check bundle/signature/dyld errors and pre-main failures in device console; also verify the resolved Documents container and write permission. |
| Loading progress stalls or log stops during startup | Obtain the main-thread backtrace, pending compiler count and elapsed time. Check the crash report for watchdog `0x8badf00d`. |
| Footprint rises and remaining process budget becomes small | Obtain the device's jetsam report and compare repeated stage load/unload cycles. Virtual reservation size alone does not establish memory pressure. |
| Failure occurs only after backgrounding | Test suspension and return without the debugger; record lifecycle/Metal errors and drawable availability. |
| Xcode install works detached but SideStore install fails | Compare the *same* archived executable, bundle resources, embedded profile, identifiers and entitlements after re-signing. |
| Source selection fails after repeated pick/cancel operations | Check picker error/count/scope-release messages; test local Files and a downloaded document-provider file independently. |

Watchdog signatures and jetsam reports are documented by Apple in
[Addressing watchdog terminations](https://developer.apple.com/documentation/xcode/addressing-watchdog-terminations)
and [Identifying high-memory use](https://developer.apple.com/documentation/xcode/identifying-high-memory-use-with-jetsam-event-reports).
A vanished process and a live, blocked process are different failures; preserve
the last timestamp, process state, and device report rather than treating both as
“logging died.”

## Next device test

1. Build one optimized device archive, retain its dSYM, and verify the app/IPA
   with `tools/check_ios_package.py`. On macOS also inspect final signatures with
   `codesign -d --entitlements :-` and decode `embedded.mobileprovision` with
   `security cms -D -i`. Match the application identifier and team with the
   provisioning profile. The Python checker does not verify a cryptographic
   signature.
2. Install through Xcode and test once with the debugger attached. Save both log
   files. Then disconnect the development machine, terminate the app, and launch
   **that same installed build from the home screen**. Repeat with Metal
   validation/HUD off and the same game data. This isolates debugger effects
   before changing the signing/install method.
3. Export/package that archive and install through SideStore. Verify its final
   bundle identifier and container; re-signing can create a different app with
   a different data directory. Import the same game/update/DLC data. Compare the
   logged startup boundaries, memory and shader-loading progress with step 2.
4. Complete a fresh installation, enter gameplay directly, then cold-launch an
   already-installed game. Exercise a loading-intensive stage and compare visual
   output with the prior build. Record watchdog or jetsam reports if either fails.
5. Pick the same local source repeatedly, cancel a picker, then complete an
   installation. Repeat with a fully downloaded document-provider source. Check
   that scopes are released at installer shutdown and the app remains responsive.
6. Change an option that requires restart, dismiss the relaunch message, and reopen
   from the home screen. Repeat from the DLC menu: the next launch must open the
   DLC installer once and subsequent launches must start normally.
7. Background/foreground during installer, pipeline compilation and gameplay.
   Run repeated gameplay/menu transitions without the debugger and record current
   and peak footprint, frame-time statistics and Metal validation errors.

## Linux validation and limits

`xex_load_tests` covers every truncation of valid synthetic compressed and
uncompressed XEX images, unaligned byte spans, malformed counts/offsets, overflow,
resource bounds, unsupported encryption/compression, underfilled blocks and a
byte-mutation sweep. `bounded_log_tests` verifies flushes, rotation, append across
launches, oversized messages and open failures. `launch_request_tests` verifies
atomic replacement, one-shot consumption, cancellation, malformed/oversized
markers and unwritable directory failure. These run on Linux with strict compiler
warnings and AddressSanitizer/UndefinedBehaviorSanitizer.

The UIKit picker, ARC code path, Mach memory queries, debugger query, Apple bundle
metadata, actual Metal compilation, IPA signing and SideStore installation require
macOS/iOS validation. No on-device reliability, visual accuracy, GPU performance
or frame-pacing result is claimed from Linux tests.
