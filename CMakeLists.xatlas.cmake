add_library(xatlas STATIC third_party/xatlas/src/xatlas.cpp)
target_include_directories(xatlas PUBLIC third_party/xatlas/src)
target_compile_definitions(xatlas PRIVATE _CRT_SECURE_NO_WARNINGS)