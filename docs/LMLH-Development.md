# Build and test the launcher hack

The working setup is Windows, LLVM-MinGW 20260324 targeting x86, and
`fxc.exe` from Windows SDK 10.0.26100.0. Keep a separate copy of the game and
of Lucas Mod Launcher **1.27.1** for tests. Copy the launcher's framework
files under `Mods` too so command-line builds can find them. The English
retail executable used for tests has this SHA-256.

```text
2928118887B910AE03CE714E287D9BC67FB95F29E1B4F1271B8AF389990C0795
```

The native hook addresses refer to that retail executable. The older Visual Studio source tree and map files help identify functions by name and behavior. Confirm addresses and expected bytes against retail before using them. `llvm-objdump -d` with `--start-address` and `--stop-address` is enough to check the small regions around most of the hooks.

## What Lucas loads

| File | Format and destination |
| --- | --- |
| `PBR Shaders.lmlh` | A 32-bit Windows DLL with launcher metadata in the `META.INI` resource. Put it in `Hacks` beside Lucas. |
| `PBR Shader Demo.lmlm` | A compiled launcher mod. Put it in `Mods` and enable it in **General**. The folder under `package` is its editable source. |
| `PBRMaterials.p3d` and `PBRSedanMaterials.p3d` | The demo's packed maps and per-material settings. The Lua handlers attach them before the shaders that use them. Both ship inside the LMLM. |

`HackMeta.ini` is the hack's metadata source. `hack.rc` embeds it, the license, and the ACES LUT. The launcher calls the exported `HackEntryPoint` at install-hacks event `1`. The x86 build has to export the name `HackEntryPoint`, which you can check with `llvm-objdump -p`.

`VehicleShaders` was the car-only name. Keep it as the internal ID so
Lucas finds the same dependencies and saved settings. The launcher displays
**PBR Shaders** under **Settings**. The demo appears under **General** and
uses Custom Files to load its assets from the LMLM. The old command-line
switch and loose material folder are still handled for legacy mods.

