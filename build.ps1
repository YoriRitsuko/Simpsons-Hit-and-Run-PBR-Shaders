param(
    [string] $Compiler = 'i686-w64-mingw32-clang++',
    [string] $ShaderCompiler,
    [string] $ResourceCompiler
)

$ErrorActionPreference = 'Stop'
$Compiler = (Get-Command -Name $Compiler -CommandType Application).Source
if (!$ResourceCompiler) {
    $ResourceCompiler = Join-Path (Split-Path $Compiler) 'i686-w64-mingw32-windres.exe'
}
$ResourceCompiler = (Get-Command -Name $ResourceCompiler -CommandType Application).Source
if (!$ShaderCompiler) {
    $sdk = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10/bin'
    $ShaderCompiler = Get-ChildItem -LiteralPath $sdk -Directory |
        Where-Object Name -Match '^\d+(\.\d+)+$' |
        Sort-Object { [version] $_.Name } -Descending |
        ForEach-Object { Join-Path $_.FullName 'x64/fxc.exe' } |
        Where-Object { Test-Path -LiteralPath $_ } |
        Select-Object -First 1
}
if (!$ShaderCompiler) { throw 'Pass the Windows SDK fxc.exe path as -ShaderCompiler.' }

$intermediate = Join-Path $env:TEMP ('shar-pbr-shaders-' + [Guid]::NewGuid().ToString('N'))
New-Item -Path $intermediate -ItemType Directory | Out-Null
$hlsl = Join-Path $PSScriptRoot 'effects.hlsl'
$profiles = @{
    GlassVertex = 'vs_3_0'
    PaintVertex = 'vs_3_0'
    PbrVertex = 'vs_3_0'
    GlassPixel = 'ps_3_0'
    PaintPixel = 'ps_3_0'
    PbrPixel = 'ps_3_0'
    PbrReflectionPixel = 'ps_3_0'
}
$postProfiles = @{
    AcesComposite = 'ps_3_0'
    NeutralComposite = 'ps_3_0'
    BloomPrefilter = 'ps_3_0'
    BloomDownsample = 'ps_3_0'
    BloomConvolution = 'ps_3_0'
    BloomStreak = 'ps_3_0'
    ScreenReflection = 'ps_3_0'
    ScreenIrradiance = 'ps_3_0'
    StaticIrradianceShape = 'ps_3_0'
    FilterIrradiance = 'ps_3_0'
    IndirectComposite = 'ps_3_0'
    HorizonOcclusion = 'ps_3_0'
    FilterOcclusion = 'ps_3_0'
    SkyBrightness = 'ps_3_0'
    DepthOfFieldSource = 'ps_3_0'
    DepthOfField = 'ps_3_0'
    HistogramMeter = 'ps_3_0'
    HistogramRows = 'ps_3_0'
    HistogramCurve = 'ps_3_0'
    ApplyHistogram = 'ps_3_0'
}
$release = Join-Path $PSScriptRoot 'Release'
$out = Join-Path $release 'PBR Shaders.lmlh'
New-Item -Path $release -ItemType Directory -Force | Out-Null
Push-Location -LiteralPath $PSScriptRoot
try {
    foreach ($entry in $profiles.Keys) {
        $include = Join-Path $intermediate ($entry + '.inc')
        & $ShaderCompiler /nologo /WX /T $profiles[$entry] /E $entry /Fh $include /Vn ($entry + 'Code') $hlsl
        if ($LASTEXITCODE -ne 0) { throw "Compilation failed for $entry." }
    }
    foreach ($entry in $postProfiles.Keys) {
        $include = Join-Path $intermediate ($entry + '.inc')
        & $ShaderCompiler /nologo /WX /T $postProfiles[$entry] /E $entry /Fh $include /Vn ($entry + 'Code') 'postfx.hlsl'
        if ($LASTEXITCODE -ne 0) { throw "Compilation failed for $entry." }
    }
    $resource = Join-Path $intermediate 'hack.o'
    & $ResourceCompiler -i 'hack.rc' -o $resource -O coff
    if ($LASTEXITCODE -ne 0) { throw "Resource build failed with exit code $LASTEXITCODE." }
    & $Compiler -std=c++17 -m32 -O2 -Wall -Wextra -Werror -shared -static -I $intermediate renderer.cpp materials.cpp postfx.cpp hooks.S $resource -lshell32 '-Wl,--dynamicbase,--nxcompat,--strip-all' -o $out
    if ($LASTEXITCODE -ne 0) { throw "Hack build failed with exit code $LASTEXITCODE." }
} finally {
    Pop-Location
    Get-ChildItem -LiteralPath $intermediate -File | Remove-Item
    Remove-Item -LiteralPath $intermediate
}
Get-Item -LiteralPath $out
