# OBS PingPong Loop Filter

A video filter plugin for OBS Studio that creates a seamless ping-pong loop effect by recording and playing back video frames in a forward-backward-forward pattern.

## Features

- **Configurable Buffer Duration**: Record 10 to 60 seconds of video
- **Ping-Pong Playback**: Smooth forward and backward playback creating a seamless loop
- **Variable Playback Speed**: Adjust speed from 0.1x to 2.0x
- **Hotkey Support**: Toggle loop on/off with customizable hotkeys
- **Real-time Preview**: See live video while the buffer is recording

## Installation

### Building with Docker (Recommended for Linux/macOS)

The easiest way to build the plugin on Linux/macOS is using Docker, which handles all dependencies automatically.

#### Prerequisites
- Docker installed ([Get Docker](https://docs.docker.com/get-docker/))
- Git

#### Quick Build

```bash
# Clone the repository
git clone https://github.com/Biztactix-Ryan/Ping-Pong-LoopFilter.git
cd Ping-Pong-LoopFilter

# Build the plugin (default: OBS 31.1.1)
./build-docker.sh

# Build for a specific OBS version
OBS_VERSION=30.2.3 ./build-docker.sh

# The plugin will be in build-output/
ls build-output/
```

The built plugin (.so file for Linux) will be in the `build-output/` directory.

### Building on Windows

#### Prerequisites
- Visual Studio 2022 (Community Edition or higher)
- CMake 3.28+ ([Download](https://cmake.org/download/))
- Git

#### Quick Build with PowerShell

```powershell
# Clone the repository
git clone https://github.com/Biztactix-Ryan/Ping-Pong-LoopFilter.git
cd Ping-Pong-LoopFilter

# Run from Visual Studio 2022 Developer PowerShell
.\build-windows.ps1

# Or specify OBS version
.\build-windows.ps1 -OBSVersion 30.2.3

# Clean build
.\build-windows.ps1 -Clean
```

#### Quick Build with Command Prompt

```cmd
# Run from Visual Studio 2022 Developer Command Prompt
build-windows.bat

# Or specify OBS version
build-windows.bat 30.2.3
```

The script will:
1. Download OBS dependencies automatically
2. Download and build OBS libraries
3. Build the plugin
4. Offer to install it to your user directory

### Building from Source (Advanced)

#### Prerequisites
- CMake 3.28 or higher
- OBS Studio 31.1.1 or higher
- C++17 compatible compiler
- Platform-specific build tools:
  - **Windows**: Visual Studio 2022
  - **macOS**: Xcode 16.0+
  - **Linux**: GCC or Clang with build-essential, libobs-dev

#### Manual Build Instructions

1. Clone the repository:
```bash
git clone https://github.com/Biztactix-Ryan/Ping-Pong-LoopFilter.git
cd Ping-Pong-LoopFilter
```

2. Configure with CMake:
```bash
# Linux
cmake --preset ubuntu-x86_64

# macOS
cmake --preset macos

# Windows
cmake --preset windows-x64
```

3. Build the plugin:
```bash
# Linux
cmake --build build_x86_64 --config RelWithDebInfo

# macOS
cmake --build build_macos --config RelWithDebInfo

# Windows
cmake --build build_x64 --config RelWithDebInfo
```

4. Install the plugin:

#### Linux Installation
```bash
# Create plugin directory
mkdir -p ~/.config/obs-studio/plugins/obs-pingpong-loop-filter/bin/64bit/

# Copy the plugin (from Docker build)
cp build-output/*.so ~/.config/obs-studio/plugins/obs-pingpong-loop-filter/bin/64bit/

# Or from manual build
cp build_x86_64/*.so ~/.config/obs-studio/plugins/obs-pingpong-loop-filter/bin/64bit/
```

#### Windows Installation
- Copy the .dll file to: `C:\Program Files\obs-studio\obs-plugins\64bit\`

#### macOS Installation  
- Copy the .dylib file to: `/Library/Application Support/obs-studio/plugins/`

After installation, restart OBS Studio.

## Usage

1. Add the filter to any video source:
   - Right-click on a source in OBS
   - Select "Filters"
   - Click the "+" button under "Effect Filters"
   - Choose "PingPong Loop Filter"

2. Configure the filter settings:
   - **Buffer Length**: Set the duration of the loop (10-60 seconds)
   - **Ping-Pong Mode**: Enable for forward-backward playback, disable for forward-only loop
   - **Playback Speed**: Adjust the speed of playback

3. Start the loop:
   - Click "Toggle Loop" button in the filter properties, or
   - Use the assigned hotkey (configure in OBS Settings → Hotkeys)

## How It Works

The filter operates in two modes:

### Recording Mode (Loop Disabled)
- Video frames pass through unchanged to the output
- Frames are continuously recorded into a circular buffer
- Oldest frames are discarded when buffer is full

### Playback Mode (Loop Enabled)
- Buffered frames are played back in sequence
- When reaching the end, playback reverses direction
- Creates a seamless ping-pong effect
- Live input is ignored during playback

## Performance Considerations

### Memory Usage Estimates

| Resolution | FPS | Memory per Second | 30s Buffer | 60s Buffer |
|------------|-----|-------------------|------------|------------|
| 720p       | 30  | ~100 MB          | ~3 GB      | ~6 GB      |
| 1080p      | 30  | ~250 MB          | ~7.5 GB    | ~15 GB     |
| 1080p      | 60  | ~500 MB          | ~15 GB     | ~30 GB     |
| 1440p      | 60  | ~900 MB          | ~27 GB     | ~54 GB     |
| 4K         | 60  | ~2 GB            | ~60 GB     | ~120 GB    |

**Note**: The plugin includes automatic memory limiting (default 4GB) to prevent excessive GPU memory usage. If your desired buffer size exceeds the memory limit, frames will be automatically reduced.

### Performance Tuning Guide

1. **Memory Management**
   - The plugin automatically limits memory usage to 4GB by default
   - If you need more buffer capacity, ensure you have sufficient GPU memory
   - Resolution changes automatically clear the buffer to prevent memory issues

2. **Optimal Settings by System**
   - **Low-end (< 4GB VRAM)**: 720p sources, 10-20 second buffers
   - **Mid-range (4-8GB VRAM)**: 1080p sources, 20-40 second buffers  
   - **High-end (> 8GB VRAM)**: Any resolution, full 60 second buffers

3. **Reducing Memory Usage**
   - Lower the source resolution before applying the filter
   - Reduce buffer duration
   - Close other GPU-intensive applications

### Stability Improvements (v1.1.0)

The following issues have been addressed to improve long-term stability:

1. **Memory Leak Fixes**
   - Fixed GPU texture leak in error paths
   - Removed unsafe property pointer storage
   - Added proper cleanup for all GPU resources

2. **Race Condition Fixes**
   - Fixed frame buffer access synchronization
   - Ensured thread-safe frame rendering

3. **Resource Management**
   - Added automatic memory limiting
   - Resolution changes now clear buffer
   - Added overflow protection for long-running streams

4. **Error Handling**
   - Added null checks for GPU resource allocation
   - Improved error logging
   - Graceful fallback on resource allocation failure

### Known Limitations

- Maximum buffer memory limited to 4GB by default (configurable in code)
- Resolution changes will clear the current buffer
- Very high resolutions (4K+) may require significant GPU memory
- Long-running streams (days/weeks) will periodically reset counters for stability

### Troubleshooting

**Issue**: "Buffer won't fill completely"
- **Solution**: Check GPU memory usage - the plugin may be hitting memory limits

**Issue**: "Playback stutters or freezes"
- **Solution**: Reduce buffer size or source resolution

**Issue**: "OBS crashes when using the filter"
- **Solution**: Update to the latest version which includes memory leak fixes

**Issue**: "Buffer clears unexpectedly"
- **Solution**: This happens on resolution changes - ensure stable source resolution

### Best Practices for Long Streams

1. **Monitor GPU Memory**: Use GPU monitoring tools to track memory usage
2. **Test Settings**: Run a test stream for 10-15 minutes before going live
3. **Resolution Stability**: Ensure your source maintains consistent resolution
4. **Periodic Restart**: For 24/7 streams, consider restarting OBS weekly
5. **Error Logs**: Check OBS logs for any memory warnings from the plugin

## License

This project is licensed under the GNU General Public License v2.0 - see the LICENSE file for details.

## Author

Biztactix

## Acknowledgments

- OBS Studio team for the plugin API and template
- Community contributors for testing and feedback