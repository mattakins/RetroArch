# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

### Configuration and Build
```bash
./configure                    # Configure build with default options
make                          # Build RetroArch
make clean                    # Clean build artifacts
make install                  # Install RetroArch system-wide
```

### Platform-Specific Builds
```bash
# Desktop builds
make                          # Unix/Linux/macOS
make -f Makefile.win          # Windows

# Console builds
make -f Makefile.ctr          # Nintendo 3DS
make -f Makefile.vita         # PlayStation Vita
make -f Makefile.switch       # Nintendo Switch
make -f Makefile.wiiu         # Wii U
```

### Development Options
```bash
./configure --enable-debug    # Debug build with symbols
make DEBUG=1                  # Alternative debug build
./configure --help            # Show all configure options
make info                     # Show build configuration
```

### Testing
```bash
# Test configurations available in tests-other/
# No unified test suite - individual test configs for input/joypad/autoconfig testing
```

### Build and Deployment
```bash
# IMPORTANT: Never build locally - always push to GitHub for CI builds
# Local builds may fail due to NDK version mismatches and environment differences
# GitHub Actions handles all platform builds including Android APKs
git add .
git commit -m "Description of changes"
git push origin master
```

### Code Documentation and Attribution
```bash
# IMPORTANT: Never mention AI tool references in:
# - Code comments
# - Commit messages
# - Documentation files
# - Variable names or function names
# - Log messages or user-facing text
# Keep all contributions appearing as natural developer work
```

## Gyro Support in Shaders

RetroArch now exposes gyroscope and accelerometer data to shaders, enabling motion-based visual effects.

### Configuration
- **Setting:** Video → Shaders → Enable Gyro for Shaders
- **Config Key:** `shader_gyro_enable = "true"`
- **Default:** Enabled
- **Platforms:** Android, Web (mobile browsers), PlayStation 3

### Available Shader Uniforms

Both Slang and GLSL shaders can access the following uniforms:

#### Gyroscope Data
- `GyroscopeX` (float) - Angular velocity around X axis
- `GyroscopeY` (float) - Angular velocity around Y axis
- `GyroscopeZ` (float) - Angular velocity around Z axis

#### Accelerometer Data
- `AccelerometerX` (float) - Acceleration along X axis (includes gravity)
- `AccelerometerY` (float) - Acceleration along Y axis (includes gravity)
- `AccelerometerZ` (float) - Acceleration along Z axis (includes gravity)

### Shader Example (Slang/GLSL)

```glsl
#version 450

// Gyro uniforms
uniform float GyroscopeX;
uniform float GyroscopeY;
uniform float GyroscopeZ;

// Accelerometer uniforms
uniform float AccelerometerX;
uniform float AccelerometerY;
uniform float AccelerometerZ;

// Shader parameters
#pragma parameter gyro_strength "Gyro Effect Strength" 1.0 0.0 5.0 0.1

uniform float gyro_strength;

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;
layout(binding = 0) uniform sampler2D Source;

void main()
{
    // Use gyro data for parallax effect
    vec2 offset = vec2(GyroscopeX, GyroscopeY) * 0.01 * gyro_strength;
    vec2 parallax_coord = vTexCoord + offset;

    // Use accelerometer for tilt-based color shift
    float tilt = AccelerometerX * 0.1;

    vec4 color = texture(Source, parallax_coord);
    color.r += tilt;

    FragColor = color;
}
```

### Use Cases

1. **Motion Blur Effects** - Add directional blur based on rotation speed
2. **Parallax/Depth Effects** - Simulate 3D depth by shifting layers based on device tilt
3. **Screen Distortion** - Reactive distortion effects based on motion intensity
4. **Tilt-Based Filters** - Color grading or effects that respond to device orientation

### Technical Details

- **Data Source:** Uses RetroArch's existing `input_get_sensor_state()` API
- **Sensitivity:** Sensitivity multipliers are already applied at the input layer
- **History:** Current frame only (no historical buffering)
- **Filtering:** Raw sensor data (no additional smoothing)
- **Port:** Uses port 0 (primary controller)
- **Performance:** Zero overhead when disabled via settings

