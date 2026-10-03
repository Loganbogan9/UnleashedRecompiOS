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

Apple distinguishes [extended virtual addressing](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.developer.kernel.extended-virtual-addressing)
from an [increased resident-memory limit](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.developer.kernel.increased-memory-limit).
Compare both exact entitlement keys in the final Xcode and re-signed profiles.
Also check for an [Increased Debugging Memory Limit](https://developer.apple.com/help/glossary/increased-debugging-memory-limit/)
capability used for internal development/testing. None is configured by this
source change, and none is proven necessary for this app. A failed 4 GiB mapping
and a jetsam termination after rising footprint are distinct observations.

If a supported signing method cannot provide sufficient virtual address space,
the current generated `base + guestAddress` ABI still requires a contiguous guest
window. Avoiding that requirement would need generator/runtime changes to guest
address translation; it would not be solved by a JIT entitlement. First establish
an actual mapping failure and compare profiles rather than assuming that this
architectural work is necessary.

## Controller input after guest startup

The iOS startup path runs the guest entry on a worker thread and returns from
`SDL_main` so UIKit can keep servicing its main run loop. The controller backend
does not update just because that run loop is running: `SDL_PumpEvents` must poll
SDL's joystick drivers. The existing application-update hook only pumps on the
original main thread, so moving guest execution to a worker left no per-frame
controller updates. The desktop path in `origin/main` runs the guest entry on
the calling thread and does not have this mismatch.

`StartIOSEventLoop` now installs an SDL iOS animation callback before starting
the guest. It pumps and drains SDL events and updates the window on UIKit's main
thread. SDL disables its nested UIKit run-loop pump after `SDL_main` returns;
joystick polling and event watches continue to work. Application notifications
maintain input focus after SDL removes its own lifecycle observer at that
handoff. Focus is atomic, and guest state/capability/rumble calls share SDL's
joystick lock with controller events. Connecting an idle controller makes it
available immediately; disconnecting it clears both the active pointer and
cached inputs. iOS controller LED requests are applied after the main-thread
event pump, avoiding an inversion between SDL's event-watch and joystick locks.

The portable `sdl_controller_menu_input` test uses real SDL virtual controllers
to check Start/A, D-pad and stick updates, concurrent guest reads and rumble,
disconnect, and reconnect. On macOS it also runs the production iOS callback
against a simulated UIKit scheduler and application notifications. This checks
callback registration, polling, focus transitions, and registration failure
without requiring a device. Actual Bluetooth/MFi behavior still needs an iPhone
or iPad check:

1. Connect a controller before launch, then use Start/A to leave the title menu.
2. Repeat with a controller connected after the menu appears.
3. Disconnect while holding a button, reconnect, and verify no input stays held.
4. Background and resume the app, then verify menu navigation and rumble resume.

Startup logs include `iOS main-thread event loop started` and each successfully
opened controller's name. Initialization/open failures include SDL's error.

## Optimized startup stuck in archive loading

The supplied optimized-build backtrace shows the guest main thread polling an
archive request in `sub_82E0C550`, while the request worker and two audio workers
are sleeping inside `RtlEnterCriticalSection`. UIKit is servicing its run loop;
the render-command and shader-compiler workers are idle. The log reaches guest
device creation with about 2.6 GiB of remaining process memory budget. This
locates the stall in guest synchronization during loading.

The critical-section implementation previously used `compare_exchange_weak`
and immediately waited on the returned owner after failure. On ARM64, an
exclusive store can fail spuriously even when the lock is free. In that case,
the owner remains zero and `wait(0)` sleeps without a lock holder to notify it.
The auto-reset event had the same problem: a spurious failure while consuming a
signal could call `wait(true)` on an already signaled event. These acquisition
paths now use strong compare-and-swap; try-enter also avoids reporting false
contention. Compiler optimizations remain enabled.

`kernel_synchronization_release` compiles the production functions with
`-O2 -DNDEBUG`, tests recursive locks and contended wait/notify, and injects legal
spurious weak-CAS failures to reproduce both invalid sleep paths. The iOS ARM64
compiler output confirms that weak CAS uses a single exclusive-store attempt
and strong CAS retries it. The supplied dump does not include the observed lock
owners or wait values, so confirmation of this particular device hang still
requires rebuilding and cold-launching the optimized app. If it persists,
capture all thread backtraces and the `OwningThread`/`RecursionCount` fields of
each blocked critical section, using the current run's `cs` addresses.

## Pausing when inactive or in the background

The UIKit observers previously only changed input focus. The guest frame loop,
queued-audio producer and renderer could continue working after deactivation.
That permits GPU submissions during the transition to the background, where
iOS can reject them with `NotPermitted`.
[Apple's Metal background guidance](https://developer.apple.com/documentation/metal/preparing-your-metal-app-to-run-in-the-background)
requires disabling new commits and scheduling previously committed buffers on
every command queue before returning from the background notification.

The app now requests a cooperative pause on `UIApplicationWillResignActive`
and also handles `UIApplicationDidEnterBackground`. Guest updates, presentation,
new guest thread entries, audio callbacks, render batches and pipeline tasks
wait at their next boundary. Work already in progress and loading/file I/O can
finish; this avoids freezing threads while they hold locks needed elsewhere.
UIKit does not wait for guest workers, which may themselves need the main thread
to finish UI work.

`plume-0007-metal-lifecycle.patch` serializes every native command-buffer commit
with deactivation, including acquire, present, render, copy and semaphore-wait
buffers. It calls `waitUntilScheduled()` on the latest submitted buffer on each
queue. Queue reservations occur inside the submission gate, so an uncommitted
buffer waiting for reactivation cannot block scheduling of previous work.
Lifecycle notifications run synchronously on UIKit's posting thread.

Audio output pauses and its queued samples are cleared. An audio callback
already running when deactivation arrives drops its late output. Execution
resumes on `UIApplicationDidBecomeActive`, after enabling Metal submissions and
audio; merely entering the foreground while still inactive does not resume it.
Each frame-producing thread discards its first accumulated delta after resume
so simulation does not jump forward by the time spent in the background.

The log records `iOS pause requested; audio stopped and Metal submissions
drained.` and `iOS execution resumed.` `ios_execution_lifecycle_release` checks
worker wakeups, repeated transitions, resume timing and late audio callbacks.
The Metal presentation regression also checks blocking acquisition/presentation,
render/copy submission gating and scheduling on both queues. UIKit/controller
integration tests check notification ordering and inactive startup.

On a device, background the rebuilt Release app during play and while loading,
wait at least 30 seconds, then return. Repeat with screen locking and Control
Center. Verify audio stops, gameplay resumes without skipping the background
interval, video remains visible and the log contains the pause/resume pair
without Metal `NotPermitted` errors. Continue testing immediate force-quit and
relaunch separately: this lifecycle defect does not establish the cause of a
black screen in a newly launched process.

## Black screen while the game keeps running

A detached Release launch that still plays audio and responds to game input
needs presentation diagnostics as well as guest-thread backtraces. The reported
case happens frequently on an immediate relaunch after force-quitting; switching
apps or locking/unlocking does not recover video. That observation does not
establish a compiler miscompilation or identify a particular GPU error.

The Metal swapchain had a race: its GPU completion callback advanced the slot
index while the CPU acquisition path read that index twice, allowing the returned
index and updated drawable to disagree. Acquisition now advances the index on
the calling thread, using one slot throughout. Presentation releases the slot's
drawable reference after submission, while the GPU completion handler retains
the submitted drawable for the required lifetime. Completion handlers own shared
latency state so they can safely finish after the swapchain is destroyed.
[Apple's CAMetalLayer documentation](https://developer.apple.com/documentation/quartzcore/cametallayer)
requires prompt release of drawable references to keep the drawable pool usable.

An unavailable drawable no longer submits an acquire command buffer, modifies
the output index, or advances the slot. A subsequent frame retries acquisition.
Drawable metadata comes from the actual texture, covering layout changes while
the window-size cache catches up. The dependency change is applied through
`plume-0006-metal-presentation.patch` without editing the dependency checkout.

The persistent app log now records:

- `iOS drawable acquired` at the first successful acquisition and after recovery.
- `iOS drawable unavailable` on the first failure and at most once every five
  seconds until recovery, with size, focus and resize state.
- `Metal drawable displayed` for the first three presentation callbacks, with
  the drawable's presentation timestamp. A positive timestamp provides stronger
  evidence of display than the CPU's `Video::Present` submission messages.
- `Metal command buffer failed` for the first sixteen GPU errors per process,
  with the command label, error code and description. This includes render,
  acquire, present and semaphore-wait command buffers. Apple documents GPU
  restrictions and the `NotPermitted` error in
  [Preparing your Metal app to run in the background](https://developer.apple.com/documentation/metal/preparing-your-metal-app-to-run-in-the-background).

`metal_drawable_presentation_release` compiles the production swapchain methods
with `-O2 -DNDEBUG` against a controlled drawable pool and GPU callback scheduler.
It covers delayed completions, temporary acquisition failures, release and slot
rotation across many frames, callback lifetime and error reporting. These tests
also pass with AddressSanitizer and UBSan. They validate the source fixes; they
do not confirm the cause of the reported physical-device failure.

For device verification, install the rebuilt optimized app and repeatedly
force-quit/relaunch it without attaching LLDB. Compare immediate relaunches with
relaunches after locking the phone. If video still fails while input works, save
the current and previous app logs before another launch rotates them. The new
messages distinguish missing drawables, GPU command errors, and submission that
never produces a positive presentation timestamp.

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
with these boundaries. Initialization records now retain the original failed API
and native error code before cleanup can change it; a reservation/protection
failure is written to the persistent log immediately after logger setup as well
as at guest initialization.

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
