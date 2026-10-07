# Builds a PS1 game package in recomp mode on Windows: the game recompiled from the player's
# disc by N64Recomp (com.recomp.ps1's copy, PS1 mode), on the recomp runtime, as the library the
# com.recomp.ps1 addon links.
#
#   build_recomp.ps1 -Package <Packages\com.recomp.<game>> [-Disc <.bin/.cue/.iso or extracted folder>] [-Config RelWithDebInfo]
#   build_recomp.ps1 -Package <Packages\com.recomp.<game>> -Live [-Config RelWithDebInfo]
#
# The game package provides Recomp\: game.json (title, name, config, rom: the boot executable's
# file name and sha1, the disc image's usual name), the N64Recomp config it names, the symbols
# (syms.toml, psyq.txt, data_symbols.txt: tools/recomp/ps1_syms.py) and CMakeLists.txt calling
# ps1_recomp_game (Runtime/cmake/Ps1Recomp.cmake), and optionally mods.toml (the mods' code and
# hooks: tools/recomp/ps1_mods.py, Docs/Recomp.md). The disc: -Disc, else the package's
# Assets\game.json "disc", else the extracted Assets\Disc. Steps, each skipped when up to date:
#   1. the recompiler's input from the disc (ps1_rom.py: rom.bin, section_files.c) -> <game>\Native\build\recomp
#   2. N64Recomp.exe from the vendored source (ThirdParty\N64Recomp)           -> Runtime\build\n64recomp
#   3. the C: N64Recomp with the game's config                                  -> <game>\Native\build\recomp\funcs
#   4. the game library (the game's Recomp\CMakeLists.txt)                      -> <game>\Native\build\recomp-<Config>
#   5. published: Lib\Windows\<name>_recomp.lib, and Source\Guest\<name>_recomp\ - one file that
#      links it into the addon and registers the game with Ps1Player (it changes with the library,
#      so the editor relinks). The game's Decomp build (Source\Guest\<name>) is removed: one
#      module per game package (Build mode Decomp makes it again).
#   6. the disc extracted to the package's Assets\Disc when it isn't (Ps1Player reads it)
# The generated C and the library are game code made from your disc: they stay in build\ and
# Lib\, which git ignores. Called by the editor (PS1 Target Options, Build mode Recomp).
#
# -Live (Build mode Recomp (live)): no C is generated and no disc is needed to build. The
# library is the runtime with N64Recomp's LiveRecomp, which recompiles the game from the
# player's disc when it boots (about 0.2 s for Digimon World), so it holds no game code. The
# recompiler data (game.json, syms.toml) is copied to <game>\Assets\Recomp\Live, which ships
# with the project's builds (git-ignored: it is made from Recomp\).
param(
    [Parameter(Mandatory = $true)][string]$Package,
    [string]$Disc = '',
    [string]$Config = 'RelWithDebInfo',
    [switch]$Live,
    # The debug C runtime (/MDd), for an editor built in Debug: the addon is compiled with the
    # editor's runtime, and a library made for the other one does not link with it
    [switch]$DebugCrt
)
$ErrorActionPreference = 'Continue'
$ps1 = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$tools = Join-Path $ps1 'Runtime\tools'
$Package = (Resolve-Path $Package -ErrorAction Stop).Path
$recompDir = Join-Path $Package 'Recomp'
$outDir = Join-Path $Package 'Native\build\recomp'
$funcs = Join-Path $outDir 'funcs'
function Fwd([string]$p) { return $p -replace '\\', '/' }

$crtArg = if ($DebugCrt) { '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDebugDLL' } else { '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL' }
$crtSuffix = if ($DebugCrt) { '-dcrt' } else { '' }

