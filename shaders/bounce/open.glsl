
            // Ported from skwd-wall bounce transition

            vec4 open_color(vec3 coords_geo, vec3 size_geo) {
                float p = 1.0 - niri_clamped_progress;
                vec2 uv = coords_geo.xy;
                float PI = 3.14159265358;
                float bounces = 3.0;

                float time = p;
                float stime = sin(time * PI / 2.0);
                float phase = time * PI * bounces;
                float yy = (abs(cos(phase))) * (1.0 - stime);
                float d = uv.y - yy;

                vec2 sample_uv = uv;
                sample_uv.y = uv.y + (1.0 - yy);
                vec3 tc = niri_geo_to_tex * vec3(sample_uv, 1.0);
                vec4 win = texture2D(niri_tex, tc.st);

                float reveal = step(d, 0.0);
                return win * reveal;
            }
