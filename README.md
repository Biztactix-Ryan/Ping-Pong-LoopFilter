# Looper - OBS Studio Loop Plugin

Create seamless video loops for presentations, meetings, and creative content with this powerful OBS Studio plugin.

## Overview

Looper is an OBS Studio filter plugin that records video from any source and plays it back in a continuous loop. Perfect for virtual backgrounds, animated overlays, or creating the illusion of presence during online meetings - Looper ensures your video never looks static or frozen.

### Key Features

- 🔄 **Seamless Looping**: Record 10-60 seconds of video that plays continuously
- 🎯 **Ping-Pong Mode**: Forward-backward playback for perfectly smooth loops
- ⚡ **Variable Speed**: Adjust playback from 0.1x to 2.0x speed
- 🎮 **Hotkey Control**: Toggle loops instantly with customizable hotkeys
- 👁️ **Live Preview**: See your video while the buffer records
- 💾 **Smart Memory Management**: Automatic optimization prevents crashes

## Use Cases

### 🏢 Virtual Meetings
Keep your video active during long meetings - perfect for when you need to step away briefly. Your colleagues will see natural movement instead of a frozen frame.

### 🎬 Content Creation
- Create mesmerizing background loops for streams
- Build repeating animations without video editing software
- Design dynamic overlays that never stop moving

### 📊 Presentations
Loop product demonstrations, data visualizations, or ambient backgrounds to keep your audience engaged.

### 🎨 Creative Effects
- Generate infinite zoom effects
- Create seamless motion backgrounds
- Build hypnotic visual patterns

## Installation

### Quick Install - Download Pre-built Release

1. Download the latest release from the [Releases page](https://github.com/Biztactix-Ryan/Ping-Pong-LoopFilter/releases)
2. Extract the plugin file to your OBS plugins folder:
   - **Windows**: `C:\Program Files\obs-studio\obs-plugins\64bit\`
   - **macOS**: `/Library/Application Support/obs-studio/plugins/`
   - **Linux**: `~/.config/obs-studio/plugins/obs-looper/bin/64bit/`
3. Restart OBS Studio

### Install from Source

#### Option 1: Docker Build (Linux/macOS) - Easiest

```bash
# Clone and build
git clone https://github.com/Biztactix-Ryan/Ping-Pong-LoopFilter.git
cd Ping-Pong-LoopFilter
./build-docker.sh

# Install the plugin
mkdir -p ~/.config/obs-studio/plugins/obs-looper/bin/64bit/
cp build-output/*.so ~/.config/obs-studio/plugins/obs-looper/bin/64bit/
```

#### Option 2: Windows PowerShell Build

```powershell
# Clone and build (from Visual Studio 2022 Developer PowerShell)
git clone https://github.com/Biztactix-Ryan/Ping-Pong-LoopFilter.git
cd Ping-Pong-LoopFilter
.\build-windows.ps1

# The script will offer to install automatically
```

## How to Use

### Basic Setup

1. **Add Looper to a Source**
   - Right-click any video source in OBS
   - Select "Filters"
   - Click "+" under Effect Filters
   - Choose "Looper"

2. **Configure Your Loop**
   - **Buffer Length**: How much video to record (10-60 seconds)
   - **Ping-Pong Mode**: Enable for smooth reverse playback
   - **Playback Speed**: Control how fast the loop plays

3. **Start Looping**
   - Click "Toggle Loop" in the filter properties
   - Or use your configured hotkey (set in OBS Settings → Hotkeys)

### Pro Tips

- **Meeting Mode**: Record 30 seconds of natural movement (nodding, blinking, small gestures) then enable the loop
- **Creative Loops**: Use 60-second buffers for complex animated backgrounds
- **Performance**: Lower resolution sources use less memory and run smoother

## How It Works

Looper operates in two intelligent modes:

**Recording Mode** (Loop Off)
- Continuously captures frames to a circular buffer
- Shows live video to your audience
- Automatically manages memory usage

**Playback Mode** (Loop On)  
- Plays your recorded buffer seamlessly
- In ping-pong mode: plays forward, then backward, then forward again
- Creates natural-looking continuous motion

## Performance Guide

### Memory Requirements

| Resolution | 30 FPS | 60 FPS |
|------------|--------|---------|
| 720p | ~3 GB (30s) | ~6 GB (30s) |
| 1080p | ~7.5 GB (30s) | ~15 GB (30s) |
| 4K | ~30 GB (30s) | ~60 GB (30s) |

### Recommended Settings

- **Basic Systems** (4GB VRAM): 720p, 10-20 second buffers
- **Standard Systems** (8GB VRAM): 1080p, 30-40 second buffers
- **High-End Systems** (16GB+ VRAM): Any resolution, full 60 second buffers

## Troubleshooting

**Loop won't start?**
- Check available GPU memory
- Try reducing buffer length or source resolution

**Stuttering playback?**
- Lower the source resolution
- Reduce buffer duration
- Close other GPU-intensive applications

**Buffer clears unexpectedly?**
- This is normal when resolution changes
- Ensure your source maintains consistent resolution

## Building from Source

### Prerequisites

- CMake 3.28+
- C++17 compiler
- Platform-specific tools:
  - **Windows**: Visual Studio 2022
  - **macOS**: Xcode 16.0+
  - **Linux**: GCC/Clang with build-essential

### Build Commands

#### Configure
```bash
# Linux
cmake --preset ubuntu-x86_64

# macOS
cmake --preset macos

# Windows
cmake --preset windows-x64
```

#### Build
```bash
# Linux
cmake --build build_x86_64 --config RelWithDebInfo

# macOS
cmake --build build_macos --config RelWithDebInfo

# Windows
cmake --build build_x64 --config RelWithDebInfo
```

### Docker Build (Linux/macOS)

```bash
# Build with Docker (handles all dependencies)
./build-docker.sh

# Specify OBS version
OBS_VERSION=30.2.3 ./build-docker.sh

# Output will be in build-output/
```

### Windows Scripts

```powershell
# PowerShell (Visual Studio 2022 Developer PowerShell)
.\build-windows.ps1

# Command Prompt (Visual Studio 2022 Developer Command Prompt)
build-windows.bat
```

## Technical Details

### Stability Features (v1.1.0)

- **Memory Protection**: Automatic 4GB limit prevents crashes
- **Thread Safety**: Proper mutex synchronization
- **Resource Management**: RAII pattern for GPU textures
- **Error Recovery**: Graceful fallback on allocation failures
- **Overflow Protection**: Safe counter handling for long streams

### Architecture

- Circular frame buffer using `std::deque`
- GPU texture rendering with `gs_texrender`
- Time-based frame synchronization
- Dynamic resolution adaptation

## License

GNU General Public License v2.0 - see LICENSE file for details.

## Author

Created by Biztactix

## Support

- [Report Issues](https://github.com/Biztactix-Ryan/Ping-Pong-LoopFilter/issues)
- [Latest Releases](https://github.com/Biztactix-Ryan/Ping-Pong-LoopFilter/releases)

## Acknowledgments

- OBS Studio team for the excellent plugin API
- Community testers and contributors
- Everyone who uses Looper to stay "present" in their meetings 😉