`Title` is the display name. `Name` in each `[Setting]` section is the saved key. When adding a control, update the metadata and its native reader together. The native settings lookup in `ReadPbrSettings` is specific to the tested `Hacks.dll`. The code checks the PE timestamp and image size, checks the address behind export ordinal `2560`, and reads tickboxes, integer values, and multiple-choice indices at offset `0x188`. It falls back to the C++ defaults if those checks fail. When porting to a new launcher, verify that lookup and its value layout before trusting the UI state. [Launcher mod settings reference](https://docs.donutteam.com/docs/LucasSimpsonsHitAndRunModLauncher/Mods/ConfiguringMods)

## Build and package

1. From the Git checkout, run `./build.ps1` with `i686-w64-mingw32-clang++.exe` on `PATH`, or pass `-Compiler` with its full path. The script looks for x86 `windres` beside the compiler and 64-bit `fxc.exe` in the Windows SDK. Override either path with `-ResourceCompiler` or `-ShaderCompiler` if needed.
2. `fxc` compiles the Shader Model 3 shaders into temporary headers. The compiler links those headers with the native code and `hack.rc` into `Release/PBR Shaders.lmlh`. Add new shader entry points to the profile list in `build.ps1`. The ACES LUT is already in `resources`, so a normal build needs no LUT baker or demo input archives.
3. Copy the built hack to the test launcher's `Hacks` folder. Copy `package/PBR Shader Demo` as a folder under its `Mods` folder. Reload and right-click **PBR Shader Demo → Compile**, or run the command below. The output goes into the directory passed to `-outputpath`. [Launcher command line](https://docs.donutteam.com/docs/LucasSimpsonsHitAndRunModLauncher/CommandLineArguments#mod-compilation)

```powershell
$launcher = 'C:\SHAR-Test\LucasModLauncher\Lucas Simpsons Hit & Run Mod Launcher.exe'
$output = Join-Path (Get-Location) 'Release'
Start-Process -FilePath $launcher -WorkingDirectory (Split-Path $launcher) -Wait `
    -ArgumentList ('-appid PBRShadersDev -compile "PBR Shader Demo" -outputpath "{0}"' -f $output)
```

The demo uses Custom Files path handlers to append the original preview mesh to `art/l1z1.p3d` and add a reflection parameter to the stock Family Sedan windshield. The Lua reads those assets from the player's installation. Use the stock Level 1 zone and sedan to test those handlers. `Debug Custom Files Output` can help inspect their in-memory output. [Path handlers](https://docs.donutteam.com/docs/LucasSimpsonsHitAndRunModLauncher/Hacks/CustomFiles/PathHandlerScripts) · [Debug Custom Files Output](https://docs.donutteam.com/docs/LucasSimpsonsHitAndRunModLauncher/Hacks/DebugCustomFilesOutput)

You can compile the demo straight from the checkout. To rebuild its preview
mesh and material packages, extract `MaterialPreviewMesh.zip` and
`MaterialPreviewMeshShiny_Textures.zip`. Run
`node tools/build-preview.cjs <preview-folder> <output-folder> <shiny-textures-folder>`
with Node 22 or newer. Copy the resulting preview model and both material
packages into the demo's `Resources`. The output's `AuthoringMaps` folder
is scratch space for intermediate maps.

If you already have the preview maps, use
`node tools/build-preview.cjs --pack-demo <output-folder> <maps-folder>`.
Edit `tools/demo-materials.ini` and `tools/demo-sedan.ini` to change the
demo settings. To change the interior fixture, generate it with
`node tools/build-preview.cjs --interior <output-folder>`, update the
preview room maps and model, then run `--pack-demo` again.

To rebuild the Nuclear Waste Truck, extract `NuTruck_Remaster.zip` and run
`node tools/build-nuctruck.cjs <extracted-folder> "package/PBR Shader Demo/Resources/NuTruckRemaster.p3d"`.
The launcher combines this file with the retail `art/cars/nuctruck.p3d`
when the truck loads. It swaps the three meshes and centers the rear axle
in the new wheel arches. The handler moves the tail lamp sprites to the
new lenses and applies both adjustments to the rest pose and the stock
pose animation. The barrels use the baked emissive map with the stock
glow sprites detached. The cabin uses the original swatches.

The player package needs `Hacks/PBR Shaders.lmlh`, the optional
`Mods/PBR Shader Demo.lmlm`, and the guide. The GUI and CLI packers are
authoring tools in the source checkout. Keep `Release/`, launcher settings,
and retail game files out of Git. Source files use LF because `windres`
embeds the metadata and license bytes verbatim.

To check a release, extract both ZIPs, verify the bundled DLL and demo hashes against the tested copies, then rebuild the LMLH from the extracted source on the same toolchain. When comparing rebuilt PE files, the four-byte COFF timestamp is at `e_lfanew + 8`, where `e_lfanew` is the little-endian value at file offset `0x3c`. Normalize just those four bytes for a rebuild comparison.

## Playtest and debug

Choose the stock game executable in the test launcher. Enable **No Introduction Movies** to skip movie playback. The launcher log or game command line should show `SKIPMOVIE`. Level loads can still take a while. Use one isolated launcher profile and one game process so each test has a clear configuration and log.

```powershell
$env:SHAR_SHADER_LOG = Join-Path $env:TEMP 'pbr-shaders-playtest.log'
Start-Process -FilePath $launcher -WorkingDirectory (Split-Path $launcher) `
    -ArgumentList '-appid PBRShadersDev -launch -ignoreenabledmods -enabledmod "PBR Shaders" -enabledmod "PBR Shader Demo" -enabledmod NoIntroductionMovies'
```

`-enabledmod` takes the name of the file or folder in the launcher, without the extension. `RequiredHack` and the settings section use the internal name. The log path must be absolute and writable. `SHAR_SHADER_LOG` is inherited from the launching shell, and the log appends across runs. Use a fresh filename when comparing configurations. [Launcher command line](https://docs.donutteam.com/docs/LucasSimpsonsHitAndRunModLauncher/CommandLineArguments#enabled-mods--hacks)

| Check | What to look for |
| --- | --- |
| Hack missing from the launcher | Use the launcher's `Hacks` location, check the embedded metadata, reload the list, and look under **Settings**. The compiled demo is under **General**. |
| Material setup | Look for `Loaded material settings and maps from a Pure3D package` before the model's `Selected material` messages. Try the compiled demo without a loose material folder. |
| Glass and the opaque PBR path | Look through the Family Sedan windshield while walking around the car. The sky reflection moves with the camera. Check the shiny demo body and lens as separate materials. |
| Linear HDR | Disable Lucas MSAA. Try both tone mappers. Test HDR and sky brightness with the stock world before stacking the screen-space passes. |
| Bloom | Enable HDR and Bloom and test the demo with an emissive material. Tune `EmissiveStrength` to check the soft knee. Compare Streak Mix at 0%, 15%, and 100% to isolate the halo and horizontal streak. Pan past the chrome preview and watch narrow highlights as they move across the screen. |
| Vignette | Toggle it with Bloom disabled and compare the center and corners at two aspect ratios. Check both tone mappers and confirm that the HUD and AO-only view stay clear. |
| Depth of field | Check distant roofs against the sky with the default preset. Their silhouettes should soften along with their detail. Set focus to 4 units for a closer test and verify the player stays sharp with a 5-unit sharp range. |
| Histogram equalization | Compare low-contrast and high-contrast scenes with the effect enabled at 30%. Check that the curve adapts smoothly between views and that a flat frame keeps its original luminance. |
| Interior mapping | Walk past the framed demo window beside the chrome preview. The room should shift with the view, the window frame should stay fixed, and the glass reflection should grow at grazing angles. |
| SSR | Enable HDR and SSR. Check the shiny sample with nearby characters in view. It traces from the final world depth. Test roughness and partial coverage near glass. |
| SSGI | Enable SSGI with Bloom off. Look for blue bounce on the white receiver near the cyan emitter. Toggle **SSGI on Opaque Level Scenery** to compare the stock receiver scope. |
| GTAO | Use **AO View → Occlusion Only** to inspect contact shading and halos. Switch to **Game** to check the composite with scene color. |

Separating the GPU test from the full level load helped when tuning the effects. A D3D9 test can use a tiny synthetic depth/color scene and run the exact `fxc` shader entry points into render targets for readback. Useful cases include a stock wall lighting a stock floor, an occluder in front of the receiver, a flat plane next to an AO corner, and a known color patch through the real ACES LUT. Use Level 1 afterward to verify hooks, game render state, sky, and HUD.

## Where the rendering lives

- `materials.cpp` reads the `PBRD` chunk when the retail Pure3D loader
  begins a top-level chunk. The stock loader skips that chunk afterward.
  The packer puts a version, content ID, named texture bytes, and shader
  settings in it. Keep the pack before the model's shader chunks. The
  older hooks in `hooks.S` still match shader names and drop pointers when
  the game destroys a shader.
- `renderer.cpp` follows the Direct3D 9 factory import in Lucas's `Direct3D9.dll`, then wraps the D3D9 draw calls. This gives the material effects the game's bound textures, streams, lights, and transforms. Stock calls remain the fallback when a custom material's data or shader setup is unavailable. Keep device state restoration around partial failures, the glass second pass, and the depth-only scenery replay.
- `effects.hlsl` handles vehicle glass, legacy paint, and the rigid PBR draw. Base color and emission decode from sRGB. Normal and ORM inputs stay linear. Keep the active stock albedo binding available so the original vehicle damage textures can swap in. The PBR directional and environment sliders apply to the PBR lighting terms. World exposure is taken later from the finished scene.
- Interior materials draw the façade through PBR. The camera ray hits a
  virtual room box, which gives the mirror-ball room map its parallax.
  The demo loads `PBRInterior.p3d` after `PBRMaterials.p3d`, reusing the
  chrome preview's environment for the window reflection.
- `postfx.cpp` owns the FP16 world target and post-processing. The world-layer hook returns the original render target and depth surface before the GUI runs. The sky pass scales the early sky draw inside its viewport, before the level renders over it. Bloom thresholds source texels and rolls off bright outliers before reducing resolution, then builds its halo with a normalized two-pass convolution at each level. The horizontal streak uses a vertical convolution on the quarter-resolution highlights for a softer profile. The composite scatters a portion of the filtered highlights into these kernels, then applies vignette and HDR exposure before ACES or Neutral tone mapping.
- `postfx.hlsl` and `ao.hlsli` share the final world depth where required. SSR and SSGI need four simultaneous render targets and readable INTZ depth. PBR writes octahedral view normals, roughness, depth, albedo, metallic, and the environment specular fallback. GTAO can run with INTZ alone and reconstructs its geometric normals from depth. Leave the sky and HUD out of the AO shading.
- `dof.hlsli` packs the circle of confusion from the completed INTZ depth alongside the HDR color, then gathers 32 matched color and depth samples. Background mode bypasses sharp pixels and rejects them from nearby blur samples. DOF runs before bloom. `histogram.hlsli` samples the tone-mapped image, reduces per-row cumulative counts into a 128-entry curve, and blends that curve over time on the GPU before remapping display luminance. Both passes are skipped in the AO-only view.
- Run the screen-space passes after the full world draw. Resolving at the world-translucency boundary missed characters and some vehicles. Trace SSGI from the original HDR scene before SSR and bloom. Its optional scenery pass marks actual static draws with a depth replay and tests them against final world depth before reconstruction.

Useful reference projects for the basic native hack layout are [WadSupport](https://github.com/EnAppelsin/WadSupport) and [FPCamInCar](https://github.com/Krolik9933/FPCamInCar). For the AO integration, see [Practical Realtime Strategies for Accurate Indirect Occlusion](https://research.activision.com/publications/archives/practical-real-time-strategies-for-accurate-indirect-occlusion).
