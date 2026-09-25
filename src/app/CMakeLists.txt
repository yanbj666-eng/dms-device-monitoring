# 应用装配层:devices.json + app.ini → 各模块配置值对象(docs/15)
# 依赖方向(HLD §1):app -> domain/transport/common
add_library(dms_app STATIC
    Config.cpp
)

target_include_directories(dms_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_features(dms_app PUBLIC cxx_std_17)
target_link_libraries(dms_app PUBLIC dms_domain dms_common Qt6::Core)
