# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is an OBS Studio plugin written in C++ that implements a ping-pong loop filter for video sources. The filter records a configurable buffer (10-60 seconds) of video frames and plays them back in a continuous forward-backward-forward loop pattern, creating a seamless ping-pong effect.

## Build Commands

### Configure and Build

```bash
# Ubuntu/Linux (x86_64)
cmake --preset ubuntu-x86_64
cmake --build build_x86_64 --config RelWithDebInfo

# Ubuntu/Linux (aarch64)
cmake --preset ubuntu-aarch64
cmake --build build_aarch64 --config RelWithDebInfo

# macOS (Universal Binary)
cmake --preset macos
cmake --build build_macos --config RelWithDebInfo

# Windows (x64)
cmake --preset windows-x64
cmake --build build_x64 --config RelWithDebInfo
```

### Code Formatting

```bash
# Check C/C++ formatting
clang-format --dry-run --Werror src/*.c src/*.h

# Apply C/C++ formatting
clang-format -i src/*.c src/*.h

# Check CMake formatting (requires gersemi)
gersemi --check cmake/ CMakeLists.txt
```

### Clean Build

```bash
# Remove build directory and reconfigure
rm -rf build_*
cmake --preset <platform>
```

## Architecture

### Plugin Structure

The plugin follows OBS Studio's module architecture:

- **Entry Point**: `src/plugin-main.c` contains `obs_module_load()` and `obs_module_unload()`
- **Plugin Registration**: Filters/sources/outputs are registered in `obs_module_load()`
- **Callbacks**: Processing happens in filter update callbacks registered with OBS

### Key OBS Plugin Concepts

1. **Sources**: Generate or provide media (video/audio)
2. **Filters**: Process media from sources (this plugin should be a filter)
3. **Outputs**: Send processed media to files/streams
4. **Properties**: User-configurable settings exposed in OBS UI

### Build System

- **CMake 3.28+** required
- **buildspec.json**: Contains project metadata (name, version, dependencies)
- **CMakePresets.json**: Platform-specific build configurations
- Platform-specific CMake modules in `cmake/<platform>/`

### CI/CD Pipeline

GitHub Actions workflows handle:
- Multi-platform builds (Windows, macOS, Ubuntu)
- Code formatting validation (clang-format, gersemi)
- Artifact generation and release creation
- macOS code signing and notarization

## Development Guidelines

### Adding Filter Functionality

When implementing the ping-pong loop filter:

1. Register the filter in `obs_module_load()`:
   ```c
   obs_register_source(&ping_pong_filter);
   ```

2. Implement required callbacks:
   - `create`: Initialize filter data
   - `destroy`: Clean up resources
   - `update`: Handle property changes
   - `filter_audio` or `filter_video`: Process media frames
   - `get_properties`: Define UI controls

3. Handle audio/video processing in the appropriate callback with proper buffering for loop functionality

### Platform Considerations

- **Windows**: Uses Visual Studio 17 2022, requires Windows SDK
- **macOS**: Requires Xcode 16.0+, targets macOS 13.0+, Universal Binary (arm64 + x86_64)
- **Linux**: Ubuntu 24.04 baseline, uses Ninja build system

### Code Standards

- C17 standard
- 120 character line limit
- Tab indentation (width 8 for C)
- Follow existing OBS plugin patterns
- Use OBS logging macros: `blog(LOG_INFO, ...)`, `blog(LOG_ERROR, ...)`

### Common Development Tasks

```bash
# View recent changes
git log --oneline -20

# Find filter-related code in OBS examples
grep -r "obs_register_source" --include="*.c"

# Check for compilation errors/warnings
cmake --build build_<platform> --config RelWithDebInfo --verbose

# Package the plugin (after building)
cmake --build build_<platform> --config RelWithDebInfo --target package
```

## Important Files

- `buildspec.json`: Project metadata and version info
- `CMakeLists.txt`: Main build configuration
- `src/plugin-main.c`: Plugin entry point and registration
- `.github/workflows/`: CI/CD pipeline definitions
- `.clang-format`: Code formatting rules

## OBS API Integration Points

Key OBS functions to use:
- `obs_register_source()`: Register the filter
- `obs_source_get_settings()`: Get filter configuration
- `obs_data_get_*()`: Read settings values
- `obs_source_update()`: Apply settings changes
- `obs_source_process_filter_begin()`/`end()`: Video filter processing
- `obs_audio_data`: Audio filter data structure

## Implementation Details

The ping-pong loop filter is fully implemented with the following features:

### Core Functionality
- **Circular Frame Buffer**: Uses `std::deque<gs_texrender_t*>` to store video frames
- **Memory Management**: Automatically manages buffer size based on configured duration (10-60 seconds)
- **Playback Modes**: 
  - Pass-through mode: Shows live video while recording to buffer
  - Loop mode: Plays buffered frames in ping-pong pattern
- **Frame Timing**: Precise frame advancement based on OBS video FPS and playback speed

### User Controls
- **Buffer Length**: Configurable from 10 to 60 seconds
- **Ping-Pong Toggle**: Enable/disable reverse playback (vs forward-only loop)
- **Playback Speed**: Adjustable from 0.1x to 2.0x speed
- **Toggle Button**: UI button to start/stop loop playback
- **Hotkey Support**: Register custom hotkey for quick toggle

### Technical Implementation
- **Thread Safety**: Uses `std::mutex` for frame buffer access
- **Resolution Handling**: Dynamically adapts to source resolution changes
- **Resource Management**: Proper cleanup of GPU textures on destruction
- **Fallback Behavior**: Shows live feed if buffer is empty or on errors