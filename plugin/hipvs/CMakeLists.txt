cmake_minimum_required(VERSION 3.31 FATAL_ERROR)

project(seekdb_hipvs_plugin LANGUAGES CXX HIP)

find_package(cuvs REQUIRED COMPONENTS compiled)

add_library(seekdb_hipvs SHARED seekdb_hipvs_plugin.cu)
target_compile_features(seekdb_hipvs PRIVATE cxx_std_17)
target_include_directories(
  seekdb_hipvs
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src/oblib/lib/vector"
)
target_link_libraries(seekdb_hipvs PRIVATE cuvs::cuvs)
set_target_properties(seekdb_hipvs PROPERTIES OUTPUT_NAME seekdb_hipvs)
