# Put PBR materials on your car

Start with one painted panel on a car you can already spawn. Once that panel works, add the other materials one at a time. The same setup works on rigid props and level meshes with normals and a UV set.

## Get the paint working

1. Install `PBR Shaders.lmlh` in the launcher's `Hacks` folder and tick **PBR Shaders** under **Settings**. Make a `VehicleShaders` folder beside `Simpsons.exe` and put `VehicleShaders.ini` in it. Use the existing INI if you already have one. [Example INI](../VehicleShaders/VehicleShaders.ini)
2. Open your car's `.p3d` in Lucas' Pure3D Editor. Find the **Shader** chunk the body panel's primitive group uses and copy its **Name**. Export the mesh with positions, normals, and UV0. For a new opaque material, use `simple` as its `PddiShaderName` and keep its regular `TEX` texture so it still has a stock material. [Pure3D Shader chunk](https://docs.donutteam.com/docs/Pure3DFiles/ChunkTypes/Shader)
3. Add this section to the INI and replace `my_car_body_m` with the shader name from step 2. A custom name should be unique across the assets you load. When working with an existing damageable car, keep the original panel shader names and add a section for each panel you want to change.

```ini
[my_car_body_m]
Mode=PBR
Roughness=0.30
Metallic=0.0
```

At this point the shader uses your car's current base texture, the mesh normals, and the built-in environment. Leaving `BaseColor` out lets the game keep supplying the current texture as the car takes damage. `Roughness` near `0.2` gives a sharp reflection. Raise it toward `0.6` for a soft, dull finish. Paint and plastic use `Metallic=0`. Try the car in daylight and turn the camera to see the highlight move.

Load your model through your car mod as usual. A replacement for the Family Sedan, for example, can go in the car mod's `CustomFiles/art/cars/famil_v.p3d`. Set `RequiredHack=VehicleShaders` in that mod's `[Miscellaneous]` section to make Lucas load PBR Shaders as a dependency when the hack is installed. [Custom Files](https://docs.donutteam.com/docs/LucasSimpsonsHitAndRunModLauncher/Hacks/CustomFiles/Intro)

## Add your texture set

Save the maps as uncompressed 24-bit or 32-bit true-color TGA files. Use 32-bit for a base color with transparency. Put the files beside `VehicleShaders.ini` and use bare filenames for the INI values.

| INI key | What to export | How to pack it |
| --- | --- | --- |
| `BaseColor` | sRGB TGA | RGB is the paint color. Alpha is optional opacity. White alpha is solid and black is transparent. |
| `NormalMap` | Linear tangent-space RGB TGA | RGB encodes X, Y, and Z. A flat normal is roughly `(128, 128, 255)`. Flip green if the bake uses the opposite Y convention. |
| `ORM` | Linear RGB TGA | **R** is ambient occlusion, **G** is roughness, **B** is metallic. The shader ignores the alpha channel. |
| `EmissiveMap` | sRGB RGB TGA | Black is off. Paint the color you want the material to emit directly into RGB. |
| `Environment` | Prefiltered RGBA16F DDS mirror-ball map | Optional. Leave it out to use the built-in environment. |

To make an ORM map, start with three grayscale maps at the same size. Paste occlusion into red, roughness into green, and metallic into blue. Use white red if you have no AO bake. In green, dark pixels are glossy and light pixels are rough. In blue, use black for paint or plastic and white for bare metal. If your tool exports glossiness, invert it before pasting it into green. Export the packed map as data with no sRGB conversion. Use the same data export for the normal map.

Add `NormalMap=body_normal.tga` and `ORM=body_orm.tga` to the body section once those files are ready. An ORM map supplies roughness and metallic per pixel. For a single finish, you can leave it out and use the two numbers from the first example. Add `BaseColor=body_color.tga` when you want a fixed texture for the material.

If you want your own environment, use a square power-of-two Radiance RGBE `.hdr` mirror-ball image with scanline compression. From the repo folder, run the converter and copy the generated DDS next to the INI. Its mips go from sharp to progressively rougher reflections.

```powershell
node -e "require('./tools/prefilter-environment.cjs').prefilterEnvironment(process.argv[1], process.argv[2])" .\garage-mirrorball.hdr .\garage.dds
```

Then add `Environment=garage.dds` to the material section.

## Make a light or badge glow

Give the lamp insert its own Pure3D shader if you want to tune it separately from the rest of the car. Paint the illuminated pixels into `tail_emission.tga`, with black everywhere else. Lamps can share the same map and use different `EmissiveStrength` values in their sections.

```ini
[my_car_tail_lamp_m]
Mode=PBR
AlphaMode=Opaque
BaseColor=tail_base.tga
Roughness=0.28
Metallic=0.0
EmissiveMap=tail_emission.tga
EmissiveStrength=3.0
```

`EmissiveStrength` is the per-material power multiplier from `0` to `16`. At `1.0`, the shader uses the RGB map at its normal strength. At `3.0`, it triples the light in linear space. Set it to `0` to switch off emission for that material. Use a period for decimals and restart the game after editing the INI. You can adjust the shipped demo in the same way with `EmissiveStrength` under `[PBRBounceEmitter_m]`.

Check **Linear HDR** and **Bloom** in **PBR Shaders → Mod Settings** to see the glow around a bright lamp. For a bounce test, turn Bloom off and enable **Screen-Space Global Illumination (SSGI)**. Bring a nearby opaque PBR surface into view. Turn on **SSGI on Opaque Level Scenery** if you want the light to tint stock scenery too. The lamp and the receiving surface need to be visible for the screen-space bounce.

## Set up windows and cutouts

- For a windshield with the usual game glass shading, use an alpha-blended Pure3D `spheremap` shader. Enable `HasTranslucency`, keep the glass texture's alpha, and bind the reflection texture to `REFL`. If your mod loads the level's `EnvMap.bmp`, you can use it there. A simple alpha-blended shader also works with an INI section set to `Mode=Glass`. `Reflection=0.55` is a starting point for the reflection gain.
- For a PBR lens or window, give it its own blend-enabled Pure3D shader with `HasTranslucency` set. Use `Mode=PBR`, `AlphaMode=Blend`, and a 32-bit base-color TGA with the opacity in alpha. `Opacity` multiplies that alpha. Blended surfaces use their environment reflections.
- For a grille or other hard-edged cutout, use `AlphaMode=Mask` with an alpha channel in the base color. `AlphaCutoff=0.5` keeps the pixels at or above half coverage. Keep the mesh in the opaque queue for this mode.

## If the panel still looks stock

- Check that the INI section uses the **Shader** chunk name, including any `_m` suffix. Check that its mesh has normals and UV0.
- Double-check the directory beside **Simpsons.exe** and the TGA export setting. For example, `NormalMap=body_normal.tga` points to `VehicleShaders/body_normal.tga`. A bad configured texture can send the material back to the stock draw.
- Turn off the Level 1 demo while testing your own replacement for the Family Sedan. Its path handler has its own windshield setup.
- Use the log to check for `Selected material`, then `Rendered opaque PBR material` or `Rendered translucent PBR material`. [Getting a log](LMLH-Development.md#playtest-and-debug)