$gameJson = Join-Path $recompDir 'game.json'
if (-not (Test-Path $gameJson)) { throw "$Package has no Recomp\game.json (see com.recomp.ps1's README, Recomp mode)" }
$game = Get-Content $gameJson -Raw | ConvertFrom-Json
$name = $game.name
if (-not $name -or $name -notmatch '^[A-Za-z_][A-Za-z0-9_]*$') { throw 'Recomp\game.json needs "name" (a C identifier, e.g. dw)' }
$toml = Join-Path $recompDir $(if ($game.config) { $game.config } else { 'recomp.toml' })
if (-not (Test-Path $toml)) { throw "Recomp\game.json names $(Split-Path $toml -Leaf), which is not in Recomp\" }
$packageId = Split-Path $Package -Leaf
$python = 'python'

# The disc: -Disc, the package's Assets\game.json "disc", the extracted Assets\Disc
if (-not $Disc) {
    $assetsJson = Join-Path $Package 'Assets\game.json'
    if (Test-Path $assetsJson) {
        $assets = Get-Content $assetsJson -Raw | ConvertFrom-Json
        if ($assets.disc) {
            $candidate = if ([System.IO.Path]::IsPathRooted($assets.disc)) { $assets.disc } else { Join-Path $Package $assets.disc }
            if (Test-Path $candidate -PathType Leaf) { $Disc = (Resolve-Path $candidate).Path }
        }
    }
}
$extracted = Join-Path $Package 'Assets\Disc'
if (-not $Disc -and -not $Live -and (Test-Path (Join-Path $extracted $game.rom.file))) { $Disc = $extracted }

# Visual Studio's x64 environment, clang, CMake and Ninja (as the decomp build uses)
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -property installationPath
if (-not $vs) { throw 'Visual Studio (with the C++ workload and its Clang tools) is needed' }
if (-not $env:VSCMD_VER) {
    $vars = & "$env:SystemRoot\System32\cmd.exe" /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul 2>&1 && set"
    if (-not $vars) { throw 'could not import the Visual Studio environment (vcvars64.bat)' }
    foreach ($line in $vars) {
        if ($line -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
    }
}
$llvm = "$vs\VC\Tools\Llvm\x64\bin"
$cmake = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninja = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
foreach ($tool in @("$llvm\clang.exe", $cmake, $ninja)) {
    if (-not (Test-Path $tool)) { throw "missing $tool (install the Visual Studio C++ Clang tools and CMake)" }
}
$cmakeCommon = @('-G', 'Ninja', "-DCMAKE_MAKE_PROGRAM=$ninja", $crtArg, "-DCMAKE_C_COMPILER=$llvm\clang.exe",
    "-DCMAKE_CXX_COMPILER=$llvm\clang++.exe", "-DCMAKE_BUILD_TYPE=$Config")

# Publishes the library and the addon file that links it and registers the game
function Publish([string]$lib, [string]$mode, [string]$from) {
    $libDir = Join-Path $ps1 'Lib\Windows'
    New-Item -ItemType Directory -Force $libDir | Out-Null
    $target = Join-Path $libDir "${name}_recomp.lib"
    Copy-Item $lib $target -Force
    $hash = (Get-FileHash $target -Algorithm SHA1).Hash
    $module = "ps1w_module_${name}_recomp"
    $guestDir = Join-Path $ps1 "Source\Guest\${name}_recomp"
    New-Item -ItemType Directory -Force $guestDir | Out-Null
    $text = @"
// Generated by com.recomp.ps1's Runtime/tools/recomp/build_recomp.ps1 - do not edit.
// $($game.title), package "$packageId": $mode ($from).
// Library sha1 $hash (this file changes with it, so the editor relinks the addon).
// Recomp builds are Windows x64 only: other targets build the game in Decomp mode.
#if defined(_WIN32) && defined(_M_X64)
#include "../../Wasm/ps1w_module.h"

#pragma comment(lib, "$(Fwd $target)")
// The library's C++ (N64Recomp's thread_locals) needs the C runtime's TLS destructor support.
// The editor can hand a release addon a Debug Lua.lib, whose objects ask for the debug runtime
// too; the linker would then take that support from it and fail (_malloc_dbg, _free_dbg).
// Only the addon's own runtime is wanted.
#if defined(_DEBUG)
#pragma comment(linker, "/NODEFAULTLIB:msvcrt.lib")
#else
#pragma comment(linker, "/NODEFAULTLIB:msvcrtd.lib")
#endif

extern "C" const Ps1wModule $module;

namespace
{
struct Register
{
    Register() { ps1w_register_module(&$module); }
} sRegister;
}
#endif
"@
    $file = Join-Path $guestDir "${name}_recomp_guest_register.cpp"
    $old = if (Test-Path $file) { Get-Content $file -Raw } else { '' }
    if ($old -ne $text) { Set-Content -Path $file -Value $text -NoNewline }
    Set-Content -Path (Join-Path $guestDir 'mode.txt') -Value $mode -NoNewline
    # one module per game package: the Decomp build of the game leaves the addon
    $decompDir = Join-Path $ps1 "Source\Guest\$name"
    if (Test-Path $decompDir) {
        Remove-Item -LiteralPath $decompDir -Recurse -Force
        Write-Host "removed the Decomp build of $($game.title) from the addon (Source\Guest\$name; Build mode Decomp makes it again)"
    }
    Write-Host "published ${name}_recomp.lib ($mode, $from) and Source\Guest\${name}_recomp"
}

# Ps1Player plays the disc extracted into the package (Assets\Disc), as in Decomp mode
function ExtractDisc {
    if (Test-Path (Join-Path $extracted 'disc.idx')) { return }
    if (-not $Disc -or -not (Test-Path $Disc -PathType Leaf)) {
        Write-Host "the disc is not extracted to Assets\Disc: Pre Process Rom (Tools > Recomp) does it, or pass -Disc"
        return
    }
    & $python -u (Join-Path $tools 'extract_disc.py') $Disc $extracted
    if ($LASTEXITCODE -ne 0) { throw 'extracting the disc failed' }
}

if ($Live) {
    # 1. The library: the runtime and LiveRecomp, no game code
    $buildDir = Join-Path $Package "Native\build\recomp-live-$Config$crtSuffix"
    & $cmake -S $recompDir -B $buildDir @cmakeCommon -DPS1_RECOMP_LIVE=ON | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'cmake configure failed (see the game package''s Recomp\CMakeLists.txt)' }
    & $cmake --build $buildDir --target "${name}_recomp_all"
    if ($LASTEXITCODE -ne 0) { throw 'build failed' }
    $lib = Join-Path $buildDir "${name}_recomp_all.lib"

    # 2. The data the game recompiles the disc with, shipped in the package's Assets
    $dataDir = Join-Path $Package 'Assets\Recomp\Live'
    New-Item -ItemType Directory -Force $dataDir | Out-Null
    $shipped = @('game.json', 'syms.toml')
    foreach ($file in $shipped) {
        $source = Join-Path $recompDir $file
        if (-not (Test-Path $source)) { throw "Recomp\$file is missing (live mode ships it)" }
        Copy-Item $source (Join-Path $dataDir $file) -Force
    }
    Get-ChildItem $dataDir -Recurse -File | Where-Object { $shipped -notcontains $_.FullName.Substring($dataDir.Length + 1) } |
        ForEach-Object { Remove-Item -LiteralPath $_.FullName }
    $ignore = Join-Path $Package '.gitignore'
    $ignored = (Test-Path $ignore) -and (Select-String -Path $ignore -SimpleMatch '/Assets/Recomp/Live/' -Quiet)
    if (-not $ignored) {
        Add-Content -Path $ignore -Value "`n# Recomp (live) data, copied from Recomp/ by com.recomp.ps1's build_recomp.ps1 -Live`n/Assets/Recomp/Live/"
    }

    # 3. Publish
    Publish $lib 'recomp-live' "the runtime and LiveRecomp; recompiler data in Assets\Recomp\Live"
    ExtractDisc
    exit 0
}

# 1. The recompiler's input from the disc
if (-not $Disc) {
    throw "no disc: pass -Disc, set ""disc"" in the package's Assets\game.json, or extract it to Assets\Disc (Pre Process Rom)"
}
New-Item -ItemType Directory -Force $funcs | Out-Null
$syms = Join-Path $recompDir 'syms.toml'
$sha1Arg = if ($game.rom -and $game.rom.sha1) { @('--sha1', $game.rom.sha1) } else { @() }
& $python -u (Join-Path $tools 'recomp\ps1_rom.py') --syms $syms --disc $Disc --out $outDir @sha1Arg
if ($LASTEXITCODE -ne 0) { throw "this disc is not the one $($game.title)'s symbols describe, or it could not be read ($Disc)" }

# 2. N64Recomp
$rcBuild = Join-Path $ps1 'Runtime\build\n64recomp'
$rcExe = Join-Path $rcBuild 'N64Recomp.exe'
if (-not (Test-Path (Join-Path $rcBuild 'CMakeCache.txt'))) {
    Write-Host 'building N64Recomp'
    & $cmake -S (Join-Path $ps1 'ThirdParty\N64Recomp') -B $rcBuild -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" `
        "-DCMAKE_C_COMPILER=$llvm\clang-cl.exe" "-DCMAKE_CXX_COMPILER=$llvm\clang-cl.exe" -DCMAKE_BUILD_TYPE=Release | Out-Null
}
# (up to date in a moment when ThirdParty\N64Recomp didn't change)
$rcLog = & $cmake --build $rcBuild --target N64RecompCLI 2>&1
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $rcExe)) { $rcLog | Write-Host; throw 'building N64Recomp failed' }

