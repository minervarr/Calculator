# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A privacy-respecting, ad-free Android **graphing/scientific calculator** that bypasses the Android
UI toolkit entirely: the whole app is native C/C++17 rendered with **Vulkan**. The Android side is a
bare `android.app.NativeActivity` (no Kotlin/Java UI, no XML) that only hosts the `ANativeWindow` and
forwards touch events. Licensed **AGPLv3** (`LICENSE` at root; keep the short header on new files).

## Build / run

```bash
git submodule update --init --recursive      # REQUIRED first; canvas engine nests font engine nests FreeType
./gradlew :app:assembleDebug                  # builds arm64-v8a + x86_64 (ABI splits; no universal APK)
```

- Toolchain: NDK `29.0.14206865`, CMake `3.22.1+`, `minSdk 26 / compileSdk 37`. Native lib name is
  `calculator` (must match `AndroidManifest.xml` `android.app.lib_name`).
- **Shaders** are Slang, compiled to SPIR-V by `slangc` at build time via the engine's reusable
  `libs/vulkan_canvas_engine/cmake/VceShaders.cmake` (`vce_compile_slang`). The compiler is resolved
  by `find_program(slangc)` with a fallback to `VCE_SLANGC` (defaults to the Vulkan SDK
  `C:/VulkanSDK/1.4.341.1/Bin/slangc.exe`; override with `-DVCE_SLANGC=...`). Source lives in the
  **font engine** `…/vulkan_font_engine/app/src/main/shaders_src/`; outputs + the `.spv` are committed
  under `app/src/main/assets/shaders/`. After editing a `.slang`, recompile that one shader into
  `app/src/main/assets/shaders/` (the build also does it, but committed `.spv` are the source of truth).
- **No test framework.** `core/math` + `core/calc` are pure C++17 (no Vulkan/Android) and are the only
  unit-testable part; a host C++ compiler (MSVC/clang) is needed to build a desktop test — not assumed
  present here. Otherwise verification is **on a device** (`adb install -r app/build/outputs/apk/debug/app-x86_64-debug.apk`).

## Font atlas (offline)

Text is **not** drawn from the font at runtime — it's a pre-baked **MSDF atlas**
(`app/src/main/assets/fonts/font.msdf` metrics + `atlas.rgba`). Regenerate after charset/quality
changes. The baker is the **C++ host tool** living INSIDE the font engine submodule at
`libs/vulkan_canvas_engine/vulkan_font_engine/tools/atlas_gen/` — producer, reader
(`MsdfFont::load`) and the source OTFs (`tools/atlas_gen/fonts/`, GUST Font License) all ship
together so any app reusing the engine gets the whole pipeline.

```bash
# Host build (desktop, not the Android app): vendored FreeType + msdfgen, no vcpkg.
FE=libs/vulkan_canvas_engine/vulkan_font_engine
pwsh $FE/tools/atlas_gen/build.ps1    # → $FE/tools/atlas_gen/build/atlas_gen.exe
# 4 faces baked into one sheet: roman / bold / italic (text) + latinmodern-math (all math).
$FE/tools/atlas_gen/build/atlas_gen.exe \
    $FE/tools/atlas_gen/fonts/lmroman10-regular.otf $FE/tools/atlas_gen/fonts/lmroman10-bold.otf \
    $FE/tools/atlas_gen/fonts/lmroman10-italic.otf $FE/tools/atlas_gen/fonts/latinmodern-math.otf \
    app/src/main/assets/fonts/font.msdf app/src/main/assets/fonts/atlas.rgba
```

