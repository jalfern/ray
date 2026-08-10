# glTF Extension Plan: KHR_materials_transmission + KHR_materials_ior

## Goal

Wire up transmission and IOR from glTF extensions so that models like
WaterBottle, Lantern, and IridescenceLamp render as glass instead of
falling back to plastic.

The renderer already handles glass (Fresnel, refraction, configurable
IOR) — this is purely a parser + material classification change.

---

## Step 1 — Extend GltfMaterial struct

Add fields to carry transmission and IOR data out of the parser:

```c
typedef struct {
    float base_color[4];
    float metallic;
    float roughness;
    float emissive[3];
    float transmission;   /* 0–1, default 0 */
    float ior;            /* 1.0–3.0, default 1.5 */
} GltfMaterial;
```

Default `transmission = 0` and `ior = 1.5` in the memset/init.

---

## Step 2 — Parse KHR_materials_transmission

In `parse_materials`, add a branch for the `"extensions"` key.
Within the extensions object, look for `"KHR_materials_transmission"`
and read `"transmissionFactor"` (float, default 0).

```c
} else if (strcmp(kbuf, "extensions") == 0) {
    const char* ex = obj;
    skip_ws_ptr(&ex);
    if (*ex == '{') ex++;
    while (*ex && *ex != '}') {
        char ek[64];
        const char* esave = ex;
        if (!parse_json_string(&ex, ek, sizeof(ek))) { ex = esave; skip_value(&ex); continue; }
        skip_ws_ptr(&ex);
        if (*ex == ':') ex++;
        skip_ws_ptr(&ex);
        if (strcmp(ek, "KHR_materials_transmission") == 0) {
            const char* tx = ex;
            skip_ws_ptr(&tx);
            if (*tx == '{') tx++;
            while (*tx && *tx != '}') {
                char tk[64];
                const char* tsave = tx;
                if (!parse_json_string(&tx, tk, sizeof(tk))) { tx = tsave; skip_value(&tx); continue; }
                skip_ws_ptr(&tx);
                if (*tx == ':') tx++;
                skip_ws_ptr(&tx);
                if (strcmp(tk, "transmissionFactor") == 0) {
                    float fv; if (parse_json_number(&tx, &fv)) mats[n].transmission = fv;
                } else {
                    skip_value(&tx);
                }
                skip_ws_ptr(&tx);
                if (*tx == ',') tx++;
            }
            if (*tx == '}') tx++;
            ex = tx;
        } else if (strcmp(ek, "KHR_materials_ior") == 0) {
            const char* ix = ex;
            skip_ws_ptr(&ix);
            if (*ix == '{') ix++;
            while (*ix && *ix != '}') {
                char ik[64];
                const char* isave = ix;
                if (!parse_json_string(&ix, ik, sizeof(ik))) { ix = isave; skip_value(&ix); continue; }
                skip_ws_ptr(&ix);
                if (*ix == ':') ix++;
                skip_ws_ptr(&ix);
                if (strcmp(ik, "ior") == 0) {
                    float fv; if (parse_json_number(&ix, &fv)) mats[n].ior = fv;
                } else {
                    skip_value(&ix);
                }
                skip_ws_ptr(&ix);
                if (*ix == ',') ix++;
            }
            if (*ix == '}') ix++;
            ex = ix;
        } else {
            skip_value(&ex);
        }
        skip_ws_ptr(&ex);
        if (*ex == ',') ex++;
    }
    if (*ex == '}') ex++;
    obj = ex;
}
```

---

## Step 3 — Classify transmission materials as glass

In `build_gltf_scene`, after reading material properties, add
transmission-aware classification:

```c
float transmission = 0.0f;
if (mat_idx >= 0 && mat_idx < num_materials) {
    ...
    transmission = materials[mat_idx].transmission;
}

if (is_emissive) {
    ...
} else if (transmission > 0.0f) {
    strcpy(mo->material, "glass");
    mo->color = (Vec3){base_color[0], base_color[1], base_color[2]};
    mo->reflectivity = 0.0f;
    mo->ior = materials[mat_idx].ior;
} else if (metallic > 0.5f) {
    ...
}
```

The renderer's glass path handles Fresnel, refraction, and IOR
already — this just routes the data correctly.

---

## Step 4 — Test with extension-heavy models

| Model | Extensions Used | Expected Result |
|-------|----------------|-----------------|
| `WaterBottle` | transmission, ior | Renders as glass instead of plastic |
| `Lantern` | transmission, ior | Glass panels render with refraction |
| `IridescenceLamp` | transmission, volume, ior | Transmission parts render as glass |
| `Box` | none | No change (plastic) |
| `Suzanne` | none | No change (plastic) |

Test each with `--cpu` and GPU, verify no crashes and glass
appearance is correct.

---

## Step 5 — Remove or downgrade the extensions warning

Currently `check_extensions` warns if `extensionsRequired` is
non-empty. After this change, transmission and ior are handled,
so the warning should only fire for genuinely unsupported
extensions. Update the check to be more selective, or just
remove the warning since unknown extensions are silently skipped
anyway.

---

## What this does NOT cover

- `KHR_materials_volume` (thickness, attenuation) — renderer has
  no volume absorption path
- `KHR_materials_iridescence` — renderer has no thin-film
  interference model
- Texture-based transmission/ior (only factor values) — same
  limitation as the existing base color path