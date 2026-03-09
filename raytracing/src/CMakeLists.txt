set(LIB_SOURCES
    vec3.cc
    color.cc
    ray.cc
)

add_library(RayTracerCore STATIC ${LIB_SOURCES})

target_include_directories(RayTracerCore PUBLIC .)