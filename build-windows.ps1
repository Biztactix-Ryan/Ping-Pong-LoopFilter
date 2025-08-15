# PowerShell build script for OBS PingPong Loop Filter on Windows
# Requires: Visual Studio 2022, CMake, and OBS Studio development files

param(
    [string]$OBSVersion = "31.1.1",
    [string]$Configuration = "RelWithDebInfo",
    [switch]$Clean = $false
)

Write-Host "=== OBS PingPong Loop Filter - Windows Build ===" -ForegroundColor Cyan
Write-Host ""
Write-Host "OBS Version: $OBSVersion" -ForegroundColor Yellow
Write-Host "Configuration: $Configuration" -ForegroundColor Yellow
Write-Host ""

# Check for required tools
function Test-Command {
    param($Command)
    $null = Get-Command $Command -ErrorAction SilentlyContinue
    return $?
}

# Check for Visual Studio
if (-not (Test-Command "cl.exe")) {
    Write-Host "ERROR: Visual Studio compiler not found" -ForegroundColor Red
    Write-Host "Please run this script from a Visual Studio 2022 Developer PowerShell"
    Write-Host "or initialize the environment first:"
    Write-Host '& "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\Launch-VsDevShell.ps1"'
    exit 1
}

# Check for CMake
if (-not (Test-Command "cmake")) {
    Write-Host "ERROR: CMake not found" -ForegroundColor Red
    Write-Host "Please install CMake from https://cmake.org/download/"
    exit 1
}

# Clean build if requested
if ($Clean) {
    Write-Host "Cleaning previous build..." -ForegroundColor Yellow
    if (Test-Path "build_x64") {
        Remove-Item -Recurse -Force "build_x64"
    }
    if (Test-Path "obs-studio") {
        Remove-Item -Recurse -Force "obs-studio"
    }
    if (Test-Path "obs-deps") {
        Remove-Item -Recurse -Force "obs-deps"
    }
}

# Create build directory
if (-not (Test-Path "build_x64")) {
    New-Item -ItemType Directory -Path "build_x64" | Out-Null
}

# Download OBS dependencies if needed
if (-not (Test-Path "obs-deps")) {
    Write-Host ""
    Write-Host "Downloading OBS dependencies..." -ForegroundColor Yellow
    New-Item -ItemType Directory -Path "obs-deps" | Out-Null
    Set-Location "obs-deps"
    
    # Download prebuilt dependencies
    Write-Host "Downloading OBS Studio dependencies..."
    try {
        Invoke-WebRequest -Uri "https://github.com/obsproject/obs-deps/releases/download/2025-07-11/windows-deps-2025-07-11-x64.zip" `
                         -OutFile "obs-deps-windows.zip"
    } catch {
        Write-Host "ERROR: Failed to download OBS dependencies" -ForegroundColor Red
        Set-Location ..
        exit 1
    }
    
    # Download Qt6
    Write-Host "Downloading Qt6..."
    try {
        Invoke-WebRequest -Uri "https://github.com/obsproject/obs-deps/releases/download/2025-07-11/windows-deps-qt6-2025-07-11-x64.zip" `
                         -OutFile "qt6-windows.zip"
    } catch {
        Write-Host "ERROR: Failed to download Qt6" -ForegroundColor Red
        Set-Location ..
        exit 1
    }
    
    # Extract dependencies
    Write-Host "Extracting dependencies..."
    Expand-Archive -Path "obs-deps-windows.zip" -DestinationPath "." -Force
    Expand-Archive -Path "qt6-windows.zip" -DestinationPath "." -Force
    
    Set-Location ..
}

