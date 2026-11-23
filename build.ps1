# Builds bin\RE6Tweaks.addon32 with MSVC (Visual Studio 2022+ with C++ tools).
# Resident Evil 6 (BH6.exe) is a 32-bit D3D9 game, so this is a 32-bit (x86) add-on for the
# 32-bit ReShade (d3d9.dll). MSVC is required: ReShade is built with it.
# Usage: .\build.ps1 [-Install] [-GameDir <folder of BH6.exe>]
#   -Install  copies the add-on next to the game's exe (game closed).
param(
    [switch]$Install,
    [string]$GameDir = "E:\SteamLibrary\steamapps\common\Resident Evil 6"
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$exe = "BH6.exe"

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
# vcvars32.bat runs vswhere by name, and its batch blocks break on some PATH entries
# (parentheses), so it gets a minimal PATH (this process only).
$env:PATH = "$(Split-Path $vswhere);$env:SystemRoot\system32;$env:SystemRoot;$env:SystemRoot\System32\WindowsPowerShell\v1.0"
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio with the C++ x86/x64 tools not found." }
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars32.bat"

$obj = "$root\bin\obj"
New-Item -ItemType Directory -Force $obj | Out-Null
$addon = "$root\bin\RE6Tweaks.addon32"

# ReShade add-on headers in third_party\reshade match ReShade 6.8.0.
$cmd = @(
    "call `"$vcvars`" >nul",
    ("cl /nologo /std:c++17 /O2 /EHa /MT /W3 /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX " +
     "/I `"$root\third_party\imgui`" /I `"$root\third_party\reshade\include`" " +
     "/Fo`"$obj\\`" `"$root\src\main.cpp`" user32.lib " +
     "/link /DLL /MACHINE:X86 /OUT:`"$addon`" /PDB:`"$obj\RE6Tweaks.pdb`" /DEBUG")
) -join " && "
cmd /c $cmd
if ($LASTEXITCODE) { throw "build failed" }
Write-Host "Built $addon"

if ($Install) {
    if (-not (Test-Path "$GameDir\$exe")) { throw "$exe not found in $GameDir (pass -GameDir)" }
    if (Get-Process ([IO.Path]::GetFileNameWithoutExtension($exe)) -ErrorAction SilentlyContinue) { throw "Close the game first." }
    Copy-Item $addon $GameDir -Force
    Write-Host "Installed to $GameDir"
}
