
            // Ported from skwd-wall colour-distance transition

            vec4 close_color(vec3 coords_geo, vec3 size_geo) {
                float p = 1.0 - niri_clamped_progress;
                vec2 uv = coords_geo.xy;
                float power = 5.0;

                vec3 tc = niri_geo_to_tex * vec3(uv, 1.0);
                vec4 win = texture2D(niri_tex, tc.st);

                float colorMag = length(win.rgb);
                float m = step(colorMag, p);
                float reveal = mix(m, 1.0, pow(p, power));

                return win * reveal;
            }
