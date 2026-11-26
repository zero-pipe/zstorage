cmake_minimum_required(VERSION 3.14)
project(zmedia C)

# Standalone build for the shared media model. Must not depend on ZN or ZMS;
# ci/check_layers.py in the server repo enforces that at the include level.

add_subdirectory(libcore)
add_subdirectory(libmpeg)
add_subdirectory(libflv)
add_subdirectory(libhls)
add_subdirectory(librtp)
add_subdirectory(libsdp)
add_subdirectory(libmov)
add_subdirectory(libmkv)
add_subdirectory(libdash)
