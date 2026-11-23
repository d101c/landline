# The one privileged piece, kept apart from the daemon: see main.cpp. It is
# built from two files and links nothing but the C++ runtime, so that what
# carries the capability stays small enough to read.
add_executable(landline-le-params
    main.cpp
    ${CMAKE_SOURCE_DIR}/src/core/src/bluetooth/le_conn_update.cpp
)
target_include_directories(landline-le-params PRIVATE ${CMAKE_SOURCE_DIR}/src/core/include)
find_package(Threads REQUIRED)
target_link_libraries(landline-le-params PRIVATE Threads::Threads)
# No library of its own to be found at run time: a program that carries a
# capability must not load code from a path someone else can write to.
target_link_options(landline-le-params PRIVATE -static-libstdc++ -static-libgcc)
set_target_properties(landline-le-params PROPERTIES SKIP_BUILD_RPATH TRUE)
set_target_properties(landline-le-params PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

install(TARGETS landline-le-params
        RUNTIME DESTINATION libexec/landline)
