cmake_minimum_required(VERSION 3.13)
project(vk-layer-flimes)

set(CMAKE_CXX_STANDARD 17)

find_package(Threads)

include(GNUInstallDirs)

file(GLOB SOURCE_FILES
    "src/*.cpp"
    "src/*.hpp"
)
set(SCRIPT_FILE "data/vk-layer-flimes")
set(JSON_FILE "data/vk_layer_flimes.json")

add_library(${PROJECT_NAME} SHARED
    ${SOURCE_FILES}
    ${SCRIPT_FILE}
    ${JSON_FILE}
)

target_compile_definitions(${PROJECT_NAME}
    PUBLIC
    -DVK_NO_PROTOTYPES
    -DVK_LAYER_FLIMES_NAME="vk-layer-flimes"
    -DVK_LAYER_FLIMES_VERSION="1.1.0"
)

option(COMPILE_32_BIT "Compile using -m32 flag")
if(COMPILE_32_BIT)
    target_compile_options(${PROJECT_NAME}
        PUBLIC
        -m32
    )
    target_link_options(${PROJECT_NAME}
        PUBLIC
        -m32
    )
endif()

target_link_libraries(${PROJECT_NAME}
    ${CMAKE_THREAD_LIBS_INIT}
)

install(TARGETS ${PROJECT_NAME}
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(FILES ${JSON_FILE}
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/vulkan/implicit_layer.d"
)
install(PROGRAMS ${SCRIPT_FILE}
    DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
