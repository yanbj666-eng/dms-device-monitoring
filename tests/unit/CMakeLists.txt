# 单元测试:每个被测模块一个可执行文件,统一注册进 CTest
add_executable(tst_crc32 tst_crc32.cpp)

target_compile_features(tst_crc32 PRIVATE cxx_std_17)
target_link_libraries(tst_crc32 PRIVATE dms_protocol Qt6::Test)

# /utf-8 已随 dms_protocol PUBLIC 传播,这里无需重复

add_test(NAME tst_crc32 COMMAND tst_crc32)