`atlas_gen` produces **true multi-channel MSDF** via **msdfgen** (vendored in the font engine at
`…/vulkan_font_engine/app/src/main/msdfgen`, next to FreeType; the same engine the app links for
the runtime dynamic-atlas fallback — `normalize`→`orientContours`→`edgeColoringSimple`→`generateMSDF`, row-flipped),
glyph outlines/metrics from **FreeType**, and the OpenType `MATH` table parsed by hand from raw bytes
(`math_table.{hh,cc}` — no HarfBuzz/ttf-parser). MSDF is scale-independent → a
compact atlas stays crisp at any size, so one optical size per style suffices — not the 8 in `lm/`.
It bakes **by glyph-id** across four **named styles** (`FontStyle` 0=Roman 1=Bold 2=Math 3=Italic;
glyph key = `(style<<24)|gid`) and cracks the math font's OpenType **`MATH`** table — exporting
MathConstants, per-glyph italic-correction, and the **stretchy vertical constructions** (variant lists
+ assembly recipes for radicals, `( ) [ ] { } |`, `∫ ∑ ∏`). Variables render in the math face's
**math-italic** (U+1D44E…); lowercase Greek (π, θ) is math-only (lmroman has no lowercase Greek).
The bespoke `font.msdf` **v2** binary format (magic `0x4644534D`, version word `2`) is read by
`MsdfFont::load`; the reader and the writer (`atlas_gen.cc`) must stay byte-for-byte in sync —
don't change the format without bumping the version and updating both. Density is set by `EM` (atlas texels/em, the sharpness lever) with `RANGE` pinned at `EM/10`
(the shader pxRange; mathlayout's `glyphPadEm` depends on `RANGE/EM == 0.1`); `AW` (sheet width) must
grow with `EM` so the packed sheet stays under 4096 in **both** dims (min-spec Vulkan
`maxImageDimension2D`; the engine sizes the GPU texture from the header, so no code change is needed).
Current default is `EM=100`, `AW=3072` (≈3072×3095, ~36 MiB) — up from the old `EM=80`/2048 bake; both
are overridable via the `ATLAS_EM` / `ATLAS_AW` env vars. Source OTFs live in the font engine at
`…/vulkan_font_engine/tools/atlas_gen/fonts/` (build sources, kept out of the APK — only the baked
atlas ships).

## Architecture

### Layering & the reuse rule (load-bearing)
- `libs/` holds three Git **submodules**, all the author's own repos:
  - `vulkan_canvas_engine` — the canvas + reusable host/widgets (camera-app heritage; only the
    camera-free parts are used here).
  - `…/vulkan_font_engine` (nested) — GPU text: FreeType (`Font`, Bézier) **and** the MSDF path
    (`MsdfFont`); vendors FreeType.
  - `archive_engine` — an audio-archival/zip/tagging library (NOT a database; currently unused; SQLite
    persistence is greenfield).
- **Reuse-first directive:** generic, app-agnostic pieces are committed **into the
  `vulkan_canvas_engine` submodule** so other apps reuse them — e.g. `CanvasHost`, `GestureRecognizer`,
  `Pager` (all in `…/vulkan_canvas_engine/app/src/main/cpp/`). Calculator-specific code stays in
  `app/` and `core/`. When you add something reusable, put it in the submodule working tree.

### Render pipeline (per frame)
`app/src/main/cpp/main.cc::android_main` runs a native_app_glue loop. It builds a `Canvas` (engine,
immediate-mode: `clear/rect/text/button/setClip`) into two buffers and calls
`CanvasHost::renderFrame(curves, quads)`:
- **`curves`** — 20-float SDF records (backgrounds, panels, rounded buttons). Rasterized by
  `OverlayRasterizer` (`overlay.cc`) through two compute shaders: `tiling` (per-curve → 16×16 tiles)
  then `coverage` (per-pixel SDF). Composited over a cleared background.
- **`quads`** — MSDF text vertices (8 floats/vert) from `Canvas::useMsdf(host.msdfFont(), &quads)`.
  Drawn on top by the MSDF pipeline inside `CanvasHost` (atlas texture + `msdf_vert/frag`, `median(rgb)`).
- Curve record types: `2`=filled rounded rect, `3`=SDF capsule/segment, `4/5/6`=Bézier **winding**
  glyph fills. The calculator emits **no winding curves** (text is MSDF), and `coverage.slang` gates
  its expensive per-pixel winding scan behind a `hasWinding` push constant — keep it off for this app.
- One frame in flight; render loop is **invalidate-on-dirty** but drives **continuous frames while
  `pager.animating()`** (FIFO present paces it to refresh rate).

### Calculator core (`core/`, pure C++17, no Vulkan)
- `core/math/`: `lexer` (implicit multiplication) → `parser` (recursive descent → `ast`) →
  `IEvaluator` / `NumericEvaluator`. Parser depends on `IEvaluator`, never the reverse, so a future
  symbolic/CAS evaluator drops in without touching the parser.
- `core/calc/Calc`: facade the UI feeds canonical tokens; owns the expression, `Ans`, DEG/RAD flag,
  result formatting (`%.12g` + scientific fallback), and ASCII→pretty display mapping (`* →×`, etc.).
- Built into the app via `CORE_DIR` in `app/src/main/CMakeLists.txt`.

### 2D math rendering (`app/src/main/cpp/mathbox.{hh,cc}` + `mathlayout.{hh,cc}`)
- **`mathbox`** is the single TeX-style layout core: a small box IR (HBox/Text/Glyph/Frac/
  Radical/Script/Delim/Placeholder/Caret) laid out with TeX's rules read from the font's
  OpenType MATH table — Display/Text/Script/ScriptScript styles, the TeXbook ch.18 inter-class
  spacing table, fraction shift/gap constants, stretchy radical constructions.
- **`mathlayout`** holds two thin builders (editor tree → IR, CAS AST → IR) + the entry points
  (`drawEditor`, `fitAst`/`drawAstAt`, `drawKeyIcon`). Per-surface differences are builder
  decisions (placeholders, ghost closers, precedence delimiters) or `mathbox::Style` toggles
  (caret, negative-red). Never draw math geometry outside mathbox — that's how input and
  results stay pixel-identical.

### App UI (`app/src/main/cpp/main.cc`)
Two horizontally-swipeable pages (Standard / Scientific) driven by `Pager` + `GestureRecognizer`,
**sharing one `Calc`**. Per-page key tables (`pageKeys`) are the single source of truth for both
drawing and hit-testing. Swipes are gated to a top strip (`kSwipeZoneFrac`); the rest is tap-only.
Display fits long values by shrinking then panel-clipping.

## Conventions / gotchas

- **Do not run `git commit` or `git push`** — the user does all git operations manually; leave
  finished work as uncommitted changes in the working tree.
- Reusable engine edits live in the **submodule** working trees (`libs/…`), pinned by the superproject
  — but still don't commit them.
- The math/graphing cores must stay free of Canvas/Vulkan/Android includes (decoupling is a hard
  requirement of this project).
- UTF-8 string literals (`× ÷ − √ π`) are used directly in C++ and rendered via the MSDF atlas — only
  codepoints baked into the atlas render; others show blank.
