
            // Ported from skwd-wall crosswarp transition

            vec4 close_color(vec3 coords_geo, vec3 size_geo) {
                float p = 1.0 - niri_clamped_progress;
                vec2 uv = coords_geo.xy;

                float x = smoothstep(0.0, 1.0, (p * 2.0 + uv.x - 1.0));
                vec2 warped = clamp((uv - 0.5) * x + 0.5, vec2(0.0), vec2(1.0));

                vec3 tc = niri_geo_to_tex * vec3(warped, 1.0);
                vec4 win = texture2D(niri_tex, tc.st);

                float in_bounds = step(0.0, uv.x) * step(uv.x, 1.0) * step(0.0, uv.y) * step(uv.y, 1.0);
                return win * x * in_bounds;
            }
