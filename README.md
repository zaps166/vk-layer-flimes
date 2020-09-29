# vk-layer-flimes

Frame limiter for Vulkan

# How to use

- run Vulkan application with 60 FPS limit: `vk-layer-flimes 60 executable_name`
- run Vulkan application with initial 60 FPS limit which can be changed: `vk-layer-flimes 60 ext_control executable_name`
- run Vulkan application with 60 FPS limit, disable V-Sync: `vk-layer-flimes 60 immediate executable_name`
- run Vulkan application in Steam with 60 FPS limit (Properties... -> SET LAUNCH OPTIONS): `vk-layer-flimes 60 %command%`

For more information, run `vk-layer-flimes` with no arguments.

# Environment variables

All environment variables are set via `vk-layer-flimes` script, but they also can be set manually.

- `ENABLE_VK_LAYER_FLIMES` - `1` - enable vk-layer-flimes,
- `VK_LAYER_FLIMES_ENABLE_EXTERNAL_CONTROL` - `1` - enable external FPS control
- `VK_LAYER_FLIMES_FILTER` - `nearest` or `trilinear` - force texture filtering
- `VK_LAYER_FLIMES_MIP_LOD_BIAS` - float number - force Mipmap LOD bias
- `VK_LAYER_FLIMES_MAX_ANISOTROPY` - float number - force max anisotropy
- `VK_LAYER_FLIMES_MIN_IMAGE_COUNT` - integer number - force minimum image count if supported by the driver:
  - `2` - double buffering
  - `3` - triple buffering
- `VK_LAYER_FLIMES_PRESENT_MODE` - force present mode if supported by the driver:
  - `immediate` - V-Sync OFF
  - `mailbox` - tear-free
  - `fifo` - V-Sync ON
  - `fifo_relaxed` - adaptive V-Sync

# Install

See `vk-layer-flimes-git` AUR package.
