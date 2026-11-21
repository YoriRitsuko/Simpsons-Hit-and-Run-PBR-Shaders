# Put your own materials in a mod

Start with a part you can find easily in-game, like a hood or shop window.
In Lucas's Pure3D Editor, find the **Shader** chunk the part uses. Copy its
**Name**, including the `_m` if it has one. That's what PBR Shaders matches.
The mesh needs positions, normals, and UV0. Keep its regular `TEX` binding
for the stock draw. [Pure3D Shader chunk](https://docs.donutteam.com/docs/Pure3DFiles/ChunkTypes/Shader)

## Pack a texture set

On Windows, with PowerShell 7 and Node 22 or newer, run this from the
project folder.

```powershell
pwsh -NoProfile -STA -File .\tools\PbrTexturePacker.ps1
```

Enter the shader name and choose your maps. The packer can read PNG, BMP,
JPG, and uncompressed TGA. For reflections, pick a mirror-ball `.hdr` or an
RGBA16F `.dds` already made by the environment prefilter. The normal map
uses tangent space. Its tangent-space green channel needs to match the
model's UV convention.

Use **Packed ORM** if you already have one. Otherwise, pick separate AO,
roughness, and metallic maps of the same size. The packer takes the red
channel from each and puts it into R, G, and B respectively. Missing
channels use white AO, middle roughness, and zero metal.

Pick an **Input model** if you want the material inside the model's `.p3d`.
Leave that field blank to make a smaller material-only `.p3d`. Choose an
output and click **Pack**. Pack after editing the model so the custom chunk
travels with the final file. You can leave Base Color blank to use the game's
bound texture. That's handy for keeping a car's damage textures.

## Put it in the LMLM

Ship the output in your mod using Lucas's Custom Files hack. A packed model
goes wherever the game normally loads that model. Since the material chunk
is at the start of the file, PBR Shaders sees it before the model's shader.

If you made a separate material file, have your path handler load or prepend
it before the model's shader chunks. The demo's `Sedan.lua` shows how to put
both into one virtual file. This lets you use the original game model where
the player already has it. [Custom Files path handlers](https://docs.donutteam.com/docs/LucasSimpsonsHitAndRunModLauncher/Hacks/CustomFiles/PathHandlerScripts)

Add these lines to your mod's `Meta.ini` and compile the mod in Lucas.

```ini
RequiredHack=VehicleShaders
RequiredHack=CustomFiles
```

`VehicleShaders` is the dependency ID for PBR Shaders. The material maps and
settings live in the `.p3d` you ship. Use a unique shader name for a new model
so it doesn't collide with another mod.

## Try a window

Pick **Interior** in the packer. Use a flat window model with normals and
UV0. If the prop needs to show from both sides, give one surface a two-sided
shader.

| Map | RGB | Alpha |
| --- | --- | --- |
| Base color | Façade and window frame | White reveals the room. Black keeps the exterior. |
| Interior mirror-ball | Room colors | White adds emission. Black uses the room brightness alone. |

Capture the mirror-ball map from the center of the room, with the back wall
at the center of the disc. The view ray hits a box behind the glass, so the
room shifts when you walk sideways. Use a separate exterior environment
map for what the glass reflects.

## Tune a material by hand

Use a plain INI while you're working and pack it into the Pure3D file when
you're ready. Keep the maps next to the INI during authoring. This route
takes uncompressed 24- or 32-bit TGA and prefiltered RGBA16F DDS.

```ini
[shop_window_m]
Mode=Interior
BaseColor=shop-facade.tga
InteriorMap=shop-room.tga
InteriorDepth=3.0
InteriorBrightness=0.7
EmissiveStrength=2.0
GlassRoughness=0.08
GlassReflectance=0.12
```

```powershell
node tools/pack-materials.cjs shop.ini shop-packed.p3d shop.p3d
```

The last argument is optional. The tool replaces any earlier PBR material
chunk in the input model and copies the other chunks. For a square
mirror-ball RGBE HDR, you can make the reflection DDS with this command.

```powershell
node -e "require('./tools/prefilter-environment.cjs').prefilterEnvironment(process.argv[1], process.argv[2])" sky.hdr sky.dds
```

- **PBR** uses `Roughness`, `Metallic`, `BaseColor`, `NormalMap`, `ORM`,
  `EmissiveMap`, and `Environment`. Set `AlphaMode=Mask` for cutouts or
  `AlphaMode=Blend` for glass. The stock model needs the matching draw mode.
- **Interior** also uses `InteriorDepth`, `InteriorTilesU`, `InteriorTilesV`,
  `InteriorBrightness`, `GlassRoughness`, and `GlassReflectance`. The room
  borrows the RGB color for emission, with the alpha channel as its mask.
- **Glass** can use the game's `REFL` binding or an authored `SphereMap`.
  Use `Reflection` to set its strength.
- **Paint** is the older vertex-mask workflow. It uses `NormalMap`,
  `SphereMap`, `PaintColour`, and `MaskSet`.

The editable [demo settings](../tools/demo-materials.ini) show how all the
preview props use the same packed reflection map. If a material looks stock,
check the shader name, UV0, and whether the material package is appended
before the relevant shader is loaded.

Already have a loose setup? You can pack your existing INI and maps as
they are. The [legacy example](legacy-materials.ini) uses the same keys.
