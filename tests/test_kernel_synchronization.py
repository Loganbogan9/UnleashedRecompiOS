"""Exercise the production kernel waits with Release flags and spurious CAS failures.

Only the atomic types are substituted in the fault-injection build. The event
and critical-section functions are extracted from imports.cpp without rewriting
their control flow. The native build also tests actual host atomic wait/notify.
"""
import pathlib
import subprocess
import sys
import tempfile


def block(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        if source[end] == "{":
            depth += 1
        elif source[end] == "}":
            depth -= 1
        end += 1
    return source[start:end]


def run(*arguments, timeout=60):
    result = subprocess.run(arguments, capture_output=True, text=True, timeout=timeout)
    if result.returncode:
        sys.stderr.write(result.stdout + result.stderr)
        raise RuntimeError(f"Command failed ({result.returncode}): {arguments}")
    return result.stdout


root = pathlib.Path(sys.argv[1])
compiler = sys.argv[2]
source = (root / "UnleashedRecomp/kernel/imports.cpp").read_text()
production = block(source, "struct Event final") + ";\n"
for signature in (
    "uint32_t RtlInitializeCriticalSection(",
    "void RtlInitializeCriticalSectionAndSpinCount(",
    "void RtlEnterCriticalSection(",
    "void RtlLeaveCriticalSection(",
    "bool RtlTryEnterCriticalSection(",
):
    production += block(source, signature) + "\n"

with tempfile.TemporaryDirectory(prefix="kernel synchronization ") as directory:
    work = pathlib.Path(directory)
    for injected in (False, True):
        implementation = production
        if injected:
            implementation = implementation.replace("std::atomic_ref", "TestAtomicRef")
            implementation = implementation.replace("std::atomic<bool>", "TestAtomic<bool>")
        (work / "kernel_waits.h").write_text(implementation)
        run(compiler, "-std=c++20", "-pthread", "-Wall", "-Wextra", "-Werror",
            *sys.argv[3:], "-O2", "-DNDEBUG",
            "-I" + str(work), "-I" + str(root / "tools/XenonRecomp/XenonUtils"),
            str(root / "tests/portable/kernel_synchronization.cpp"), "-o", str(work / "probe"))
        for case in ("critical-section", "try-enter", "auto-reset-event", "contention"):
            print(run(str(work / "probe"), case, timeout=15), end="")
        print("Spurious CAS injection passed" if injected else "Native atomic wait/notify passed")
