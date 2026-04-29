            vec4 close_color(vec3 coords_geo, vec3 size_geo) {
                float p = niri_clamped_progress;
                vec2 uv = coords_geo.xy;

                float pixel_size = mix(0.0005, 0.10 + niri_random_seed * 0.04, p * p);
                vec2 pixelated_uv = floor(uv / pixel_size) * pixel_size + pixel_size * 0.5;

                vec3 tex_coords = niri_geo_to_tex * vec3(pixelated_uv, 1.0);
                vec4 color = texture2D(niri_tex, tex_coords.st);

                float alpha = smoothstep(1.0, 0.5, p);

                return color * alpha;
            }
