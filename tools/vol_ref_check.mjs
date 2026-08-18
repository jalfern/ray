#!/usr/bin/env node
/* Float64 port of the three.js KHR_materials_volume reference (the exact
 * GLSL the web viewer runs), used as the reference side of the volume
 * math parity check.  Prints the same grid as tools/vol_check.c (CPU
 * float32).
 *
 * Source ported (web_viewer/node_modules/three/src/renderers/shaders/
 * ShaderChunk/transmission_pars_fragment.glsl.js):
 *
 *   vec3 volumeAttenuation( const in float transmissionDistance,
 *       const in vec3 attenuationColor, const in float attenuationDistance ) {
 *       if ( isinf( attenuationDistance ) ) { return vec3( 1.0 ); }
 *       else {
 *           vec3 attenuationCoefficient = -log( attenuationColor ) / attenuationDistance;
 *           vec3 transmittance = exp( - attenuationCoefficient * transmissionDistance );
 *           return transmittance;
 *       }
 *   }
 *
 * A self-check first asserts the expression this port mirrors is still
 * verbatim in the vendored GLSL chunk, so the port cannot silently
 * drift from the file.
 */
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const extra = process.env.VOL_THREE_DIR ? join(here, process.env.VOL_THREE_DIR)
    : join(here, '../web_viewer/node_modules/three/src/renderers/shaders/ShaderChunk');

/* ── self-check: the ported expression lives verbatim in the GLSL ──── */
const glsl = readFileSync(join(extra, 'transmission_pars_fragment.glsl.js'), 'utf8');
const must = [
    'vec3 volumeAttenuation(',
    'isinf( attenuationDistance )',
    'vec3( 1.0 )',
    '-log( attenuationColor ) / attenuationDistance',
    'exp( - attenuationCoefficient * transmissionDistance )',
];
for (const m of must) {
    if (!glsl.includes(m)) {
        console.error(`GLSL DRIFT: ${m} not found in transmission_pars_fragment.glsl.js`);
        process.exit(1);
    }
}

/* ── the port ─────────────────────────────────────────────────────── */
function volumeAttenuation(dist, c, d) {
    if (d === Infinity) return [1, 1, 1];
    const coefR = -Math.log(c[0]) / d;
    const coefG = -Math.log(c[1]) / d;
    const coefB = -Math.log(c[2]) / d;
    return [Math.exp(-(coefR * dist)), Math.exp(-(coefG * dist)), Math.exp(-(coefB * dist))];
}

const xs = [0, 0.005, 0.01, 0.02, 0.05, 0.2, 1.0];
const ds = [Infinity, 10, 1, 0.1, 0.05, 0.005];
const cs = [
    [1, 1, 1],
    [0.9, 0.6, 0.3],
    [0.4, 0.7, 0.9],
    [0.7, 0.45, 0.25],
];

const fmt = (v) => (v === Infinity ? 'inf' : v.toExponential(10));
for (const x of xs)
    for (const d of ds)
        for (const c of cs) {
            const t = volumeAttenuation(x, c, d);
            console.log([fmt(x), fmt(d), ...c.map(fmt), ...t.map(fmt)].join(' '));
        }
