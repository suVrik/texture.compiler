# texture.compiler

Normal part of `RG normal, B metalness, A ambient occlusion` is mip mapped with normal renormalization, the rest of this texture is mip mapped like a regular texture.

| Input texture | Output (Production quality) | Output (Development quality) | Output (No-compression quality) |
| ------------- | ------------- | ------------- | ------------- |
| RGB base color, A roughness | BC7 | BC3 | RGBA8 |
| RG normal, B metalness, A ambient occlusion | BC7 | BC3 | RGBA8 |
| R height | BC4 | BC4 | R8 |
| HDR cube map | BC6H | BC3 | RGBA16 |
| Irradiance (automatically generated from cube map) | RGBA16 | RGBA16 | RGBA16 |
| Prefilter (automatically generated from cube map) | BC6H | BC6H | RGBA16 |

```
usage:
  texture_compiler.exe  options

where options are:
  --albedo-roughness                      Input contains albedo map and
                                          roughness map
  --normal-metalness-ambient-occlusion    Input contains normal, metalness
                                          and ambient occlusion maps
  --parallax                              Input contains parallax map
  --cube-map                              Input contains cube map
  --input <example.png>                   Input texture path
  --output <example.texture>              Output texture path
  --output-size <1024>                    Output texture size (needed only
                                          for cube map, for other textures
                                          output texture size is equal to
                                          input texture size)
  --irradiance <irradiance.texture>       Output irradiance texture path
                                          (needed only for cube map)
  --irradiance-size <32>                  Output irradiance texture size
                                          (needed only for cube map)
  --prefilter <prefilter.texture>         Output prefilter texture path
                                          (needed only for cube map)
  --prefilter-size <128>                  Output prefilter texture size
                                          (needed only for cube map)
  --production                            Good but slow texture compression
  --development                           Poor but quick texture compression
  --no-compression                        No texture compression
  -?, -h, --help                          display usage information
```
