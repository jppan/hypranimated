
            // Ported from skwd-wall directional transition

            vec4 close_color(vec3 coords_geo, vec3 size_geo) {
                float p = niri_clamped_progress;
                vec2 uv = coords_geo.xy;
                vec3 tc = niri_geo_to_tex * vec3(uv, 1.0);
                vec4 win = texture2D(niri_tex, tc.st);

                vec2 dir = vec2(0.0, 1.0);
                vec2 q = uv + (1.0 - p) * sign(dir);
                float inside = step(0.0, q.y) * step(q.y, 1.0) * step(0.0, q.x) * step(q.x, 1.0);

                return win * (1.0 - inside);
            }
