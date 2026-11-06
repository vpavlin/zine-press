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
    INCLUDE_DIRS src ${PDFIUM_INCLUDE_DIR}
    FIND_PACKAGES qpdf JPEG
    LINK_LIBRARIES qpdf::libqpdf JPEG::JPEG ${PDFIUM_LIBRARY}
)