# 3. The C: the game's config with its paths made absolute, this rom and this output folder
$rom = Join-Path $outDir 'rom.bin'
$lines = foreach ($line in Get-Content $toml) {
    if ($line -match '^\s*(\w+_path)\s*=\s*"(.*)"\s*$') {
        $key = $Matches[1]; $value = $Matches[2]
        if ($key -eq 'output_func_path') { $value = Fwd $funcs }
        elseif ($key -eq 'rom_file_path') { $value = Fwd $rom }
        elseif (-not [System.IO.Path]::IsPathRooted($value)) { $value = Fwd ([System.IO.Path]::GetFullPath((Join-Path $recompDir $value))) }
        "$key = `"$value`""
    } else { $line }
}
$genToml = Join-Path $outDir 'recomp.gen.toml'
$text = ($lines -join "`n") + "`n"
# the mods' hooks (Recomp\mods.toml) go into the generated code
$modsToml = Join-Path $recompDir 'mods.toml'
if (Test-Path $modsToml) {
    $hooksFile = Join-Path $outDir 'mods.hooks.toml'
    & $python -u (Join-Path $tools 'recomp\ps1_mods.py') hooks $modsToml $syms $hooksFile
    if ($LASTEXITCODE -ne 0) { throw 'Recomp\mods.toml has errors (see above)' }
    $text += (Get-Content $hooksFile -Raw)
}
if (-not (Test-Path $genToml) -or (Get-Content $genToml -Raw) -ne $text) { Set-Content -Path $genToml -Value $text -NoNewline }
$stamp = Join-Path $outDir 'funcs.stamp'
$inputs = @($genToml, $rom, $rcExe, $syms)
$stale = -not (Test-Path $stamp) -or -not (Test-Path (Join-Path $funcs 'funcs.h'))
if (-not $stale) {
    $made = (Get-Item $stamp).LastWriteTimeUtc
    $stale = @($inputs | Where-Object { (Get-Item $_).LastWriteTimeUtc -gt $made }).Count -gt 0
}
if ($stale) {
    Write-Host "recompiling $($game.title) with $(Split-Path $toml -Leaf)"
    Remove-Item $stamp -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $funcs 'funcs_*.c') -ErrorAction SilentlyContinue
    Push-Location $outDir
    & $rcExe $genToml | Where-Object { $_ -notmatch '^\[Info\]' }
    $code = $LASTEXITCODE
    Pop-Location
    if ($code -ne 0) { throw "N64Recomp failed ($code)" }
    Set-Content -Path $stamp -Value (Get-Date -Format o)
}

# 4. The game library
$buildDir = Join-Path $Package "Native\build\recomp-$Config$crtSuffix"
& $cmake -S $recompDir -B $buildDir @cmakeCommon "-DPS1RECOMP_GENERATED=$(Fwd $funcs)" `
    "-DPS1RECOMP_SECTION_FILES=$(Fwd (Join-Path $outDir 'section_files.c'))" | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'cmake configure failed (see the game package''s Recomp\CMakeLists.txt)' }
& $cmake --build $buildDir
if ($LASTEXITCODE -ne 0) { throw 'build failed' }

# 5, 6. Publish
Publish (Join-Path $buildDir "${name}_recomp_game.lib") 'recomp' "from $(Split-Path $Disc -Leaf)"
ExtractDisc
