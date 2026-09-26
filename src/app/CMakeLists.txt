# 应用装配层:devices.json + app.ini → 各模块配置值对象(docs/15)
# 依赖方向(HLD §1):app -> ui/domain/transport/common(装配入口在 main)
add_library(dms_app STATIC
    Config.cpp
)

target_include_directories(dms_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_features(dms_app PUBLIC cxx_std_17)
target_link_libraries(dms_app PUBLIC dms_domain dms_common Qt6::Core)

# 上位机可执行:GUI + IO 线程 + DB 线程 + 配置装配
add_executable(dms_monitor main.cpp)
target_compile_features(dms_monitor PRIVATE cxx_std_17)
target_link_libraries(dms_monitor PRIVATE dms_app dms_ui dms_persistence Qt6::Widgets)

# 配置模板部署到可执行目录(首启初始化源,docs/15 §1)
add_custom_command(TARGET dms_monitor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/../../config $<TARGET_FILE_DIR:dms_monitor>/config)
