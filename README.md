# PBR Shaders

Give a car real paint and glass, add a room behind a shop window, or use
the HDR effects on their own. The mod is for the English retail PC game
with Lucas Mod Launcher 1.27.1.

## Install it

1. Put `PBR Shaders.lmlh` in the launcher's `Hacks` folder.
2. Reload the list and enable **PBR Shaders** under **Settings**.
3. To try the demo, put `PBR Shader Demo.lmlm` in `Mods` and enable
   **PBR Shader Demo** under **General**.

Start Level 1 and look around the Simpsons house. There's a chrome ball,
a cyan light beside a white panel, and a window you can walk past to see
the room shift behind the glass. The demo also gives the Nuclear Waste
Truck new bodywork, wheels, and glass while keeping its original cabin
material. Its maps are already in the demo's Pure3D files.

Updating from an older build? Replace both packages if you use the demo.
`VehicleShaders` was the car-only name. Lucas still uses it behind the
scenes so it can find your settings and existing mod dependencies.
Older custom materials can still use their loose files.

## Set up the picture

Right-click **PBR Shaders** and open **Mod Settings**. Restart the game
after changing settings. Turn off Lucas's MSAA if you're using Linear HDR.

- **Tone Mapper** defaults to Khronos PBR Neutral. ACES 1.3 is there if
  you prefer its look. **HDR Exposure** changes the whole scene. **Sky
  Brightness** changes the sky before buildings and cars draw over it.
- **Bloom Strength** controls the light around bright spots. Lower
  **Bloom Streak Mix** for a softer glow. At 0%, you get the halo alone.
- **Depth of Field** blurs the world beyond the focus range. Try a focus
  of 4 and a sharp range of 5 for a closer third-person look. Leave
  **Blur Foreground** off if you want the player and nearby props sharp.
- **Histogram Equalization** gradually spreads the tone-mapped luminance
  across the available range. Lower its strength for a subtler change.
- **SSR** adds reflections from visible objects. **SSGI** adds a diffuse
  bounce. **Ambient Occlusion** adds contact shading. These effects use
  the game's depth buffer. SSR and SSGI also need four render targets.

The bundled preset uses 200% exposure, 300% sky brightness, 40% Bloom
Strength, 30% Streak Mix, and 65% Vignette. Depth of field starts at
90 world units. Histogram strength starts at 35%.

## Make your own materials

You can start with one car panel and a base color map. The
[material guide](docs/Materials.md) walks through the Windows texture
packer, how to include the output in a mod, and how to set up a window.
Your players get the finished `.lmlm` and enable it in Lucas.

## Build it

Run `./build.ps1` with [x86 LLVM-MinGW](https://github.com/mstorsjo/llvm-mingw)
and the Windows SDK's `fxc.exe`. The result goes in `Release`.
Use `-Compiler` and `-ShaderCompiler` if the tools live somewhere else.

The editable demo is under `package`. To compile it, put the hack in
Lucas's `Hacks` folder, copy the demo source folder into `Mods`, reload the
list, and right-click the demo to choose **Compile**. The checked-in Pure3D
files already include the demo maps.

See [development notes](docs/LMLH-Development.md) for rebuilding the demo
assets, testing the native hack, and packaging a release.

The code is under the [MIT license](LICENSE).
