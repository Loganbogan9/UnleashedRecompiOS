#!/usr/bin/env python3
"""Check iOS app/IPA structure without an Apple SDK or extracting the archive.

This checks packaging and Mach-O metadata, not cryptographic signatures or
whether entitlements match the provisioning profile after re-signing.
"""
import argparse
import pathlib
import plistlib
import posixpath
import re
import struct
import zipfile


class PackageError(ValueError):
    pass


def macho_metadata(data):
    if len(data) < 32:
        raise PackageError("truncated Mach-O header")
    # iOS distribution targets a thin ARM64 image. Reject simulator/universal
    # desktop products rather than silently inspecting the wrong architecture.
    magic, cpu, _, filetype, count, size, _, _ = struct.unpack_from("<8I", data)
    if magic != 0xFEEDFACF or cpu != 0x0100000C:
        raise PackageError("expected a thin 64-bit ARM64 Mach-O (not a simulator/macOS product)")
    if size > len(data) - 32 or count > size // 8:
        raise PackageError("invalid Mach-O load-command extent")
    end = 32 + size
    offset = 32
    result = {"type": filetype, "platform": None, "rpaths": [], "libraries": [], "signature": False}
    for _ in range(count):
        if offset + 8 > end:
            raise PackageError("truncated Mach-O load command")
        command, length = struct.unpack_from("<II", data, offset)
        if length < 8 or length % 8 or length > end - offset:
            raise PackageError("invalid Mach-O load-command size")
        payload = data[offset:offset + length]
        if command == 0x32:  # LC_BUILD_VERSION
            if length < 24:
                raise PackageError("truncated LC_BUILD_VERSION")
            result["platform"] = struct.unpack_from("<I", payload, 8)[0]
        elif command == 0x25:  # LC_VERSION_MIN_IPHONEOS, older toolchains
            if length < 16:
                raise PackageError("truncated LC_VERSION_MIN_IPHONEOS")
            result["platform"] = 2
        elif command == 0x1D:  # LC_CODE_SIGNATURE, presence only
            if length < 16:
                raise PackageError("truncated LC_CODE_SIGNATURE")
            start, extent = struct.unpack_from("<II", payload, 8)
            if start > len(data) or extent > len(data) - start:
                raise PackageError("signature range lies outside the executable")
            result["signature"] = extent > 0
        elif command in (0xC, 0x80000018, 0x8000001F, 0x20, 0x80000023, 0x8000001C):
            minimum = 12 if command == 0x8000001C else 24
            if length < minimum:
                raise PackageError("truncated dylib/rpath command")
            start = struct.unpack_from("<I", payload, 8)[0]
            if not minimum <= start < length or b"\0" not in payload[start:]:
                raise PackageError("invalid dylib/rpath string")
            value = payload[start:].split(b"\0", 1)[0].decode("utf-8")
            result["rpaths" if command == 0x8000001C else "libraries"].append(value)
        offset += length
    if offset != end:
        raise PackageError("inconsistent Mach-O command count")
    if result["platform"] != 2:
        raise PackageError(f"Mach-O platform is {result['platform']}, expected iPhoneOS (2)")
    return result


