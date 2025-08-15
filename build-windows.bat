@echo off
setlocal EnableDelayedExpansion

REM Build script for OBS PingPong Loop Filter on Windows
REM Requires: Visual Studio 2022, CMake, and OBS Studio development files

echo === OBS PingPong Loop Filter - Windows Build ===
echo.

REM Configuration
set OBS_VERSION=31.1.1
if not "%1"=="" set OBS_VERSION=%1

echo Building for OBS version: %OBS_VERSION%
echo.

REM Check for Visual Studio 2022
where cl >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: Visual Studio compiler not found in PATH
    echo Please run this script from a Visual Studio 2022 Developer Command Prompt
    echo or run vcvarsall.bat first:
    echo "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
    exit /b 1
)

REM Check for CMake
where cmake >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: CMake not found in PATH
    echo Please install CMake from https://cmake.org/download/
    exit /b 1
)

REM Create build directory
if not exist build_x64 mkdir build_x64

REM Check if OBS dependencies are downloaded
if not exist obs-deps (
    echo.
    echo Downloading OBS dependencies...
    mkdir obs-deps
    cd obs-deps
    
    REM Download prebuilt OBS dependencies
    echo Downloading OBS Studio dependencies...
    curl -L -o obs-deps-windows.zip "https://github.com/obsproject/obs-deps/releases/download/2025-07-11/windows-deps-2025-07-11-x64.zip"
    if %ERRORLEVEL% NEQ 0 (
        echo ERROR: Failed to download OBS dependencies
        cd ..
        exit /b 1
    )
    
    REM Download Qt6
    echo Downloading Qt6...
    curl -L -o qt6-windows.zip "https://github.com/obsproject/obs-deps/releases/download/2025-07-11/windows-deps-qt6-2025-07-11-x64.zip"
    if %ERRORLEVEL% NEQ 0 (
        echo ERROR: Failed to download Qt6
        cd ..
        exit /b 1
    )
    
    REM Extract dependencies
    echo Extracting dependencies...
    tar -xf obs-deps-windows.zip
    tar -xf qt6-windows.zip
    
    cd ..
)

REM Check if OBS Studio source is downloaded
if not exist obs-studio (
    echo.
    echo Downloading OBS Studio %OBS_VERSION% source...
    curl -L -o obs-studio.zip "https://github.com/obsproject/obs-studio/archive/refs/tags/%OBS_VERSION%.zip"
    if %ERRORLEVEL% NEQ 0 (
        echo ERROR: Failed to download OBS Studio source
        exit /b 1
    )
    
    REM Extract OBS Studio source
    echo Extracting OBS Studio source...
    tar -xf obs-studio.zip
    if exist obs-studio-%OBS_VERSION% (
        rename obs-studio-%OBS_VERSION% obs-studio
    )
    del obs-studio.zip
)

REM Build OBS Studio (minimal, just libobs)
if not exist obs-studio\build_x64 (
    echo.
    echo Building OBS Studio libraries...
    cd obs-studio
    cmake --preset windows-x64 ^
        -DENABLE_BROWSER=OFF ^
        -DENABLE_WEBSOCKET=OFF ^
        -DENABLE_VIRTUALCAM=OFF ^
        -DENABLE_VST=OFF ^
        -DENABLE_SCRIPTING=OFF ^
        -DENABLE_UI=OFF ^
        -DENABLE_PLUGINS=OFF
    
    cmake --build build_x64 --config RelWithDebInfo --target libobs
    if %ERRORLEVEL% NEQ 0 (
        echo ERROR: Failed to build OBS Studio libraries
        cd ..
        exit /b 1
    )
    cd ..
)

REM Configure plugin build
echo.
echo Configuring plugin build...
cmake --preset windows-x64 ^
    -DCMAKE_PREFIX_PATH="%CD%\obs-studio\build_x64;%CD%\obs-deps"

if %ERRORLEVEL% NEQ 0 (
    echo ERROR: CMake configuration failed
    exit /b 1
)

REM Build the plugin
echo.
echo Building plugin...
cmake --build build_x64 --config RelWithDebInfo

if %ERRORLEVEL% NEQ 0 (
    echo ERROR: Build failed
    exit /b 1
)

REM Check for output
if exist build_x64\RelWithDebInfo\*.dll (
    echo.
    echo ===== Build Complete! =====
    echo.
    echo Plugin DLL location:
    dir /B build_x64\RelWithDebInfo\*.dll
    echo.
    echo To install:
    echo 1. Copy the DLL from build_x64\RelWithDebInfo\ to:
    echo    C:\Program Files\obs-studio\obs-plugins\64bit\
    echo    or
    echo    %%APPDATA%%\obs-studio\plugins\obs-pingpong-loop-filter\bin\64bit\
    echo.
    echo 2. Restart OBS Studio
) else (
    echo.
    echo WARNING: No DLL files found in build output
    echo Check build_x64\RelWithDebInfo\ for build artifacts
)

endlocal