### Implementation Files

- **Slang Semantics:** `gfx/drivers_shader/slang_reflection.h`, `slang_reflection.cpp`
- **OpenGL 3+:** `gfx/drivers_shader/shader_gl3.cpp`
- **Vulkan:** `gfx/drivers_shader/shader_vulkan.cpp`
- **GLSL/CG:** `gfx/drivers_shader/shader_glsl.c`
- **Configuration:** `configuration.h`, `configuration.c`

## Code Architecture

### Core System Components
- **Main Loop**: `retroarch.c` - Primary entry point and program lifecycle
- **Run Loop**: `runloop.c` - Core execution loop managing frames, input, audio/video sync
- **Configuration**: `configuration.c` + `config.def.h` - Centralized config management with platform defaults
- **Drivers**: Modular driver architecture for audio, video, input, menu systems

### Directory Structure
- **`frontend/drivers/`** - Platform-specific initialization (Unix, Windows, Darwin, consoles)
- **`gfx/`** - Graphics drivers (OpenGL variants, Vulkan, D3D8-12, platform-specific)
- **`audio/`** - Audio drivers (ALSA, CoreAudio, PulseAudio, WASAPI, XAudio2, platform-specific)
- **`input/`** - Input handling and drivers (DirectInput, XInput, SDL, gamepad APIs)
- **`menu/`** - Menu system with multiple UI implementations (MaterialUI, Ozone, RGUI, XMB)
- **`tasks/`** - Background task system for async operations (downloads, file ops, scanning)
- **`cores/`** - Core management and libretro interface handling
- **`libretro-common/`** - Shared utilities across libretro ecosystem
- **`qb/`** - Build system shell scripts for feature detection and configuration

### Driver Architecture
RetroArch uses a pluggable driver system where each subsystem (audio, video, input, menu) can have multiple implementations selected at runtime. Drivers are loaded as function pointers in structs, enabling hot-swapping between backends and platform abstraction.

### Build System
- **QB System**: Shell-based build configuration in `qb/` directory for feature detection
- **Platform Makefiles**: Separate makefiles for different platforms and consoles
- **Configuration**: `./configure` script generates `config.mk` with detected features
- **Griffin Build**: Optional single compilation unit build for performance (`Makefile.griffin`)

### Key Development Patterns
- **Thread Safety**: Audio/video drivers require careful synchronization
- **C89 Compliance**: Core codebase maintains C89 compatibility for console platforms
- **Manual Memory Management**: No garbage collection, explicit malloc/free throughout
- **Platform Abstraction**: Common interfaces with platform-specific implementations
- **Real-time Constraints**: Audio/video sync requirements affect architectural decisions

### Configuration System
- **Default Config**: `config.def.h` contains compile-time defaults
- **Runtime Config**: `retroarch.cfg` for user settings, auto-generated if missing
- **Feature Flags**: Extensive use of `HAVE_*` flags for conditional compilation
- **Platform Variants**: Different feature sets available per platform

### libretro Integration
RetroArch serves as the reference frontend for the libretro API. It dynamically loads libretro cores (emulators/game engines) as shared libraries and provides the audio/video/input interface they require.

## Coding Standards

### Code Style (from CODING-GUIDELINES)
- **Braces**: Allman style - braces on separate line, aligned with control statement
- **Variables**: Declare at function start or block start, no C99 for-loop declarations
- **No VLAs**: Variable Length Arrays not permitted (C89 compliance)
- **Struct Ordering**: Sort members by alignment (largest types first) for optimal packing
- **Stack Usage**: Conservative stack allocation due to console memory constraints
- **Function Design**: Avoid small getter/setter functions, justify call overhead with meaningful work

### Platform Considerations
- **Memory Constraints**: Some consoles have limited RAM and stack (128KB default stack)
- **Performance Critical**: Real-time audio/video processing requirements
- **Multi-platform**: Code must work across 40+ platforms with varying capabilities
- **Backwards Compatibility**: Maintain compatibility with older libretro cores and configurations
