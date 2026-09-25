cmake_minimum_required(VERSION 3.21)

project(dms
    VERSION 0.1.0
    DESCRIPTION "DMS - Device Monitoring System"
    LANGUAGES CXX
)

# AUTOMOC 全局开启:Q_OBJECT 类散布在各 target,由 CMake 自动处理 moc
set(CMAKE_AUTOMOC ON)

# 语言标准不用全局 CMAKE_CXX_STANDARD,统一走 target_compile_features(target-based)
# Network:QTcpSocket/QTcpServer(S2 transport 与 simulator)
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Network Test)

enable_testing()

add_subdirectory(src)
add_subdirectory(tests)
