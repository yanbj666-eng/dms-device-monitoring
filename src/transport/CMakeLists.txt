# 传输层:ITransport 抽象 + TCP 实现(LLD §1)
# 约束(HLD §1):依赖 common(错误码/日志)与 Qt Core/Network;
# 不依赖 protocol/domain——transport 只搬字节,不解析帧
add_library(dms_transport STATIC
    ITransport.h  # 含 Q_OBJECT:头文件必须列入源,AUTOMOC 才会生成 moc 产物
    TcpTransport.cpp
)

target_include_directories(dms_transport PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_features(dms_transport PUBLIC cxx_std_17)
target_link_libraries(dms_transport PUBLIC dms_common Qt6::Core Qt6::Network)
