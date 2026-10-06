# Unleashed Recompiled — First iOS Release

**Commit:** `e64b693`
**Build:** Beta 1 (iOS)

This is the first public release of **Unleashed Recompiled** for iOS — an unofficial port of *Sonic Unleashed* (Xbox 360) running natively on iPhone and iPad through static recompilation.

> **⚠️ PLEASE DO NOT report issues from this build to the upstream [hedge-dev/UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp) repo.** I don't want to be responsible for issue spam over there.

---

## What's New (iOS-specific)

This release builds on top of the upstream Unleashed Recompiled v1.0.3 codebase, building upon [squidbus's fork](https://github.com/squidbus/UnleashedRecomp/tree/macos-support) that introduced initial Metal graphics support for macOS. Many thanks to **squidbus** for his foundational work bringing the Metal backend to Unleashed Recompiled.

iOS-specific additions and changes include:

- **Metal graphics backend** – fully replaces D3D12/Vulkan with Metal, including Metal GPU caches, descriptor management, and texture upload handling (based on squidbus's macOS Metal port)
- **iOS build support** – CMake toolchain, bundle metadata, system framework linkage, and IPA packaging pipeline
- **ProMotion support** – automatic high refresh rate on supported devices (120 Hz displays)
- **Automatic touch controls** – on-screen touch input with automatic detection; no controller required
- **iOS document picker integration** – native file browser for selecting game dump files
- **Standalone Mach-O executable** – no JIT entitlement needed; all guest code is statically AOT-compiled
- **Persistent startup diagnostics** – records crash/startup errors across launches for easier troubleshooting
- **Guest memory safety** – bound checks, guard page protection, validation of installed XEX images and container extents
- **Sandbox-compliant file paths** – respects iOS container directories; no `portable.txt` needed
- **No process restart** – installer runs in-process; DLC changes tell the user to relaunch from the home screen (iOS limitation)

## What's New (upstream changes included)

- Various ultrawide and aspect ratio fixes
- Frame limiter implementation
- Mod loader support
- Achievement system with data verification
- High DPI / resolution scaling improvements
- Localization updates (Italian translation added)
- D-Pad input, World Map, Gaia Colossus, and bobsleigh fixes
- Many crash and rendering reliability improvements

## Minimum Requirements

- **Device:** iPhone 14 Pro / A16 Bionic or newer (older devices may work but are untested)
- **OS:** iOS 16.0+
- **Metal:** Tier 2 argument buffer support required
- **Storage:** ~10 GB with DLC, ~6 GB without
- **RAM:** 4 GB minimum, 6 GB recommended

## Known Issues

### ⚠️ DLC Installation — Odd Filesystem Error

When installing the DLC (Adventure Packs), **install all DLC except one** using the original installer inside the app. Then, once you're in-game, install the final DLC from the title screen's installer menu.

There's an odd filesystem interaction in this first release that can cause the installer to fail when processing all DLC in a single pass. Splitting it this way reliably works around the issue.

### ⚠️ Black Screen at Launch with Audio

Sometimes the app may launch with a black screen while audio still plays in the background. This appears to be a Metal pipeline initialization timing issue.

**Fix:** Simply close the app and relaunch. It should start normally on the next attempt.

### General Caveats

- This is a **Beta 1** release — you may encounter crashes or visual glitches
- Japanese version of the game is **not supported**
- Xbox 360 game files from a **legally owned copy** are required; this project does not include any game assets
- SideStore and sideloading workflows are unvalidated; Xcode release builds from source are the recommended installation method
- No JIT entitlement is needed (code is AOT-compiled), but standard iOS code signing applies
