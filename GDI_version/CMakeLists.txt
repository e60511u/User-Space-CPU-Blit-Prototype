cmake_minimum_required(VERSION 3.16)
project(DesktopCaptureGDI LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Windows subsystem (no console window)
set(CMAKE_WIN32_EXECUTABLE TRUE)

add_executable(DesktopCapture WIN32 main_gdi.cpp)

# Link required Windows libraries
target_link_libraries(DesktopCapture PRIVATE
    gdi32
    user32
    winmm
)

# Set output directory
set_target_properties(DesktopCapture PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
