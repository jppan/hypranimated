# hypranimated

> Development status: this plugin is under active development and is highly unstable. Hyprland plugin ABI changes and rendering-hook behavior can break it or crash the compositor, so use it only if you are comfortable rebuilding and debugging after Hyprland updates.

Hyprland plugin that applies the GLSL shaders in a defined directory, for instance `jpOSh/shaders` to window open and close animations.

This plugin is built against the installed Hyprland headers and checks Hyprland's plugin ABI hash at load time. Rebuild it after every Hyprland upgrade. This has been tested on hyprland 0.54.3

The plugin accepts these Hyprland config values:

```ini
plugin:hypranimated {
    enabled = true
    effect = smoke
    shaders_dir = /home/jppan/.local/bin/jpOSh/shaders
    duration_ms = 350
    workspace_switch = false
    sync_hyprland = true
}
```

Each effect directory should contain `open.glsl` and `close.glsl` using niri's `open_color` / `close_color` shader interface.

The shader wrapper provides:

- `uniform sampler2D niri_tex`
- `uniform mat3 niri_geo_to_tex`
- `uniform float niri_progress`
- `uniform float niri_clamped_progress`
- `uniform float niri_random_seed`

Set `duration_ms = 0` to use an effect directory's `config` file `duration-ms`; otherwise the Hyprland config value overrides it. Run `make` from this directory, then reload the plugin or restart Hyprland.

`workspace_switch = true` disables Hyprland's normal workspace animations at runtime and replaces them with one monitor-sized shader pass. Each mapped, non-pinned window on the new workspace is captured into a transparent framebuffer, and the shared `open.glsl` output is patched back over those window regions using monitor-centered coordinates so effects like smoke originate from the center of the screen. Layer surfaces such as Waybar and wallpaper are not captured. Empty destination workspaces do not trigger an open effect; if the previous workspace had windows, those windows are captured instead and rendered through one shared `close.glsl` pass.

`sync_hyprland = true` makes Hyprland's `windowsMove` animation use the same duration as `duration_ms`, so tiled resize/move animations stay in time with the shader window animations.

Feature roadmap:

- [ ] Support Nvidia
- [ ] Multi monitor support

Known issues:

Flickering on workspace changes whilst screen sharing with xdg-portal-hyprland
Framebuffer issues/temporary flicker on toggling to an empty workspace
Fps drops on workspace changes with many windows

Shader credit goes to https://github.com/liixini/shaders.
