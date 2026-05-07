
            // Ported from skwd-wall directional-wipe transition

            vec4 close_color(vec3 coords_geo, vec3 size_geo) {
                float p = 1.0 - niri_clamped_progress;
                vec2 uv = coords_geo.xy;
                vec3 tc = niri_geo_to_tex * vec3(uv, 1.0);
                vec4 win = texture2D(niri_tex, tc.st);

                vec2 dir = vec2(1.0, -1.0);
                float smoothness = 0.5;
                vec2 center = vec2(0.5, 0.5);
                vec2 v = normalize(dir);
                v /= abs(v.x) + abs(v.y);
                float d = v.x * center.x + v.y * center.y;
                float reveal = (1.0 - step(p, 0.0)) *
                    (1.0 - smoothstep(-smoothness, 0.0, v.x * uv.x + v.y * uv.y - (d - 0.5 + p * (1.0 + smoothness))));

                return win * reveal;
            }
