# hypranimated

> Development status: this plugin is under active development and is highly unstable. Hyprland plugin ABI changes and rendering-hook behavior can break it or crash the compositor, so use it only if you are comfortable rebuilding and debugging after Hyprland updates.

Hyprland plugin that applies the GLSL shaders in `jpOSh/shaders` to window open and close animations.

This plugin is built against the installed Hyprland headers and checks Hyprland's plugin ABI hash at load time. Rebuild it after every Hyprland upgrade.

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

`workspace_switch = true` disables Hyprland's normal workspace slide animations at runtime and replaces them with per-window workspace transitions. Each mapped, non-pinned window on the old workspace is rendered through `close.glsl`, and each mapped, non-pinned window on the new workspace is rendered through `open.glsl`. These windows share one workspace-switch timeline and seed, using monitor-centered coordinates so effects like smoke originate from the center of the screen without animating layer surfaces such as Waybar. Empty workspaces do not trigger a shader effect.

`sync_hyprland = true` makes Hyprland's `windowsMove` animation use the same duration as `duration_ms`, so tiled resize/move animations stay in time with the shader.
