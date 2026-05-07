
            // Ported from skwd-wall fadecolor transition

            vec4 close_color(vec3 coords_geo, vec3 size_geo) {
                float p = 1.0 - niri_clamped_progress;
                vec2 uv = coords_geo.xy;
                float colorPhase = 0.4;

                vec3 tc = niri_geo_to_tex * vec3(uv, 1.0);
                vec4 win = texture2D(niri_tex, tc.st);

                float reveal = smoothstep(colorPhase, 1.0, p);
                return win * reveal;
            }
