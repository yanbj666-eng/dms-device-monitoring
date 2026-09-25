# 领域层:设备会话(S2 雏形;状态机/命令生命周期 S3 接入)
# 依赖方向(HLD §1):domain -> transport/protocol(common 为基础设施)
add_library(dms_domain STATIC
    DeviceSession.cpp
)

target_include_directories(dms_domain PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_features(dms_domain PUBLIC cxx_std_17)
target_link_libraries(dms_domain PUBLIC dms_protocol dms_transport dms_common Qt6::Core)
