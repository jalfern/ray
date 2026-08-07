# Texture Mapping Demo Scene

This scene demonstrates the UV texture mapping implementation in the ray tracer. It showcases various texture types applied to different objects:

## Features Demonstrated

1. **Textured Spheres**:
   - Checkerboard pattern (type 1) with red base color
   - Polka dot pattern (type 2) with purple base color
   - Marble pattern (type 3) with green base color
   - Ring pattern (type 4) with yellow base color

2. **Textured Mesh Object**:
   - Uses the cube_uv.obj model with UV-based texture mapping
   - Ring pattern (type 4) with orange base color

3. **Textured Floor**:
   - Checkerboard pattern (type 1) with dark gray base color

## Texture Types

The scene utilizes all five texture types defined in the project:
- **Type 0**: No texture (default)
- **Type 1**: Checkerboard pattern
- **Type 2**: Polka dot pattern  
- **Type 3**: Marble pattern
- **Type 4**: Ring pattern

The textures are applied consistently across spheres, mesh objects, and floor using the same underlying UV mapping logic that was implemented in the previous work.