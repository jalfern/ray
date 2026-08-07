#!/usr/bin/env python3
"""Auto-generate scene JSON files from OBJ models in the models/ directory."""
import json, os, sys
import glob
import math

RAY_DIR = os.path.dirname(os.path.abspath(__file__))
MODELS_DIR = os.path.join(RAY_DIR, "models")
SCENES_DIR = os.path.join(RAY_DIR, "scenes")
IMAGES_DIR = os.path.join(RAY_DIR, "images")

def get_obj_bbox(filepath):
    """Returns (center, max_diameter) for an OBJ file."""
    mins = [float('inf')] * 3
    maxs = [float('-inf')] * 3
    verts = 0
    with open(filepath) as f:
        for line in f:
            if line.startswith('v '):
                parts = line.split()
                for i in range(3):
                    val = float(parts[i+1])
                    if val < mins[i]: mins[i] = val
                    if val > maxs[i]: maxs[i] = val
                verts += 1
    if verts == 0 or mins[0] == float('inf'):
        return None
    cx = (mins[0] + maxs[0]) / 2
    cy = (mins[1] + maxs[1]) / 2
    cz = (mins[2] + maxs[2]) / 2
    diameter = max(maxs[0] - mins[0], maxs[1] - mins[1], maxs[2] - mins[2])
    return ([cx, cy, cz], diameter, verts)

def generate_scene(model_name, output_name=None, width=1280, height=720, material="plastic", color=[0.9, 0.85, 0.8], light_pos=None):
    filepath = os.path.join(MODELS_DIR, f"{model_name}.obj")
    if not os.path.exists(filepath):
        print(f"  Model not found: {filepath}")
        return None
    
    info = get_obj_bbox(filepath)
    if info is None:
        print(f"  No vertices in {model_name}.obj")
        return None
    
    center, diameter, verts = info
    
    # Scale model so it fits nicely in the scene (unit ~2-3 units tall)
    scale = 3.0 / diameter
    
    # Position: centered at origin, lifted so it sits on the floor (y=0)
    pos = [0, (center[1] * scale), 0]
    
    # Camera: positioned to view the model from slightly above and to the side
    camera_z = math.sqrt(scale * scale * diameter) * 2.5 + 3
    camera_target = [center[0] * scale, center[1] * scale + diameter * scale * 0.1, center[2] * scale]
    
    scene = {
        "width": width,
        "height": height,
        "camera": {
            "pos": [0, camera_z * 0.35, camera_z],
            "target": camera_target,
            "aperture": 0.2,
            "focus_dist": camera_z
        },
        "meshes": [{
            "file": f"../models/{model_name}.obj",
            "pos": pos,
            "scale": scale,
            "color": color,
            "material": material,
            "reflectivity": 0.15
        }],
        "lights": [{
            "pos": light_pos or [3, 8, 5],
            "size": 3
        }],
        "floor": {"checkerboard": True},
        "environment": {
            "file": "../envmaps/ambient.hdr",
            "intensity": 0.3
        },
        "output": f"images/{output_name or f'render_{model_name}'}.png"
    }
    
    scene_file = os.path.join(SCENES_DIR, f"scene_{model_name}.json")
    with open(scene_file, 'w') as f:
        json.dump(scene, f, indent=2)
    
    print(f"  Scene: {scene_file} ({verts} verts, scale={scale:.4f})")
    return scene_file

def main():
    model_names = set()
    
    if len(sys.argv) > 1:
        model_names.update(sys.argv[1:])
    else:
        for fp in glob.glob(os.path.join(MODELS_DIR, "*.obj")):
            model_names.add(os.path.basename(fp).replace('.obj', ''))
    
    print(f"Generating scenes for {len(model_names)} models:\n")
    for name in sorted(model_names):
        print(f"  {name}")
        generate_scene(name)

if __name__ == "__main__":
    main()