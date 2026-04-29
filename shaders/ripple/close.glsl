            // Ported from gl-transitions/ripple.glsl (MIT, gre)

            vec4 close_color(vec3 coords_geo, vec3 size_geo) {
                float p = niri_clamped_progress;
                vec2 uv = coords_geo.xy;
                float seed = niri_random_seed * 6.28318;

                float amplitude = 100.0;
                float speed = 50.0;

                vec2 dir = uv - vec2(0.5);
                float dist = length(dir);

                float intensity = p * p;
                vec2 offset = dir * (sin(p * dist * amplitude - p * speed + seed) + 0.5) / 30.0;

                vec2 wuv = uv + offset * intensity;
                vec3 tc = niri_geo_to_tex * vec3(wuv, 1.0);
                vec4 color = texture2D(niri_tex, tc.st);

                float alpha = smoothstep(1.0, 0.5, p);
                return color * alpha;
            }
