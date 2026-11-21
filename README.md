# PBR Shaders

Glass can pick up a spheremap reflection. Rigid meshes can use forward PBR
with GGX highlights, roughness-filtered HDR reflections and RGB emission.
The linear HDR world pass adds selectable tone mapping, convolution bloom
with horizontal streaks, vignette, opaque screen-space reflections,
screen-space global illumination and ambient occlusion. Depth of field and
luminance histogram equalization are optional.

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

Right-click **PBR Shaders** in the **Settings** tab and open **Mod Settings**.
Linear HDR defaults to **Khronos PBR Neutral** with bloom, vignette, SSR,
SSGI including opaque scenery, and ambient occlusion enabled. The
**Tone Mapper** setting also offers **ACES 1.3**. Restart the game after
changing these options.
Turn off the launcher's MSAA setting when using HDR. SSR and SSGI also
require an INTZ depth texture and four Direct3D 9 render targets.

**HDR Exposure (%)** adjusts the whole gameplay scene before the selected
tone mapper, including the stock scenery, cars, and characters. Use it
to lift ACES midtones while preserving the display transform. **Sky
Brightness (%)** adds a separate gain to the existing sky, before the
level geometry draws. Both controls range from 25% to 400%. Exposure
defaults to 200% and sky brightness to 300%. **PBR Direct Light Strength**
and **Environment Strength** default to 200% and 70%.

The world is rendered into a floating-point target. SSR uses the completed
world's color and depth, including characters and cars drawn near the end
of the world pass. It replaces part of the spheremap contribution on
opaque PBR materials. **SSR Ray Steps** defaults to 64 (16–128). **SSR
Distance (World Units)** defaults to 64 (8–160).

Bloom extracts highlights before HDR exposure, then convolves them at four
resolutions from half to sixteenth scale for a soft halo. Bright outliers
roll off smoothly before they feed the halo and the wider horizontal streak.
PBR also filters glossy highlights and environment reflections across the
pixel footprint to reduce shimmer on polished surfaces.
**Bloom Strength (%)** controls how much highlight light scatters into bloom
and defaults to 40. **Bloom Streak Mix (%)** controls how much of that bloom
goes into the streak and defaults to 30. Both range from 0 to 100.
Set the streak mix to 0 for a halo alone.

**Vignette** dims the gameplay image toward the corners with a soft,
aspect-correct falloff. **Vignette Strength (%)** defaults to 65
and ranges from 0 to 100. Both bloom and vignette apply before tone mapping.
The HUD draws after the HDR composite.

SSGI adds one diffuse bounce from nearby visible surfaces. Opaque PBR
materials receive it when SSGI is enabled. **SSGI on Opaque Level Scenery**
also includes stock scenery such as roads and buildings. It costs more GPU
time because it traces over more of the frame.

Under **Screen-Space Global Illumination** in Mod Settings, **SSGI Rays per
Pixel** defaults to 16 (6–32). **SSGI Distance (World Units)** defaults to
8 (1–12), and **SSGI Strength (%)** defaults to 120 (0–200). The rays are
traced at half resolution and filtered using depth and normals. Emissive
and sunlit surfaces can both contribute with bloom and SSR disabled.

### Ambient occlusion

Enable **Ambient Occlusion (GTAO)** in **World Rendering** with Linear HDR
enabled. It shades nearby contacts on the opaque world, including scenery,
characters, and vehicles. The pass traces both horizons on each slice of
the depth buffer, then filters and upsamples with depth and surface normals.
It works independently of SSGI and SSR and requires INTZ depth support.

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

### Camera effects

Both effects start off and require Linear HDR. Enable them in **Camera
Effects** in Mod Settings and restart the game.

- **Depth of Field** uses the completed world's INTZ depth to blur objects
  in front of or behind the focal plane. **Focus Distance (World Units)**
  defaults to 8 (1–100). **Maximum Blur Radius** defaults to 12 pixels at
  1080p (2–24) and scales with the backbuffer height.
- **Histogram Equalization** remaps the tone-mapped world's luminance using
  a 128-bin cumulative histogram. The curve adapts over time and eases
  out on nearly uniform frames. **Histogram Equalization Strength (%)**
  defaults to 30 (0–100).

Both effects apply to the world before the HUD. **AO View → Occlusion Only**
shows the diagnostic image without either effect.

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
