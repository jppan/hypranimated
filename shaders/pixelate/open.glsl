            vec4 open_color(vec3 coords_geo, vec3 size_geo) {
                float p = niri_clamped_progress;
                vec2 uv = coords_geo.xy;

                float pixel_size = mix(0.06 + niri_random_seed * 0.04, 0.0005, p * p);
                vec2 pixelated_uv = floor(uv / pixel_size) * pixel_size + pixel_size * 0.5;

                vec3 tex_coords = niri_geo_to_tex * vec3(pixelated_uv, 1.0);
                vec4 color = texture2D(niri_tex, tex_coords.st);

                vec2 center = uv - 0.5;
                float dist = length(center);
                float reveal = smoothstep(0.0, 0.5, p) * smoothstep(dist * 0.8, dist * 0.8 - 0.3, (1.0 - p));

                float alpha = smoothstep(0.0, 0.3, p);
                return color * alpha;
            }