def check_package(path):
    path = pathlib.Path(path)
    archive = None
    if path.is_dir():
        if path.suffix != ".app":
            raise PackageError("directory must be an .app bundle")
        read = lambda name: (path / name).read_bytes()
        exists = lambda name: (path / name).is_file()
        executable_mode = lambda name: (path / name).stat().st_mode
        names = [p.relative_to(path).as_posix() for p in path.rglob("*") if p.is_file()]
    else:
        archive = zipfile.ZipFile(path)
        names = archive.namelist()
        if len(names) != len(set(names)) or any(n.startswith("/") or ".." in pathlib.PurePosixPath(n).parts for n in names):
            archive.close()
            raise PackageError("duplicate or unsafe ZIP member paths")
        roots = {n.split("/")[1] for n in names if n.startswith("Payload/") and len(n.split("/")) > 2 and n.split("/")[1].endswith(".app")}
        if len(roots) != 1:
            archive.close()
            raise PackageError("IPA must contain exactly one Payload/*.app bundle")
        prefix = "Payload/" + roots.pop() + "/"
        read = lambda name: archive.read(prefix + name)
        exists = lambda name: prefix + name in archive.namelist()
        executable_mode = lambda name: archive.getinfo(prefix + name).external_attr >> 16
        names = [n[len(prefix):] for n in names if n.startswith(prefix)]
    try:
        info = plistlib.loads(read("Info.plist"))
        if not isinstance(info, dict):
            raise PackageError("Info.plist must be a dictionary")
        executable = info.get("CFBundleExecutable", "")
        if not isinstance(executable, str) or not executable or pathlib.PurePosixPath(executable).name != executable or not exists(executable):
            raise PackageError("CFBundleExecutable does not identify a bundled executable")
        if info.get("CFBundlePackageType") != "APPL":
            raise PackageError("CFBundlePackageType must be APPL")
        identifier = info.get("CFBundleIdentifier", "")
        if not isinstance(identifier, str) or not re.fullmatch(r"[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)+", identifier):
            raise PackageError("invalid or unresolved CFBundleIdentifier")
        for key in ("CFBundleVersion", "CFBundleShortVersionString"):
            value = info.get(key, "")
            if not isinstance(value, str) or not re.fullmatch(r"[0-9]+(?:\.[0-9]+){0,2}", value):
                raise PackageError(f"{key} must be a numeric dotted version")
        platforms = info.get("CFBundleSupportedPlatforms", [])
        if not isinstance(platforms, list) or "iPhoneOS" not in platforms:
            raise PackageError("CFBundleSupportedPlatforms must contain iPhoneOS")
        minimum_os = info.get("MinimumOSVersion", "")
        if not isinstance(minimum_os, str) or not re.fullmatch(r"[0-9]+(?:\.[0-9]+){0,2}", minimum_os):
            raise PackageError("missing/invalid MinimumOSVersion")
        if not executable_mode(executable) & 0o111:
            raise PackageError("main executable lacks execute permissions")
        main = macho_metadata(read(executable))
        if main["type"] != 2:  # MH_EXECUTE
            raise PackageError("main image must have MH_EXECUTE type")
        warnings = []
        checked = {executable: main}
        for name in names:
            if name.endswith(".dylib") or (".framework/" in name and name.rsplit("/", 1)[-1] == name.split(".framework/")[0].rsplit("/", 1)[-1]):
                checked[name] = macho_metadata(read(name))
        for name, meta in checked.items():
            if not meta["signature"]:
                warnings.append(f"{name}: no code-signature load command; re-sign before installing")
            for library in meta["libraries"]:
                if library.startswith(("/System/Library/", "/usr/lib/")):
                    continue
                def expand(value, loader):
                    for token, base in (("@executable_path", "."), ("@loader_path", str(pathlib.PurePosixPath(loader).parent))):
                        if value == token or value.startswith(token + "/"):
                            value = base + value[len(token):]
                            break
                    return posixpath.normpath(value)
                if library.startswith("@rpath/"):
                    rpaths = [(rpath, name) for rpath in meta["rpaths"]] + [(rpath, executable) for rpath in main["rpaths"]]
                    candidates = [expand(rpath.rstrip("/") + "/" + library[7:], loader) for rpath, loader in rpaths]
                else:
                    candidates = [expand(library, name)]
                if any(c.startswith(("/System/Library/", "/usr/lib/")) for c in candidates):
                    continue
                if not any(not c.startswith(("/", "@", "../")) and c != ".." and exists(c) for c in candidates):
                    raise PackageError(f"{name}: unresolved non-system dylib {library}")
        return {"bundle_id": identifier, "version": info["CFBundleVersion"], "minimum_os": info["MinimumOSVersion"],
                "images_checked": len(checked), "warnings": warnings}
    finally:
        if archive is not None:
            archive.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", help="path to an iPhoneOS .app or .ipa")
    args = parser.parse_args()
    try:
        result = check_package(args.package)
    except (PackageError, OSError, ValueError, KeyError, zipfile.BadZipFile) as error:
        parser.exit(1, f"Package check failed: {error}\n")
    print(f"Package structure passed: {result['bundle_id']} {result['version']}, iOS {result['minimum_os']}+, {result['images_checked']} Mach-O images")
    for warning in result["warnings"]:
        print("Warning: " + warning)
    print("Signatures, provisioning and final re-signed entitlements still require verification on a Mac/device.")


if __name__ == "__main__":
    main()
