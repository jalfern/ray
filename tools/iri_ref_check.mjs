#!/usr/bin/env node
/* Float64 port of the three.js KHR_materials_iridescence reference (the exact
 * GLSL the web viewer runs), used as the reference side of the thin-film math
 * parity check.  Prints the same grid as tools/iri_check.c (CPU float32).
 *
 * Sources ported (web_viewer/node_modules/three/src/renderers/shaders/):
 *   ShaderChunk/iridescence_fragment.glsl.js          (evalSensitivity, evalIridescence,
 *                                                      Fresnel0ToIor, IorToFresnel0)
 *   ShaderChunk/common.glsl.js                        (F_Schlick, Epic SIGGRAPH '13)
 *   ShaderChunk/lights_physical_pars_fragment.glsl.js (Schlick_to_F0)
 *
 * A self-check first asserts that every numeric constant below also appears in
 * the vendored GLSL chunk, so the port cannot silently drift from the file.
 */
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const extra = process.env.IRI_THREE_DIR ? join(here, process.env.IRI_THREE_DIR)
    : join(here, '../web_viewer/node_modules/three/src/renderers/shaders/ShaderChunk');

/* ── self-check: every constant used below exists in the real GLSL ─────── */
const glslIri = readFileSync(join(extra, 'iridescence_fragment.glsl.js'), 'utf8');
const glslComm = readFileSync(join(extra, 'common.glsl.js'), 'utf8');
const glslPhys = readFileSync(join(extra, 'lights_physical_pars_fragment.glsl.js'), 'utf8');
function has(file, lit) {
    if (!file.includes(lit)) {
        console.error(`CONSTANT MISMATCH: ${lit} not found in vendored GLSL`);
        process.exitCode = 1;
    }
}
[
    '5.4856e-13', '4.4201e-13', '5.2481e-13',
    '1.6810e+06', '1.7953e+06', '2.2084e+06',
    '4.3278e+09', '9.3046e+09', '6.6121e+09',
    '9.7470e-14', '4.5282e+09', '2.2399e+06',
    '1.0685e-7',
    '3.2404542', '-0.9692660', '0.0556434',
    '-1.5371385', '1.8760108', '-0.2040259',
    '-0.4985314', '0.0415560', '1.0572252',
].forEach(l => has(glslIri, l));
/* the GLSL spellings are " - 5.55473 * dotVH" / " - 6.98316 " (space after minus) */
['5.55473', '6.98316'].forEach(l => has(glslComm, l));
['0.9999'].forEach(l => has(glslPhys, l));
if (process.exitCode) process.exit(1);

/* ── the port ─────────────────────────────────────────────────────────── */
const PI = Math.PI;

function fSchlick(f0, f90, cosV) {
    const fresnel = 2 ** ((-5.55473 * cosV - 6.98316) * cosV);
    return f0 * (1 - fresnel) + f90 * fresnel;
}

function iorToFresnel0(t, inc) {
    const x = (t - inc) / (t + inc);
    return x * x;
}

function fresnel0ToIor(f0) {
    const s = Math.sqrt(f0);
    return (1 + s) / (1 - s);
}

function evalSensitivity(OPD, shift) {
    const phase = 2 * PI * OPD * 1e-9;
    const rr = [5.4856e-13, 4.4201e-13, 5.2481e-13];
    const pp = [1.6810e+06, 1.7953e+06, 2.2084e+06];
    const vv = [4.3278e+09, 9.3046e+09, 6.6121e+09];
    const xyz = [0, 0, 0];
    for (let c = 0; c < 3; c++) {
        xyz[c] = rr[c] * Math.sqrt(2 * PI * vv[c])
            * Math.cos(pp[c] * phase + shift[c])
            * Math.exp(-(phase * phase) * vv[c]);
    }
    xyz[0] += 9.7470e-14 * Math.sqrt(2 * PI * 4.5282e+09)
        * Math.cos(2.2399e+06 * phase + shift[0])
        * Math.exp(-4.5282e+09 * phase * phase);
    for (let c = 0; c < 3; c++) xyz[c] /= 1.0685e-7;
    /* XYZ_TO_REC709 (column-major mat3 in the GLSL) */
    return [
        3.2404542 * xyz[0] - 1.5371385 * xyz[1] - 0.4985314 * xyz[2],
        -0.9692660 * xyz[0] + 1.8760108 * xyz[1] + 0.0415560 * xyz[2],
        0.0556434 * xyz[0] - 0.2040259 * xyz[1] + 1.0572252 * xyz[2],
    ];
}

