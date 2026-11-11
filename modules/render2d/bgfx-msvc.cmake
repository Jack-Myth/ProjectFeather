# Meson supplies /EHsc for C++ targets that need it. Keep bx's own /EHs-c-.
string(REGEX REPLACE "(^|[ \t])/EHsc([ \t]|$)" " " CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")

function(feather_filter_bx_options)
    get_target_property(options bx INTERFACE_COMPILE_OPTIONS)
    # Meson's CMake converter propagates the disabled branch of this generator expression.
    list(REMOVE_ITEM options "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-msse4.2>")
    set_property(TARGET bx PROPERTY INTERFACE_COMPILE_OPTIONS "${options}")
endfunction()

cmake_language(DEFER CALL feather_filter_bx_options)
