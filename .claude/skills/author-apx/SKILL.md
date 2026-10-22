---
name: author-apx
description: Create, modify and verify .apx project files for the apEx engine (the format behind Clean Slate and the other bundled demos), and check that a project loads and renders. Use when asked to make a new demo project, hand-edit or generate a .apx, port a project, or explain why a .apx fails to import or renders nothing.
---

# Authoring a `.apx`

`.apx` is the apEx engine's project format: XML, one file, everything the demo
needs -- shaders, materials, models, scenes, and the timeline of events that
draws them.

**Read [`docs/spec/apx-format.md`](../../../docs/spec/apx-format.md) first.** It
is the source of truth and it was reverse-engineered from the importer, so it
says not only what the format is but what the loader actually does with it.
Sections worth knowing by number:

| Need | Section |
| --- | --- |
| Top-level elements, and that they are read by name not order | §1.3 |
| GUIDs, half-floats, base64, splines, matrices | §2.1-§2.7 |
| What `ImportMain` does, and what it defers | §3, §3.1 |
| `Object/@Type` enum and each type's children | §10.1 |
| A verbatim, complete smallest scene | §10.3 |
| `<event>`: type is a *child*, frame fields, per-type payloads | §11 |

## Two ways in

**Derive from a project that already works.** This is what
[`tools/make_hostile_apx.py`](../../../tools/make_hostile_apx.py) does: take a
shipped `.apx`, rewrite one element, keep everything else -- shader layouts,
constant buffers, material wiring -- exactly as the engine expects. Use it when
the goal is to test the player. Change one thing at a time; a case that also
changes the layout fails for the wrong reason.

**Build one from nothing.** Possible, and the spec gives you the pieces, but
every GUID must resolve: a material needs a technique, a technique needs a
shader with the right input layout, a scene needs a clip, and an event needs a
render target and a scene. Start from §10.3 rather than from a blank file.

## The minimum that actually renders

* `<Framerate>120</Framerate>` -- this is the timeline rate, and it decides what
  every frame number means. `<AspectX>`/`<AspectY>` set the authored aspect.
* one `<rendertarget>`, one `<rendertechnique>` (with its `<Code>`), one
  `<material>`, one `<model>`, one `<scene>` containing `<Object Type="0">` that
  names the model with `<model>GUID</model>` inside its `<clipdata>`.
* one `<event>`: `<Type>3</Type>` (RENDERSCENE), `<StartFrame>`, `<EndFrame>`,
  `<TargetRT>` (a render target GUID) and `<scene>` (a scene GUID).

## Rules that bite

* **`Type` is a child of `<event>`, not an attribute.** An event without a
  parsable `Type` is skipped in silence.
* **`StartFrame` is inclusive, `EndFrame` exclusive, and both are `unsigned
  short`.** 65535 is the ceiling: at 120 fps a project cannot exceed 546 s.
* **`Object/@Type`** (`Phoenix/Scene.h:95`): 0 Model, 1 Light, 2 CamEye, 3 Dummy,
  4 SubScene, 5 ParticleEmitter (GPU), 6 ParticleEmitterCPU, 7 Drag, 8 Gravity,
  9 Turbulence, 10 Vortex, 11 LogicObject.
* **Spline `type` is the `Spline_*` enum** in `Phoenix/Scene.h`. The original
  loader had no bounds checks (`Texgen_tool.cpp:374`, `Model_Tool.cpp:529`,
  `phxSplineExt.cpp:117`); a spline id past the table used to segfault the port.
  Keep ids inside the enum even though it is now guarded.
* **GUIDs** are 32 uppercase hex characters, unique, and resolved late (§3.1) --
  so a forward reference is fine, a duplicate is not.
* **Markup in text must be escaped.** The generator's `esc()` exists for that.
* **`<Poly>`/`<Polygons>` are unreachable from shipped data**; testing them
  needs a hand-built file (the generator says so where it gives up).

## Verify a project in seconds, without a GUI

```sh
.claude/skills/author-apx/verify.sh path/to/project.apx
```

It loads the project headless, renders a handful of frames, dumps the last one,
and reports the exit code, how many `[import]` lines came out, and the brightest
byte of the frame. A project that loads and renders nothing is otherwise
indistinguishable from one that works.

**Zero import errors is not the bar.** The shipped Clean Slate logs 35:

    [import] Event data import error: referenced parameter <GUID> not found

seven parameters, reported seven times, from shadertoy events naming material
parameters the imported technique does not carry. It plays correctly anyway, so
the number to watch is *relative*: a new project should not report far more than
the ones that work.

Beware three things when you do it by hand:

* `--dump-frame N` is the **iteration index**, not the clock frame: with
  `--frames 30` the last frame is `29`.
* `<Type>0</Type>` (ENDDEMO) stops the run early. That is the project saying
  "done", not a failure.
* A crash means the *importer* found something it did not expect; the last
  `[import]` line on stderr usually names the element.

## Next

Once a project renders, recording it is the other skill:
[`../record-demo/SKILL.md`](../record-demo/SKILL.md) -- `--raw-video` piped into
ffmpeg, with the size and rate read from the player rather than guessed.

## At scale

`tools/run_hostile_apx.py --jobs 8` runs a whole corpus eight at a time (115 cases
in under four minutes, against roughly 55 minutes serially). The bar for any
loader change is: **0 crashes, 0 timeouts, 0 non-zero exits**, and the error
counts no worse than the recorded baseline.
