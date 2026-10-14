# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=narrowing" APPEND)

project(hello_world)

# Keep untrusted Lua recursion shallow without modifying the vendored
# managed component. This must run here, not in main/CMakeLists.txt: main
# processes before the lua component target is registered, so resolving it
# there fails the configure.
idf_component_get_property(lua_component_lib georgik__lua COMPONENT_LIB)
target_compile_definitions(${lua_component_lib} PRIVATE LUAI_MAXCCALLS=32)
