param(
    [string]$BuildDirectory = 'cmake-build-debug',
    [switch]$CleanFirst,
    [ValidateRange(1, 64)][int]$Jobs = 6
)

$ErrorActionPreference = 'Stop'
$rendererRoot = Split-Path $PSScriptRoot -Parent
$rendererBuild = [IO.Path]::GetFullPath((Join-Path $rendererRoot $BuildDirectory))
$rendererCache = Get-Content -LiteralPath (Join-Path $rendererBuild 'CMakeCache.txt') -Raw
$rendererCompiler = [regex]::Match($rendererCache, '(?m)^CMAKE_CXX_COMPILER:FILEPATH=(.+)$').Groups[1].Value.Trim()
$rendererVcRoot = [regex]::Match($rendererCompiler, '^(.+[/\\]VC)[/\\]Tools[/\\]MSVC[/\\]').Groups[1].Value
if (!$rendererVcRoot) { throw 'The build cache must use the MSVC compiler.' }
$rendererVcVars = Join-Path $rendererVcRoot 'Auxiliary/Build/vcvars64.bat'
if (!(Test-Path -LiteralPath $rendererVcVars)) { throw "Missing MSVC environment script: $rendererVcVars" }

# CMake writes the localized Ninja /showIncludes prefix in the Windows ANSI
# code page. A UTF-8 terminal otherwise makes cl.exe emit a different prefix,
# so Ninja silently records zero header dependencies and reuses stale objects.
if (!('RendererBuildCodePage' -as [type])) {
    Add-Type 'using System.Runtime.InteropServices; public static class RendererBuildCodePage { [DllImport("kernel32.dll")] public static extern uint GetACP(); }'
}
$rendererCodePage = [RendererBuildCodePage]::GetACP()
$rendererOriginalCodePage = [regex]::Match((& $env:ComSpec /d /c chcp | Out-String), '\d+').Value
$rendererClean = if ($CleanFirst) { ' --clean-first' } else { '' }
$rendererCommand = 'call "{0}" >nul && chcp {1} >nul && set "_CL_=/Z7" && cmake --build "{2}" --target engine --parallel {3}{4}' -f $rendererVcVars, $rendererCodePage, $rendererBuild, $Jobs, $rendererClean
try {
    & $env:ComSpec /d /c $rendererCommand
    $rendererBuildExitCode = $LASTEXITCODE
} finally {
    if ($rendererOriginalCodePage) { & $env:ComSpec /d /c "chcp $rendererOriginalCodePage >nul" }
}
if ($rendererBuildExitCode -ne 0) { throw "Renderer build failed with exit code $rendererBuildExitCode" }