# Download OBS Studio source if needed
if (-not (Test-Path "obs-studio")) {
    Write-Host ""
    Write-Host "Downloading OBS Studio $OBSVersion source..." -ForegroundColor Yellow
    
    try {
        Invoke-WebRequest -Uri "https://github.com/obsproject/obs-studio/archive/refs/tags/$OBSVersion.zip" `
                         -OutFile "obs-studio.zip"
    } catch {
        Write-Host "ERROR: Failed to download OBS Studio source" -ForegroundColor Red
        exit 1
    }
    
    # Extract OBS Studio source
    Write-Host "Extracting OBS Studio source..."
    Expand-Archive -Path "obs-studio.zip" -DestinationPath "." -Force
    if (Test-Path "obs-studio-$OBSVersion") {
        Rename-Item "obs-studio-$OBSVersion" "obs-studio"
    }
    Remove-Item "obs-studio.zip"
}

# Build OBS Studio libraries if needed
if (-not (Test-Path "obs-studio\build_x64")) {
    Write-Host ""
    Write-Host "Building OBS Studio libraries..." -ForegroundColor Yellow
    Set-Location "obs-studio"
    
    # Configure OBS build (minimal, just libobs)
    & cmake --preset windows-x64 `
        -DENABLE_BROWSER=OFF `
        -DENABLE_WEBSOCKET=OFF `
        -DENABLE_VIRTUALCAM=OFF `
        -DENABLE_VST=OFF `
        -DENABLE_SCRIPTING=OFF `
        -DENABLE_UI=OFF `
        -DENABLE_PLUGINS=OFF
    
    if ($LASTEXITCODE -ne 0) {
        Write-Host "ERROR: Failed to configure OBS Studio" -ForegroundColor Red
        Set-Location ..
        exit 1
    }
    
    # Build libobs
    & cmake --build build_x64 --config $Configuration --target libobs
    
    if ($LASTEXITCODE -ne 0) {
        Write-Host "ERROR: Failed to build OBS Studio libraries" -ForegroundColor Red
        Set-Location ..
        exit 1
    }
    
    Set-Location ..
}

# Configure plugin build
Write-Host ""
Write-Host "Configuring plugin build..." -ForegroundColor Yellow

$ObsPath = Join-Path $PWD "obs-studio\build_x64"
$DepsPath = Join-Path $PWD "obs-deps"

& cmake --preset windows-x64 `
    "-DCMAKE_PREFIX_PATH=$ObsPath;$DepsPath"

if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: CMake configuration failed" -ForegroundColor Red
    exit 1
}

# Build the plugin
Write-Host ""
Write-Host "Building plugin..." -ForegroundColor Yellow
& cmake --build build_x64 --config $Configuration

if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: Build failed" -ForegroundColor Red
    exit 1
}

# Check for output and display results
$DllPath = "build_x64\$Configuration"
$DllFiles = Get-ChildItem -Path $DllPath -Filter "*.dll" -ErrorAction SilentlyContinue

if ($DllFiles) {
    Write-Host ""
    Write-Host "===== Build Complete! =====" -ForegroundColor Green
    Write-Host ""
    Write-Host "Plugin DLL location:" -ForegroundColor Cyan
    foreach ($dll in $DllFiles) {
        Write-Host "  $($dll.FullName)" -ForegroundColor White
    }
    Write-Host ""
    Write-Host "To install:" -ForegroundColor Yellow
    Write-Host "1. Copy the DLL to one of these locations:"
    Write-Host "   - C:\Program Files\obs-studio\obs-plugins\64bit\" -ForegroundColor White
    Write-Host "   - $env:APPDATA\obs-studio\plugins\obs-pingpong-loop-filter\bin\64bit\" -ForegroundColor White
    Write-Host ""
    Write-Host "2. Restart OBS Studio"
    
    # Offer to install
    Write-Host ""
    $response = Read-Host "Would you like to install the plugin to your user directory now? (Y/N)"
    if ($response -eq 'Y' -or $response -eq 'y') {
        $InstallPath = "$env:APPDATA\obs-studio\plugins\obs-pingpong-loop-filter\bin\64bit"
        if (-not (Test-Path $InstallPath)) {
            New-Item -ItemType Directory -Path $InstallPath -Force | Out-Null
        }
        Copy-Item -Path $DllFiles[0].FullName -Destination $InstallPath -Force
        Write-Host "Plugin installed to: $InstallPath" -ForegroundColor Green
        Write-Host "Please restart OBS Studio to use the plugin"
    }
} else {
    Write-Host ""
    Write-Host "WARNING: No DLL files found in build output" -ForegroundColor Yellow
    Write-Host "Check $DllPath for build artifacts"
}