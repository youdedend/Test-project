# Graphics toggles: what the build actually does

Every item on the community guide's list, checked against the code in this repository. "Wired" means
there is a path from the ini key to the thing that runs; "takes effect" means that path is complete
with nothing stubbed in the middle. Line numbers are for this commit.

The guide's list is not the same thing as the port's list. Two items on it did not exist when this
audit started (Bloom, AO Strength) and one was named differently (Less Ghosting).

## The table

| Guide item | Exists | Wired (ini → consumer) | Takes effect |
|---|---|---|---|
| Disable Chromatic Aberration | yes | `bbport_effects.h:50` → `bbport_settings.cpp:152` (ini) → `prepare_game.cpp:2137` (patch) → `patches/Bloodborne.xml:261` | yes, after restart |
| Disable Motion Blur | yes | `bbport_effects.h:53` → `prepare_game.cpp:2137` → `patches/Bloodborne.xml:251` | yes, after restart |
| Disable DoF | yes | `bbport_effects.h:52` → `prepare_game.cpp:2137` → `patches/Bloodborne.xml:271` | yes, after restart |
| Screen-space reflections (SSR) | yes, off by default | `bbport_effects.h:62` → `prepare_game.cpp:2137` → `patches/Bloodborne.xml:281` | yes, after restart |
| Dynamic shadows | yes | `bbport_effects.h:60` → `prepare_game.cpp:2137` → `patches/Bloodborne.xml:256` | yes, after restart |
| Ambient occlusion (SSAO) | yes, on/off only | `bbport_effects.h:58` → `prepare_game.cpp:2137` → `patches/Bloodborne.xml:276` | yes, after restart |
| AO Strength slider | **no** | — | **no.** The game takes an AO constructor, not an AO strength. The patch removes the pass; there is no scalar to scale. The console menu now says so (`bbport_overlay.cpp:379`) instead of showing a slider that would move nothing. |
| Model detail (LOD) | yes | `bbport_settings.cpp:137` (`model_lod`, −2..2) → startup patch selection `prepare_game.cpp:2159` | yes, after restart |
| Bloom | **added in this change** | `bbport_settings.h:174` → `bbport_settings.cpp:84` (parse) / `:400` (save) → `vk_presenter.cpp:692` → `pp_pass.h:30` push constant → `post_process.frag:17` | yes, live. See "Bloom" below for what it does and does not do. |
| AA / Sharpening | yes | `sharpen`/`sharpness` (`bbport_settings.cpp:99-102`) → RCAS in `vk_temporal_upscaler.cpp:740`, `vk_fsr4.cpp:403`, `fsr411.cpp:636`; values >1 add a second pass (`vk_temporal_upscaler.cpp:858`) | yes, live |
| "Less Ghosting" | yes, under another name | the guide's term is the object-motion pass: `object_motion` (`bbport_settings.cpp:107`, which now also accepts the port's `less_ghosting=` spelling) → `vk_camera_motion.cpp:238` | yes; **off** by default on the console, ~5–10% frame cost. The console menu label is now "Less ghosting (object motion vectors)" (`bbport_overlay.cpp:308`) so the two names are the same control. |
| Live Resolution Changes | PC only | `live_resolution` (`bbport_settings.cpp:139`) → `run.py:460` | **not on the console.** `xbox/platform/main.cpp:1399` sets `BB_RENDER_RES`, which fixes the 3D scene at startup; the console menu used to claim the scene resolution "applies immediately on the next frame" anyway. It now says the truth (`bbport_overlay.cpp:243`). |

## What changed in this pass

1. **Bloom exists now.** It is a host post-process step, not the game's bloom (which is already
   composited into the frame before this port sees it, and cannot be removed without a patch to the
   game). It compresses highlights above a knee of 0.72 in linear light before the transfer curve, so
   a lantern lights its surroundings instead of turning its core into a white disc. Cost: one branch
   and a `exp()` on values that already exceed the knee, in a pass that already runs — no extra
   render target, no extra bandwidth, nothing allocated. `bloom=0` is the default and is bit-exact:
   the branch is not taken. It is skipped when HDR output is on, because that image is graded by the
   TV.
2. **One table instead of three.** `gpu/shim/bbport_effects.h` is now the only list of effects: the
   ini reader/writer (`bbport_settings.h:43-45`), the console's patch compiler
   (`prepare_game.cpp:2086`) and the test read the same array. `scripts/patches.py` keeps its Python
   table (it is a separate process and a separate language) and the test checks it against this one.
3. **Ambient occlusion says what it is.** The console menu no longer implies an AO strength control
   exists.
4. **The scene-resolution hint stopped lying** when the session is startup-patched.
5. **The post-process push constant is asserted**, not documented: `static_assert` on all four
   offsets in `pp_pass.h:35-39`, and `xbox/tests/graphics_toggles_test.cpp` checks the shader's block
   matches it. A mismatch there is a picture that ignores the menu.

## New test

`xbox/tests/graphics_toggles_test.cpp`, registered in `.github/workflows/xbox.yml`. It reads the files
that ship and checks that they agree with the table:

* every `patch_off`/`patch_on` name exists as a `Metadata Name="…"` in `patches/Bloodborne.xml`;
* `scripts/patches.py` maps the same key to the same patch names;
* every runtime toggle is parsed, stored and saved by `bbport_settings.cpp` and shown by the menu;
* bloom reaches the shader (the push constant carries it, the presenter fills it every frame, the
  shader reads it), which is what distinguishes a working toggle from a checkbox.

It cannot check that the picture looks right. That needs the console.

## Still needs a console run

Nothing here has been run on hardware yet. Specifically unverified on a Series S/X: that the bloom
rolloff looks the way the description says at 25/50/75, that it costs nothing measurable, and that
`bloom=0` really is bit-identical (it should be — the branch is not taken — but "should be" is not a
measurement). Everything else in the table was already shipping before this change; the audit found
the wiring intact and added the checks that would notice if it stopped being intact.
