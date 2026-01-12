# CLAUDE.md

Instructions for Claude Code when working with this repository.

## Project Overview

PSX Verb is an audio effect module for Move Anything that provides PlayStation 1 SPU reverb emulation with a distinctive lo-fi character popular for lo-fi hip hop, vaporwave, etc.

## Architecture

```
src/
  dsp/
    psxverb.c           # Main DSP implementation
    audio_fx_api_v1.h   # Audio FX API (from move-anything)
    plugin_api_v1.h     # Plugin API types (from move-anything)
  module.json           # Module metadata
```

## Key Implementation Details

### Audio FX API

Implements Move Anything audio_fx_api_v1:
- `on_load`: Initialize work buffer and DSP state
- `on_unload`: Cleanup
- `process_block`: In-place stereo audio processing
- `set_param`: preset, decay, mix
- `get_param`: Returns current parameter values

### DSP Components

1. **Work Buffer**: 16384 samples per channel circular buffer
2. **IIR Lowpass**: One-pole input filter for warmth
3. **Comb Filters**: 4 parallel comb filters with preset-defined delay times
4. **Allpass Diffusers**: 2 cascaded allpass filters for diffusion
5. **Wall Reflection**: Feedback path with decay control
6. **Mix**: Dry/wet crossfade

### Signal Flow

```
Input --> [IIR lowpass] --> [Comb bank (4x)] --> [Allpass (2x)] --> Output
                                  ^                    |
                                  |____ feedback ______|
```

### Presets (delay times in samples at 44.1kHz)

| Preset | Comb1-4 | APF1-2 | Wall |
|--------|---------|--------|------|
| Room | 1500-1800 | 500/400 | 0.6 |
| Studio S | 1200-1500 | 400/300 | 0.5 |
| Studio M | 2000-2600 | 600/500 | 0.65 |
| Studio L | 3000-3900 | 800/700 | 0.7 |
| Hall | 4000-5200 | 1000/900 | 0.75 |
| Space Echo | 5500-7000 | 1200/1100 | 0.8 |

### Signal Chain Integration

Module declares `"chainable": true` and `"component_type": "audio_fx"` in module.json.

Installs to: `/data/UserData/move-anything/modules/chain/audio_fx/psxverb/`

## Build Commands

```bash
./scripts/build.sh      # Build for ARM64 via Docker
./scripts/install.sh    # Deploy to Move
```
