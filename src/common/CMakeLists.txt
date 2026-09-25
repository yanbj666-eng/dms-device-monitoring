# 公共基础设施:日志/错误码(LLD §8)
# 约束:不依赖 protocol/transport/domain,只依赖 Qt Core;被所有模块引用
add_library(dms_common STATIC
    Logging.cpp
)

target_include_directories(dms_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_features(dms_common PUBLIC cxx_std_17)
target_link_libraries(dms_common PUBLIC Qt6::Core)

# /utf-8 与警告级别和 dms_protocol 保持同一口径
target_compile_options(dms_common PUBLIC
    "$<$<CXX_COMPILER_ID:MSVC>:/utf-8;/W4>"
    "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra>"
)
