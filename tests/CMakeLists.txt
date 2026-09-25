# tests/:单元测试链接被测模块本身,不经过应用层组装;
# 集成测试拉起真实模拟器进程走全链路
add_subdirectory(unit)
add_subdirectory(integration)
