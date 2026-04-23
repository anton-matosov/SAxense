cmake_minimum_required(VERSION 3.16)

project(SAxense LANGUAGES CXX)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(CMAKE_CXX_STANDARD 20)
    add_executable(SAxense SAxense.cpp)
else()
    add_custom_target(SAxense ALL
        COMMAND ${CMAKE_COMMAND} -E echo "SAxense can only be built on Linux because it requires Linux hidraw/input headers and APIs."
        COMMAND ${CMAKE_COMMAND} -E false
        VERBATIM
    )
endif()