function evalIridescence(outsideIOR, eta2, cosTheta1, thinFilmThickness, baseF0) {
    let s = thinFilmThickness / 0.03;
    s = Math.min(1, Math.max(0, s));
    const mixv = outsideIOR + (eta2 - outsideIOR) * (s * s * (3 - 2 * s));
    const sinTheta2Sq = (outsideIOR / mixv) * (outsideIOR / mixv) * (1 - cosTheta1 * cosTheta1);
    const cosTheta2Sq = 1 - sinTheta2Sq;
    if (cosTheta2Sq < 0) return [1, 1, 1];
    const cosTheta2 = Math.sqrt(cosTheta2Sq);

    const R0 = iorToFresnel0(mixv, outsideIOR);
    const R12 = fSchlick(R0, 1, cosTheta1);
    const T121 = 1 - R12;
    let phi12 = 0.0;
    if (mixv < outsideIOR) phi12 = PI;
    const phi21 = PI - phi12;

    const bIOR = [0, 0, 0];
    for (let c = 0; c < 3; c++) bIOR[c] = fresnel0ToIor(Math.min(0.9999, Math.max(0, baseF0[c])));
    const R1 = [iorToFresnel0(bIOR[0], mixv), iorToFresnel0(bIOR[1], mixv), iorToFresnel0(bIOR[2], mixv)];
    const R23 = [fSchlick(R1[0], 1, cosTheta2), fSchlick(R1[1], 1, cosTheta2), fSchlick(R1[2], 1, cosTheta2)];
    const phi23 = [0, 0, 0];
    for (let c = 0; c < 3; c++) if (bIOR[c] < mixv) phi23[c] = PI;

    const OPD = 2 * mixv * thinFilmThickness * cosTheta2;
    const phi = [phi21 + phi23[0], phi21 + phi23[1], phi21 + phi23[2]];

    const R123 = [0, 0, 0];
    const r123 = [0, 0, 0];
    const Rs = [0, 0, 0];
    for (let c = 0; c < 3; c++) {
        R123[c] = Math.min(0.9999, Math.max(1e-5, R12 * R23[c]));
        r123[c] = Math.sqrt(R123[c]);
        Rs[c] = T121 * T121 * R23[c] / (1 - R123[c]);
    }
    const I = [R12 + Rs[0], R12 + Rs[1], R12 + Rs[2]];
    let Cm = [Rs[0] - T121, Rs[1] - T121, Rs[2] - T121];
    for (let m = 1; m <= 2; m++) {
        for (let c = 0; c < 3; c++) Cm[c] *= r123[c];
        const Sm = evalSensitivity(m * OPD, [m * phi[0], m * phi[1], m * phi[2]]);
        for (let c = 0; c < 3; c++) I[c] += Cm[c] * 2.0 * Sm[c];
    }
    return [Math.max(0, I[0]), Math.max(0, I[1]), Math.max(0, I[2])];
}

function schlickToF0(f, f90, dotVH) {
    const x = Math.min(1, Math.max(0, 1 - dotVH));
    const x2 = x * x;
    const x5 = Math.min(0.9999, Math.max(0, x * x2 * x2));
    return [(f[0] - f90 * x5) / (1 - x5),
            (f[1] - f90 * x5) / (1 - x5),
            (f[2] - f90 * x5) / (1 - x5)];
}

/* ── same grid as tools/iri_check.c ───────────────────────────────────── */
const cosv = [0.15, 0.30, 0.50, 0.75, 0.95, 1.0];
const thicks = [385, 395, 405, 485, 500, 515];
const iors = [1.8, 2.0];
const f0s = [[1, 1, 1], [0.04, 0.04, 0.04], [0.5, 0.4, 0.3]];

for (const cv of cosv)
    for (const d of thicks)
        for (const nf of iors)
            for (const f0 of f0s) {
                const film = evalIridescence(1.0, nf, cv, d, f0);
                const f0out = schlickToF0(film, 1.0, cv);
                console.log([cv.toFixed(4), d.toFixed(1), nf.toFixed(2),
                    ...film.map(v => v.toExponential(9)),
                    ...f0out.map(v => v.toExponential(9))].join(' '));
            }
