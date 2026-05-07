
            // Ported from skwd-wall morph transition

            vec4 close_color(vec3 coords_geo, vec3 size_geo) {
                float p = 1.0 - niri_clamped_progress;
                vec2 uv = coords_geo.xy;
                float strength_v = 0.15;

                vec3 tc0 = niri_geo_to_tex * vec3(uv, 1.0);
                vec4 cb = texture2D(niri_tex, tc0.st);
                vec2 ob = ((cb.rg + cb.b) * 0.5) * 2.0 - 1.0;
                vec2 oc = ob * strength_v;
                float w1 = 1.0 - p;

                vec2 sample_uv = uv - oc * w1;
                vec3 tc = niri_geo_to_tex * vec3(sample_uv, 1.0);
                vec4 win = texture2D(niri_tex, tc.st);

                return win * p;
            }
