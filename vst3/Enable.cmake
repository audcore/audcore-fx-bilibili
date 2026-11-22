# The optional project include runs before the FX target is declared.
cmake_language(DEFER CALL include "${CMAKE_CURRENT_SOURCE_DIR}/vst3/Build.cmake")
