            vec4 close_color(vec3 coords_geo, vec3 size_geo) {
                float p = niri_clamped_progress;
                vec2 uv = coords_geo.xy;

                vec2 center = vec2(0.5, 0.5);
                float scale = mix(1.0, 0.95, p);
                vec2 scaled_uv = (uv - center) / scale + center;

                vec3 tex_coords = niri_geo_to_tex * vec3(scaled_uv, 1.0);
                vec4 color = texture2D(niri_tex, tex_coords.st);

                float alpha = smoothstep(1.0, 0.2, p);

                return color * alpha;
            }
