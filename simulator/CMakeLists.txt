# 设备模拟器(S2 TCP;S7 增 UDP/串口):复用协议层编码,单事件循环服务全部设备。
# 设备侧不走 ITransport(那是客户端抽象),直接用 QTcpServer/QUdpSocket/QSerialPort
add_executable(dms_simulator
    DeviceSimulator.cpp
    main.cpp
)

target_compile_features(dms_simulator PRIVATE cxx_std_17)
target_link_libraries(dms_simulator PRIVATE
    dms_protocol dms_common Qt6::Core Qt6::Network Qt6::SerialPort)

# /utf-8、/W4 经 dms_common PUBLIC 传播,这里只补连接库

# S8 发布目录:模拟器随上位机一起发布(联调工具,TC-DEPLOY-002)
install(TARGETS dms_simulator RUNTIME DESTINATION bin)
