from pathlib import Path

Import("env")


def patch_animatedgif_max_width(*_args, **_kwargs):
    project_dir = Path(env.subst("$PROJECT_DIR"))
    header = (
        project_dir
        / ".pio"
        / "libdeps"
        / env.subst("$PIOENV")
        / "AnimatedGIF"
        / "src"
        / "AnimatedGIF.h"
    )
    if not header.exists():
        print(f"[patch_animatedgif] header not found yet: {header}")
        return

    text = header.read_text()
    old = """#ifdef __LINUX__
#define MAX_WIDTH 2048
#else
#define MAX_WIDTH 480
#endif // __LINUX__
"""
    new = """#ifndef MAX_WIDTH
#ifdef __LINUX__
#define MAX_WIDTH 2048
#else
#define MAX_WIDTH 1024
#endif // __LINUX__
#endif // MAX_WIDTH
"""
    if old not in text:
        if "#ifndef MAX_WIDTH" in text and "MAX_WIDTH 1024" in text:
            return
        print("[patch_animatedgif] header already patched or layout changed")
        return

    header.write_text(text.replace(old, new))
    print("[patch_animatedgif] patched MAX_WIDTH guard (MCU default 1024)")


patch_animatedgif_max_width()
env.AddPreAction("buildprog", patch_animatedgif_max_width)
