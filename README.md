# PBR Shaders

Glass can pick up a spheremap reflection. Rigid meshes can use forward PBR
with GGX highlights, roughness-filtered HDR reflections and RGB emission.
The optional linear HDR world pass adds selectable tone mapping, bloom,
opaque screen-space reflections, screen-space global illumination and
ambient occlusion.

## Install

1. Put `PBR Shaders.lmlh` in `Hacks` next to the Lucas launcher.
2. Reload the mod list and tick **PBR Shaders** in **Settings**.
3. Right-click **PBR Shaders** and open **Mod Settings…** to choose one
   directional light or all scene lights and adjust the two PBR strengths.

If upgrading from Vehicle Shaders, replace `Vehicle Shaders.lmlh` and
`Vehicle Shader Demo.lmlm` with the renamed files. Keep your existing
`VehicleShaders` folder and INI. Saved lighting settings carry over.

Use Lucas Mod Launcher 1.27.1 and the English retail PC game. Lucas loads
Direct3D 9 as a dependency.

## Make your own materials

Start with one body panel, then add maps, glass, or a glowing lamp. The
tutorial includes ready-to-copy INI sections, ORM packing, and per-material
`EmissiveStrength` examples. [Custom car and prop tutorial](docs/Materials.md)

## Linear HDR and screen-space effects

Right-click **PBR Shaders** in the **Settings** tab, open **Mod Settings**,
and check **Linear HDR**. Choose **ACES 1.3** or **Khronos PBR Neutral** in
**Tone Mapper**. **Bloom** starts checked and **Opaque Screen-Space
Reflections** and **Screen-Space Global Illumination (SSGI)** have their own
checkboxes and start disabled. Restart the game after changing these options.
Turn off the launcher's MSAA setting when using HDR. SSR and SSGI also
require an INTZ depth texture and four Direct3D 9 render targets.

**HDR Exposure (%)** adjusts the whole gameplay scene before the selected
tone mapper, including the stock scenery, cars, and characters. Use it
to lift ACES midtones while preserving the display transform. **Sky
Brightness (%)** adds a separate gain to the existing sky, before the
level geometry draws. Both controls range from 25% to 400% and default
to 100%. **PBR Direct Light Strength (%)** controls directional lighting
on PBR materials.

The world is rendered into a floating-point target. SSR uses the completed
world's color and depth, including characters and cars drawn near the end
of the world pass. It replaces part of the spheremap contribution on
opaque PBR materials. **SSR Ray Steps** defaults to 64 (16–128). **SSR
Distance (World Units)** defaults to 64 (8–160). Bloom extracts bright
pixels at full resolution, then uses filtered lower-resolution copies for
wider glow. The selected tone mapper writes SDR sRGB before the HUD.

SSGI adds one diffuse bounce from nearby visible surfaces. Opaque PBR
materials receive it by default when SSGI is enabled. Check **SSGI on Opaque
Level Scenery** to include stock scenery such as roads and buildings. This
scope is opt-in and costs more GPU time because it traces over more of the frame.

Under **Screen-Space Global Illumination** in Mod Settings, **SSGI Rays per
Pixel** defaults to 16 (6–32). **SSGI Distance (World Units)** defaults to
3 (1–12), and **SSGI Strength (%)** defaults to 100 (0–200). The rays are
traced at half resolution and filtered using depth and normals. Emissive
and sunlit surfaces can both contribute with bloom and SSR disabled.

### Ambient occlusion

Enable **Ambient Occlusion (GTAO)** in **World Rendering** with Linear HDR
enabled. It shades nearby contacts on the opaque world, including scenery,
characters, and vehicles. The pass traces both horizons on each slice of
the depth buffer, then filters and upsamples with depth and surface normals.
It works independently of SSGI and SSR. It starts off and requires INTZ
depth support.

- **AO Quality** sets the sampling budget. High is the default and uses
  four slices and six samples per side at half resolution. Performance uses
  two slices and four samples, and Ultra uses six slices and eight samples.
- **AO Radius (%)** sets the reach. 100% is 1.2 world units. The range is 25–250%.
- **AO Strength (%)** sets the amount of contact shading. The default is
  100% and the range is 0–200%.
- **AO View → Occlusion Only** shows the raw filtered visibility in grayscale.
  Switch back to **Game** for the normal HDR composite.

The pass follows the horizon integration from
[Practical Realtime Strategies for Accurate Indirect Occlusion](https://research.activision.com/publications/archives/practical-real-time-strategies-for-accurate-indirect-occlusion).

## Level 1 demo

To try the Level 1 demo, put `PBR Shader Demo.lmlm` in `Mods` and the
package's `VehicleShaders` texture folder beside `Simpsons.exe`. Tick
**PBR Shader Demo** in **General**. The shiny material preview sits by the
Simpsons house. The Family Sedan uses PBR paint on its opaque body panels
and the stock EnvMap.bmp on its transparent windshield.

Enable SSGI and turn off Bloom to see the cyan emitter tint the matte white
panel beside the preview. Move the camera around the fixture to see how the
bounce changes as surfaces enter and leave the screen.

## Build

Run `./build.ps1` with x86 [LLVM-MinGW](https://github.com/mstorsjo/llvm-mingw)
and the Windows SDK `fxc.exe`. Use `-Compiler` or `-ShaderCompiler` to pass
explicit paths. The LMLH lands in `Release`.

The ACES LUT is embedded in the hack. To regenerate it, run
`python tools/bake-aces-lut.py` with OpenColorIO 2.4.2. The script uses
the [ACES 1.3 CG config](https://opencolorio.readthedocs.io/en/latest/configurations/aces_cg.html),
Reference Gamut Compression, and the SDR Video view for sRGB. The latter
retains its historical `ACES 1.0 - SDR Video` name in the config.

To rebuild the demo, put the LMLH in `Hacks` and the demo folder from `package`
in `Mods`. Reload the list, right-click the demo and hit **Compile**.

For the preview build, extract the original MaterialPreviewMesh ZIP and run
`node tools/build-preview.cjs <extracted-folder> <output-folder> <shiny-textures-folder>`
with Node 22 or newer. The last argument is the extracted folder from
`MaterialPreviewMeshShiny_Textures.zip`. Omit it to use the original maps.
Use the generated `VehicleShaders` folder for the textures and put the
generated `PBRPreview.p3d` in the demo's `Resources` folder.

See the [modder's guide](Mod%20Guide.txt) for the full settings reference.
For the native hack layout, launcher commands, and test workflow, see the
[LMLH development notes](docs/LMLH-Development.md).

The [MIT license](LICENSE) covers the code.
