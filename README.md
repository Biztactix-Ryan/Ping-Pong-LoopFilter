# OBS PingPong Loop Filter

A video filter plugin for OBS Studio that creates a seamless ping-pong loop effect by recording and playing back video frames in a forward-backward-forward pattern.

## Features

- **Configurable Buffer Duration**: Record 10 to 60 seconds of video
- **Ping-Pong Playback**: Smooth forward and backward playback creating a seamless loop
- **Variable Playback Speed**: Adjust speed from 0.1x to 2.0x
- **Hotkey Support**: Toggle loop on/off with customizable hotkeys
- **Real-time Preview**: See live video while the buffer is recording

## Installation

### Building with Docker (Recommended)

The easiest way to build the plugin is using Docker, which handles all dependencies automatically.

#### Prerequisites
- Docker installed ([Get Docker](https://docs.docker.com/get-docker/))
- Git

#### Quick Build

```bash
# Clone the repository
git clone https://github.com/Biztactix-Ryan/Ping-Pong-LoopFilter.git
cd Ping-Pong-LoopFilter

# Build the plugin
./build-docker.sh

# The plugin will be in build-output/
ls build-output/
```

The built plugin (.so file for Linux) will be in the `build-output/` directory.

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

- Each second of buffer at 1080p 60fps uses approximately 500MB of GPU memory
- Reduce buffer duration for lower-end systems
- Consider lowering source resolution if experiencing performance issues

## License

This project is licensed under the GNU General Public License v2.0 - see the LICENSE file for details.

## Author

Biztactix

## Acknowledgments

- OBS Studio team for the plugin API and template
- Community contributors for testing and feedback