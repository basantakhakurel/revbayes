<#
.SYNOPSIS
  Deploy RevStudio and the Qt runtime into a self-contained folder (GUI_Implementation_Note.md, section 10.5).

.DESCRIPTION
  Copies RevStudio.exe, runs windeployqt (Qt DLLs and plugins), adds the app-local MSVC runtime, and copies the
  licence files. The result runs on a machine without Qt or Visual Studio.

  STATUS: written in phase 0 and NOT yet run on Windows (no Windows machine or PowerShell was available). The first
  run of gui-build.yml is its first test.

.EXAMPLE
  ./gui/packaging/windows/make-folder.ps1 -Build gui/build/ci-windows-release -Out dist/RevStudio
#>
param(
  [Parameter(Mandatory)] [string] $Build,     # build directory containing RevStudio.exe
  [Parameter(Mandatory)] [string] $Out        # folder to create (replaced if it exists)
)
$ErrorActionPreference = "Stop"

$guiDir = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
$repoDir = (Resolve-Path (Join-Path $guiDir "..")).Path
$exe = (Resolve-Path (Join-Path $Build "RevStudio.exe")).Path

if (Test-Path $Out) { Remove-Item $Out -Recurse -Force }
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
Copy-Item $exe $Out

# 1. Qt runtime and plugins. windeployqt is on PATH (install-qt-action adds Qt's bin directory).
#    The flags were checked against `windeployqt --help` of Qt 6.8.3 (run under wine). By default windeployqt also
#    deploys the OpenGL software rasterizer and the D3D/DXC compilers, which a Widgets-only application does not need.
& windeployqt --release --no-translations --no-opengl-sw --no-system-d3d-compiler --no-system-dxc-compiler `
              --dir $Out (Join-Path $Out "RevStudio.exe")
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed (exit code $LASTEXITCODE)" }

# 2. App-local MSVC runtime (Microsoft allows redistributing these DLLs with an application).
#    VCToolsRedistDir is set by ilammy/msvc-dev-cmd and by the Visual Studio developer prompts.
$redist = $env:VCToolsRedistDir
if (-not $redist) {
  $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio/Installer/vswhere.exe"
  $vs = & $vswhere -latest -products * -property installationPath
  $redist = (Get-ChildItem (Join-Path $vs "VC/Redist/MSVC") -Directory | Sort-Object Name -Descending | Select-Object -First 1).FullName
}
$crt = Get-ChildItem -Path (Join-Path $redist "x64") -Directory -Filter "Microsoft.VC*.CRT" | Select-Object -First 1
if (-not $crt) { throw "could not find the MSVC runtime under $redist" }
Copy-Item (Join-Path $crt.FullName "*.dll") $Out

# 3. Licence files
Copy-Item (Join-Path $repoDir "LICENSE") $Out
Copy-Item (Join-Path $guiDir "packaging/THIRD_PARTY_NOTICES.txt") $Out

Write-Host "Deployed folder: $Out"
Get-ChildItem $Out | Select-Object Name, Length | Format-Table -AutoSize | Out-String | Write-Host
