# Build snapx Windows installer with MSYS2 UCRT64 + Inno Setup.
# Run from MSYS2 UCRT64 or GitHub Actions windows-latest.
param(
    [string]$Version = "1.0.0"
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$BuildDir = Join-Path $Root "build"
$OutDir = Join-Path $Root "packaging\windows\output"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

Push-Location $Root
try {
    & bash "$Root/packaging/icons/generate-icons.sh"
} catch {
    Write-Warning "Icon generation skipped: $_"
}

cmake -B $BuildDir -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build $BuildDir

# Copy runtime DLLs from MSYS2 UCRT64 next to snapx.exe
$MsysBin = "C:\msys64\ucrt64\bin"
if (Test-Path $MsysBin) {
    $dlls = @("libgtk-4-1.dll", "libcairo-2.dll", "libgdk_pixbuf-2.0-0.dll",
              "libglib-2.0-0.dll", "libgobject-2.0-0.dll", "libgio-2.0-0.dll",
              "libpango-1.0-0.dll", "libharfbuzz-0.dll", "libfreetype-6.dll",
              "libpng16-16.dll", "libjpeg-8.dll", "zlib1.dll")
    foreach ($d in $dlls) {
        $src = Join-Path $MsysBin $d
        if (Test-Path $src) { Copy-Item $src $BuildDir -Force }
    }
    $gtkShare = "C:\msys64\ucrt64\share"
    if (Test-Path $gtkShare) {
        Copy-Item -Recurse (Join-Path $gtkShare "glib-2.0") (Join-Path $BuildDir "share\glib-2.0") -Force -ErrorAction SilentlyContinue
        Copy-Item -Recurse (Join-Path $gtkShare "gtk-4.0") (Join-Path $BuildDir "share\gtk-4.0") -Force -ErrorAction SilentlyContinue
    }
}

$Iscc = "C:\Program Files (x86)\Inno Setup 6\ISCC.exe"
if (-not (Test-Path $Iscc)) { $Iscc = "iscc" }

& $Iscc "/DMyAppVersion=$Version" "/DMyBuildDir=$BuildDir" "$Root\packaging\windows\installer.iss"

$Setup = Get-ChildItem "$Root\packaging\windows\output\*.exe" | Select-Object -First 1
if ($Setup) { Write-Host "Built: $($Setup.FullName)" }
else { throw "Installer not found in packaging/windows/output" }
Pop-Location
