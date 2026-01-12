# Move Anything PSX Verb

PlayStation 1 SPU reverb emulation audio effect module for Move Anything.

## Features

- **Preset**: 6 classic PSX reverb types (Room, Studio S/M/L, Hall, Space Echo)
- **Decay**: Wall reflection feedback amount
- **Mix**: Dry/wet blend

## Algorithm

Simplified PSX SPU reverb structure:

```
Input --> [IIR lowpass] --> [Comb bank (4x)] --> [Allpass (2x)] --> Output
                                   ^                    |
                                   |____ feedback ______|
```

## Building

```bash
./scripts/build.sh      # Build for ARM64 via Docker
./scripts/install.sh    # Deploy to Move
```

## Presets

| # | Name | Character |
|---|------|-----------|
| 0 | Room | Small, tight |
| 1 | Studio S | Short studio |
| 2 | Studio M | Medium studio |
| 3 | Studio L | Large studio |
| 4 | Hall | Concert hall |
| 5 | Space Echo | Long ethereal |

## Installation

The module installs to `/data/UserData/move-anything/modules/chain/audio_fx/psxverb/`

## License

MIT License - Copyright (c) 2025 Charles Vestal
