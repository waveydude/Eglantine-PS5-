#!/usr/bin/env python3
"""Host test for tools/gen-translation-id.py."""
import importlib.util
import pathlib
import subprocess
import sys
import tempfile

here = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("gen", here / "gen-translation-id.py")
gen = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gen)

with tempfile.TemporaryDirectory() as tmp:
    root = pathlib.Path(tmp) / "source"
    src = root / "src"
    (src / "shader_recompiler/backend").mkdir(parents=True)
    (src / "video_core/renderer_vulkan").mkdir(parents=True)
    (src / "shader_recompiler/backend/emit.cpp").write_text("a")
    (src / "shader_recompiler/pass.cpp").write_text("b")
    for name in ("shader_environment.cpp", "shader_environment.h"):
        (src / "video_core" / name).write_text(name)
    for name in ("vk_pipeline_prep.cpp", "vk_pipeline_prep.h"):
        (src / "video_core/renderer_vulkan" / name).write_text(name)
    (src / "video_core/texture_cache.h").write_text("unrelated")

    first = gen.translation_id(root)
    assert len(first) == 16 and first == gen.translation_id(root), "same tree, same id"

    # A file outside the translation sources does not change it: a build that only touches the
    # texture cache keeps the translations.
    (src / "video_core/texture_cache.h").write_text("changed")
    assert gen.translation_id(root) == first

    # Any change to a translation source does: content, a new file, a rename, a move of bytes
    # from one file to the next.
    (src / "shader_recompiler/pass.cpp").write_text("c")
    changed = gen.translation_id(root)
    assert changed != first
    (src / "shader_recompiler/pass.cpp").write_text("b")
    assert gen.translation_id(root) == first
    (src / "shader_recompiler/new.cpp").write_text("")
    assert gen.translation_id(root) != first
    (src / "shader_recompiler/new.cpp").unlink()
    (src / "shader_recompiler/pass.cpp").rename(src / "shader_recompiler/pass2.cpp")
    assert gen.translation_id(root) != first
    (src / "shader_recompiler/pass2.cpp").rename(src / "shader_recompiler/pass.cpp")
    (src / "shader_recompiler/backend/emit.cpp").write_text("")
    (src / "shader_recompiler/pass.cpp").write_text("ab")
    assert gen.translation_id(root) != first
    (src / "shader_recompiler/backend/emit.cpp").write_text("a")
    (src / "shader_recompiler/pass.cpp").write_text("b")
    (src / "video_core/shader_environment.h").write_text("x")
    assert gen.translation_id(root) != first
    (src / "video_core/shader_environment.h").write_text("shader_environment.h")

    # The header: written once, left alone (same mtime) when the id is the same.
    header = pathlib.Path(tmp) / "translation_id.h"
    run = [sys.executable, "-B", str(here / "gen-translation-id.py"), str(root), str(header)]
    subprocess.run(run, check=True, capture_output=True)
    assert f'#define EDEN_TRANSLATION_ID "{first}"' in header.read_text()
    stamp = header.stat().st_mtime_ns
    subprocess.run(run, check=True, capture_output=True)
    assert header.stat().st_mtime_ns == stamp

    # A missing source is an error, not a quietly smaller hash.
    (src / "video_core/renderer_vulkan/vk_pipeline_prep.h").unlink()
    assert subprocess.run(run, capture_output=True).returncode != 0

print("translation id: stable, ignores other sources, follows every translation source PASS")
