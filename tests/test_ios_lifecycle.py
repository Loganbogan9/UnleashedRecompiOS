"""Exercise production cooperative pause/frame timing and queued-audio control."""
import os
import pathlib
import subprocess
import sys
import tempfile


def block(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        if source[end] == "{":
            depth += 1
        elif source[end] == "}":
            depth -= 1
        end += 1
    return source[start:end]


root = pathlib.Path(sys.argv[1]).resolve()
source = (root / "UnleashedRecomp/apu/driver/sdl2_driver.cpp").read_text()
with tempfile.TemporaryDirectory(prefix="ios lifecycle ") as directory:
    work = pathlib.Path(directory)
    implementation = "static SDL_AudioDeviceID g_audioDevice{};\n"
    implementation += "static std::mutex g_audioDeviceMutex;\nstatic bool g_audioAppActive = true;\n"
    for signature in ("void XAudioSetAppActive(", "static void QueueAudioFrames("):
        implementation += block(source, signature) + "\n"
    (work / "ios_audio_lifecycle.h").write_text(implementation)
    subprocess.run([sys.argv[2], "-std=c++20", "-pthread", "-Wall", "-Wextra", "-Werror",
                    *sys.argv[3:], "-O2", "-DNDEBUG", "-I" + str(work),
                    "-I" + str(root / "UnleashedRecomp"),
                    str(root / "tests/portable/ios_lifecycle.cpp"), "-o", str(work / "probe")],
                   check=True)
    subprocess.run([str(work / "probe")], check=True, timeout=15,
                   env={**os.environ})
