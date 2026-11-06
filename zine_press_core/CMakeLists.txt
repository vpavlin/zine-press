cmake_minimum_required(VERSION 3.14)
project(ZinePressCorePlugin LANGUAGES CXX)
if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/LogosModule.cmake")
    include(cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found")
endif()
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/metadata.json" METADATA_JSON)
string(JSON MODULE_NAME GET ${METADATA_JSON} name)

# PDFium (pdfium-binaries): its PDFiumConfig.cmake sits at the package root, which CMake doesn't
# search on Linux - find the header and library directly
find_path(PDFIUM_INCLUDE_DIR fpdfview.h PATH_SUFFIXES include include/public REQUIRED)
find_library(PDFIUM_LIBRARY pdfium REQUIRED)
message(STATUS "PDFium: ${PDFIUM_INCLUDE_DIR} ${PDFIUM_LIBRARY}")

logos_module(
    NAME ${MODULE_NAME}
    SOURCES
        src/zine_press_core_impl.h
        src/zine_press_core_impl.cpp
        src/zine_plan.hpp
        src/zine_pdf.hpp
        src/zine_pdf.cpp
        src/zine_ipp.hpp
        src/zine_ipp.cpp
        src/zine_print.hpp
    INCLUDE_DIRS src ${PDFIUM_INCLUDE_DIR}
    FIND_PACKAGES qpdf JPEG
    LINK_LIBRARIES qpdf::libqpdf JPEG::JPEG ${PDFIUM_LIBRARY}
)

# pdfium-binaries' macOS library names itself "./libpdfium.dylib", which the linker copies into the
# plugin's load commands. Point it at the library's absolute store path: the portable bundler then
# copies it next to the plugin and rewrites the reference to @loader_path (it only bundles absolute
# /nix/store references; an @rpath one was left out, 0.1.1).
if(APPLE)
    get_filename_component(PDFIUM_LIBRARY_REAL "${PDFIUM_LIBRARY}" REALPATH)
    add_custom_command(TARGET ${MODULE_NAME}_module_plugin POST_BUILD
        COMMAND install_name_tool -change ./libpdfium.dylib "${PDFIUM_LIBRARY_REAL}" $<TARGET_FILE:${MODULE_NAME}_module_plugin>
        VERBATIM)
endif()
