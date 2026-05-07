
            // Ported from skwd-wall randomsquares transition

            float rs_rand(vec2 co) {
                return fract(sin(dot(co.xy, vec2(12.9898, 78.233))) * 43758.5453);
            }

            vec4 open_color(vec3 coords_geo, vec3 size_geo) {
                float p = niri_clamped_progress;
                vec2 uv = coords_geo.xy;
                vec3 tc = niri_geo_to_tex * vec3(uv, 1.0);
                vec4 win = texture2D(niri_tex, tc.st);

                vec2 sz = vec2(10.0, 10.0);
                float smoothness = 0.5;
                float r = rs_rand(floor(sz * uv));
                float reveal = smoothstep(0.0, -smoothness, r - (p * (1.0 + smoothness)));

                return win * reveal;
            }
