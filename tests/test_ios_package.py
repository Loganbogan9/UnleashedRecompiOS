import importlib.util
import pathlib
import plistlib
import struct
import sys
import tempfile
import unittest
import zipfile

spec = importlib.util.spec_from_file_location("package_check", pathlib.Path(sys.argv[1]) / "tools/check_ios_package.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def image(platform=2, extra=b"", count=0, filetype=2):
    commands = struct.pack("<6I", 0x32, 24, platform, 15 << 16, 0, 0) + extra
    return struct.pack("<8I", 0xFEEDFACF, 0x0100000C, 0, filetype, count + 1, len(commands), 0, 0) + commands


class PackageTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temporary.name)
        self.app = self.root / "Unleashed.app"
        self.app.mkdir()
        self.info = {"CFBundleExecutable": "Unleashed", "CFBundleIdentifier": "test.unleashed",
            "CFBundlePackageType": "APPL", "CFBundleVersion": "1.0.3", "CFBundleShortVersionString": "1.0.3",
            "CFBundleSupportedPlatforms": ["iPhoneOS"], "MinimumOSVersion": "15.0"}
        self.write_info()
        self.executable = self.app / "Unleashed"
        self.executable.write_bytes(image())
        self.executable.chmod(0o755)

    def tearDown(self):
        self.temporary.cleanup()

    def write_info(self):
        (self.app / "Info.plist").write_bytes(plistlib.dumps(self.info))

    def test_directory_and_ipa(self):
        result = module.check_package(self.app)
        self.assertEqual(result["images_checked"], 1)
        self.assertEqual(len(result["warnings"]), 1)  # Fixture intentionally unsigned.
        ipa = self.root / "game.ipa"
        with zipfile.ZipFile(ipa, "w") as archive:
            for child in self.app.iterdir():
                archive.write(child, "Payload/Unleashed.app/" + child.name)
        self.assertEqual(module.check_package(ipa), result)

    def test_versions_platform_and_permissions(self):
        for value in ("v1.0.3", "", "$(PRODUCT_VERSION)", 123, ["1.0.3"]):
            self.info["CFBundleVersion"] = value
            self.write_info()
            with self.assertRaises(module.PackageError):
                module.check_package(self.app)
        self.info["CFBundleVersion"] = "1.0.3"
        self.write_info()
        self.executable.chmod(0o644)
        with self.assertRaisesRegex(module.PackageError, "permissions"):
            module.check_package(self.app)
        self.executable.chmod(0o755)
        for platform in (1, 7):
            self.executable.write_bytes(image(platform))
            with self.assertRaisesRegex(module.PackageError, "platform"):
                module.check_package(self.app)

    def test_truncated_headers_and_load_commands(self):
        valid = image()
        for length in range(len(valid)):
            with self.assertRaises(module.PackageError):
                module.macho_metadata(valid[:length])
        for command, size in ((0x32, 8), (0xC, 8), (0x1D, 8), (0x8000001C, 8), (0xC, 0), (0xC, 999)):
            with self.assertRaises(module.PackageError):
                module.macho_metadata(image(extra=struct.pack("<II", command, size), count=1))

    def test_embedded_libraries(self):
        path = b"@rpath/Helper.framework/Helper\0"
        size = (24 + len(path) + 7) & ~7
        command = struct.pack("<6I", 0xC, size, 24, 0, 0, 0) + path
        command += bytes(size - len(command))
        rpath = b"@executable_path/Frameworks\0"
        size = (12 + len(rpath) + 7) & ~7
        rcommand = struct.pack("<3I", 0x8000001C, size, 12) + rpath
        rcommand += bytes(size - len(rcommand))
        self.executable.write_bytes(image(extra=command + rcommand, count=2))
        with self.assertRaisesRegex(module.PackageError, "unresolved"):
            module.check_package(self.app)
        directory = self.app / "Frameworks/Helper.framework"
        directory.mkdir(parents=True)
        (directory / "Helper").write_bytes(image(filetype=6))
        self.assertEqual(module.check_package(self.app)["images_checked"], 2)
        # An embedded framework may find its sibling through a relative loader
        # path, or through the main executable's loader-relative rpath.
        path = b"@loader_path/../Helper.framework/Helper\0"
        size = (24 + len(path) + 7) & ~7
        command = struct.pack("<6I", 0xC, size, 24, 0, 0, 0) + path
        command += bytes(size - len(command))
        other = self.app / "Frameworks/Other.framework"
        other.mkdir()
        (other / "Other").write_bytes(image(extra=command, count=1, filetype=6))
        self.assertEqual(module.check_package(self.app)["images_checked"], 3)
        main = self.executable.read_bytes().replace(b"@executable_path", b"@loader_path////")
        self.executable.write_bytes(main)
        self.assertEqual(module.check_package(self.app)["images_checked"], 3)

    def test_signature_extent_and_bad_archives(self):
        with self.assertRaisesRegex(module.PackageError, "signature range"):
            module.macho_metadata(image(extra=struct.pack("<4I", 0x1D, 16, 99999, 16), count=1))
        archive = self.root / "bad.ipa"
        with zipfile.ZipFile(archive, "w") as output:
            output.writestr("Payload/Game.app/../Info.plist", b"invalid")
        with self.assertRaisesRegex(module.PackageError, "unsafe"):
            module.check_package(archive)


unittest.main(argv=[sys.argv[0]])
