# Building

## 1. Clone the Repository

Clone **UnleashedRecomp** with submodules using [Git](https://git-scm.com/).
```
git clone --recurse-submodules https://github.com/Loganbogan9/UnleashedRecompiOS.git
```

### Windows
If you skipped the `--recurse-submodules` argument during cloning, you can run `update_submodules.bat` to ensure the submodules are pulled.

## 2. Add the Required Game Files

Copy the following files from the game and place them inside `./UnleashedRecompLib/private/`:
- `default.xex`
- `default.xexp`
- `shader.ar`

These files are located in the game's root directory, apart from `default.xexp`, which must be obtained via the title update package.

> [!TIP]
> It is recommended that you install the game using [an existing Unleashed Recompiled release](https://github.com/hedge-dev/UnleashedRecomp/releases/latest) to acquire these files, otherwise you'll need to rely on third-party tools to extract them.
>
> Using the Unleashed Recompiled installation wizard will also ensure that these files are compatible with each other so that they can be used with the build environment.
>
> When sourcing these files from an Unleashed Recompiled installation, they will be stored under `game` and `update` subdirectories.

## 3. Install Dependencies

### Windows
You will need to install [Visual Studio 2022](https://visualstudio.microsoft.com/downloads/).

In the installer, you must select the following **Workloads** and **Individual components** for installation:
- Desktop development with C++
- C++ Clang Compiler for Windows
- C++ CMake tools for Windows

### Linux
The following command will install the required dependencies on a distro that uses `apt` (such as Debian-based distros).
```bash
sudo apt install autoconf automake libtool pkg-config curl cmake ninja-build clang clang-tools libgtk-3-dev libpulse-dev
```
The following command will install the required dependencies on a distro that uses `pacman` (such as Arch-based distros).
```bash
sudo pacman -S base-devel ninja lld clang gtk3
```
You can also find the equivalent packages for your preferred distro.

> [!NOTE]
> This list may not be comprehensive for your particular distro and you may be required to install additional packages, should an error occur during configuration.

### macOS / iOS
For iOS, install full Xcode 16.3+ with the iOS SDK and Metal compiler tools. Select the intended installation with `xcode-select` or `DEVELOPER_DIR`. The Command Line Tools alone do not provide the complete iOS build environment.

The following commands will install additional required dependencies, depending on which package manager you use.

If you use Homebrew:
```bash
brew install cmake ninja pkg-config
```

If you use MacPorts:
```bash
sudo port install cmake ninja pkg-config
```

## 4. Build the Project

### Windows
1. Open the repository directory in Visual Studio and wait for CMake generation to complete. If you don't plan to debug, switch to the `Release` configuration.

> [!TIP]
> If you need a Release-performant build and want to iterate on development without debugging, **it is highly recommended** that you use the `RelWithDebInfo` configuration for faster compile times.

2. Under **Solution Explorer**, right-click and choose **Switch to CMake Targets View**.
3. Right-click the **UnleashedRecomp** project and choose **Set as Startup Item**, then choose **Add Debug Configuration**.
4. Add a `currentDir` property to the first element under `configurations` in the generated JSON and set its value to the path to your game directory (where root is the directory containing `dlc`, `game`, `update`, etc).
5. Start **UnleashedRecomp**. The initial compilation may take a while to complete due to code and shader recompilation.

### Linux
1. Configure the project using CMake by navigating to the repository and running the following command.
```bash
cmake . --preset linux-release
```

> [!NOTE]
> The available presets are `linux-debug`, `linux-relwithdebinfo` and `linux-release`.

2. Build the project using the selected configuration.
```bash
cmake --build ./out/build/linux-release --target UnleashedRecomp
```

3. Navigate to the directory that was specified as the output in the previous step and run the game.
```bash
./UnleashedRecomp
```

### macOS
1. Configure the project using CMake by navigating to the repository and running the following command.
```bash
cmake . --preset macos-release
```

> [!NOTE]
> The available presets are `macos-debug`, `macos-relwithdebinfo` and `macos-release`.

2. Build the project using the selected configuration.
```bash
cmake --build ./out/build/macos-release --target UnleashedRecomp
```

3. Navigate to the directory that was specified as the output in the previous step and run the game.
```bash
open -a UnleashedRecomp.app
```

### iOS with Xcode (experimental)

The app requires iOS 16.0 or later and a GPU supporting Tier 2 argument buffers. Use the same Release build for Xcode and sideloading comparisons. The renderer uses ahead-of-time shaders and CPU recompilation; no runtime JIT entitlement is requested.

1. Prepare resources, PowerPC sources, and **iPhoneOS** game shaders using tools running natively on the Mac. Provide the game files described above first.

```bash
cmake --preset macos-release -DUNLEASHED_RECOMP_SHADER_TARGET_SDK=iphoneos
cmake --build out/build/macos-release --target UnleashedRecompArtifacts
```

`UnleashedRecompArtifacts` generates inputs without linking the macOS application. An old cache without SDK provenance, or one compiled for macOS, is rejected by the iOS configuration. The handwritten Metal libraries are generated separately in each build directory and embedded from their newly compiled bytes.

Game shader/PPC/resource outputs still live in the source tree. Do not generate macOS and iOS game caches concurrently in the same checkout. To build a macOS app later, switch the host option back to `macosx` and regenerate; a configured iOS build rechecks provenance if that shared cache changes. For simultaneous development, use separate checkouts with their own generated game inputs.

2. Configure the Xcode project with your own signing identity and bundle identifier.

```bash
cmake --preset ios-xcode-release \
    -DUNLEASHED_RECOMP_IOS_DEVELOPMENT_TEAM=YOURTEAMID \
    -DUNLEASHED_RECOMP_IOS_BUNDLE_ID=com.yourname.unleashedrecomp
cmake --build --preset ios-xcode-release
open out/build/ios-xcode-release/UnleashedRecomp-ALL.xcodeproj
```

The `ios-xcode-debug`, `ios-xcode-relwithdebinfo`, and `ios-xcode-release` build presets explicitly select their matching Xcode configuration. Release uses `-O2 -g -DNDEBUG`; symbols remain available while assertions and debug validation follow the selected configuration. The iOS document picker is compiled with ARC.

3. Select the device and `UnleashedRecomp` target in Xcode. Check Signing & Capabilities. For distribution, archive the **Release** configuration and export/sign the resulting app through your intended method. Retain the archive and dSYM for crash symbolication.

4. Check the app and exported IPA before installation:

```bash
python3 tools/check_ios_package.py /path/to/UnleashedRecomp.app
python3 tools/check_ios_package.py /path/to/UnleashedRecomp.ipa
```

The checker validates bundle metadata, executable permissions, device ARM64 Mach-O platform, embedded library references and signature-command extents. It does not verify cryptographic signatures or provisioning. On a Mac, inspect the app's signed entitlements and `embedded.mobileprovision`, and repeat for the final re-signed SideStore app when available. Compare application identifier, Team ID, `get-task-allow`, `com.apple.developer.kernel.extended-virtual-addressing`, `com.apple.developer.kernel.increased-memory-limit`, any development-only memory capability, and any embedded frameworks. The project does not add a privileged JIT or increased-memory entitlement.

5. Launch from the home screen without a debugger as well as through Xcode. Follow [the controlled deployment matrix](IOS_DEPLOYMENT.md) and retain `unleashedrecomp.log`, its `.previous` file, and the matching device crash/watchdog/jetsam report. Installer and DLC actions that request restart now persist a one-shot request and ask for a manual relaunch on iOS.

The Ninja iOS presets remain available for cross-compilation, but signing/export and installation should be verified through the Xcode archive workflow above. Native iOS compilation and packaging cannot be validated on Linux.

## Portable validation without game inputs or Apple SDKs

This mode bypasses vcpkg and the game generators. It requires CMake, Python, a C/C++20 compiler, and the initialized submodules. On Linux, install the X11 development headers (`libx11-dev` on Ubuntu): the portable Metal probes include plume's shared interface header, which declares its Linux window types using Xlib. The AIR compiler command tests use the generator's pinned fmt submodule and mock `xcrun`, not the Metal compiler.

```bash
cmake -S . -B out/tests -G Ninja \
    -DUNLEASHED_RECOMP_PORTABLE_TESTS_ONLY=ON \
    -DUNLEASHED_RECOMP_TEST_SANITIZERS=ON \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_BUILD_TYPE=Debug
cmake --build out/tests
ctest --test-dir out/tests --output-on-failure
```

The tests cover production portable runtime/installer code and bounded shader/texture utilities. Simulated SDK commands, packaging fixtures and Metal source-layout checks are identified separately in [the engineering audit](ENGINEERING_AUDIT.md). They do not establish MSL compatibility, iOS linking, signing correctness or device performance.
