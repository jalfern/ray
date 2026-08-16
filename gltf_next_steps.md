> **SUPERSEDED (2026-08-16).** Phase 1 bugs are all fixed (see `nextsteps.md`).
> Phase 2 transmission/ior is done; texture support is partway done (base color +
> ORM roughness). The current material/texture plan (per-pixel PBR, iridescence)
> lives in `nextsteps.md` → "Potential Next Steps".

# glTF Next Steps — Bugs & Extensions

## Phase 1: Fix Open Bugs (affect all rendering, including glTF)

All bug details in `ray-bugs.md`. Priority order below.

### 1. Shadow rays don't skip origin mesh — Issue 5 (shared, both backends)
- `in_shadow()` in `renderer.cc:269` and `shaders.metal:198` accepts `skip_sphere` but has **no `skip_mesh` parameter**
- The mesh BVH loop traverses all meshes, including the one the shaded point lives on
- Self-intersections produce black shadow acne on mesh objects
- Pattern already exists in `emissive_visible()` — just replicate it
- **Fix:** add `skip_mesh` param to `in_shadow` on both backends, pass the hit mesh index from `trace_ray`, and `continue` when `m == skip_mesh`
- **Files:** `src/renderer/renderer.cc`, `src/renderer/shaders.metal`

### 2. Emissive mesh normal not flipped — Issue 4 (shared, both backends)
- `sample_emissive_mesh` interpolates the emissive normal but never flips it toward the shaded point
- When normals face away, `cos_light <= 0` and the sample is rejected — emissive meshes contribute nothing from unfavorable windings
- **Fix:** after computing the emissive normal, `if (dot(normal, wi) < 0) normal = -normal`
- **Files:** `src/renderer/renderer.cc`, `src/renderer/shaders.metal`

### 3. CPU missing negative clamp — Issue 3 (CPU-only)
- `render_rows` line 594 uses `fminf(color_avg.x, 1.0f)` but no `fmaxf(0.0f, ...)`
- Negative values (possible via Fresnel combos) wrap around on uint8 cast → bright garbage pixels
- **Fix:** `(uint8_t)(fmaxf(0.0f, fminf(color_avg.x, 1.0f)) * 255.0f)`
- **Files:** `src/renderer/renderer.cc`

### 4. Camera zenith/nadir singularity — Issue 6 (shared, both backends)
- `cross((V){0,1,0}, fwd)` returns zero when looking straight up/down
- **Fix:** guard with alternate up `(0,0,1)` when `|dot(fwd, up)|` near 1
- **Files:** `src/renderer/renderer.cc`, `src/renderer/shaders.metal`

---

## Phase 2: glTF Extension Support

### 1. KHR_materials_transmission + KHR_materials_ior (glass)
- **What it adds:** `transmissionFactor` (0-1), `transmissionTexture`, `ior` (1.0-3.0) in material extensions
- **Renderer already handles:** glass material with configurable IOR, Fresnel, refraction
- **Parser gap:** `gltf_parser.cc` material parser only reads `pbrMetallicRoughness` and `emissiveFactor` — extension objects are skipped
- **Work items:**
  - Parse `extensions` object in each material for `KHR_materials_transmission`
  - If `transmissionFactor > 0`, classify as `MAT_GLASS` with `ior` from `KHR_materials_ior` (or default 1.5)
  - Respect `alphaMode`/`alphaCutoff` for transparency
  - Emit warning if `transmissionTexture` referenced (not yet supported, fall back to factor)
  - Parse `KHR_materials_ior.ior` value
- **Test scenes:** WaterBottle already uses transmission/ior — currently falls back to plastic

### 2. KHR_lights_punctual (light nodes)
- **What it adds:** point, directional, and spot lights defined in node extensions
- **Renderer already handles:** point lights via `Light` struct with `pos` and `size`
- **Parser gap:** lights are only created from the JSON scene's `"lights"` array
- **Work items:**
  - Parse `extensions`/`KHR_lights_punctual` at the glTF root for light definitions
  - During node tree walk, check each node's `extensions` for light references
  - Convert to `Light` structs appended to the scene's light list
  - Spot light cone attenuation can be approximated (size=0 for directional falloff)

### 3. Texture/UV support for glTF materials
- **What it adds:** `baseColorTexture`, `metallicRoughnessTexture`, `emissiveTexture`, `normalTexture`
- **Parser already:** reads UVs from `TEXCOORD_0` accessor but hardcodes them to zero (`gltf_parser.cc:1306`)
- **Work items:**
  - Forward UV data through the pipeline instead of zeroing it
  - Parse texture/sampler/image references from glTF
  - Map to the renderer's texture system (tex_type, tex_scale)
  - Base color textures → plastic/metallic materials
  - Emissive textures → modulate emissive factor
- **Note:** The renderer's texture system supports checker/polka/marble/rings procedurals — glTF textures would require a new path (image-based UV mapping)

### 4. Emissive mesh BVH wiring
- **Status:** Already works correctly. The renderer's `setup_context` iterates all `scene->meshes` and checks `mat_name_to_type()`, which returns `MAT_EMISSIVE` for `"emissive"` — the glTF parser already sets this. The note in `nextsteps.md` is stale.

---

## Phase 3: Testing & Polish

### New test scenes
- Create glTF scenes exercising:
  - Transmission + IOR (WaterBottle already has it — verify it renders as glass)
  - Punctual lights
  - Multi-material meshes
  - Textured materials (when supported)
- Add JSON scene files in `test_scenes/` referencing these

### Test each fix
- `make test` — renders all test scenes
- `./ray2 --cpu <scene>` and GPU — verify parity
- Visually inspect glTF scenes for correct material classification, lighting, camera

### Verify against sample models
- Re-render: Box, Suzanne, Avocado, BoomBox, Lantern, WaterBottle
- Confirm no regressions after each change