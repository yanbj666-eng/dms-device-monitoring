# 集成测试:QProcess 拉起真实 dms_simulator 进程,
# 全链路 TCP/UDP/串口 + 协议 + 会话 + 持久化
add_executable(tst_integration tst_integration.cpp)
target_compile_features(tst_integration PRIVATE cxx_std_17)
# dms_domain/dms_transport 传递协议与 transport 头文件路径;
# QTcpServer/QUdpSocket 探测端口需 Network,QSerialPortInfo 探测串口需 SerialPort
target_link_libraries(tst_integration PRIVATE dms_domain dms_transport dms_persistence
                                          Qt6::Network Qt6::SerialPort Qt6::Sql Qt6::Test)
# 模拟器可执行文件路径随配置展开注入,测试不猜构建目录布局
target_compile_definitions(tst_integration PRIVATE DMS_SIMULATOR_PATH="$<TARGET_FILE:dms_simulator>")
add_dependencies(tst_integration dms_simulator)
add_test(NAME tst_integration COMMAND tst_integration)
