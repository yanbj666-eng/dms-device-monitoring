# 单元测试:每个被测模块一个可执行文件,统一注册进 CTest
add_executable(tst_crc32 tst_crc32.cpp)
target_compile_features(tst_crc32 PRIVATE cxx_std_17)
target_link_libraries(tst_crc32 PRIVATE dms_protocol Qt6::Test)
add_test(NAME tst_crc32 COMMAND tst_crc32)

add_executable(tst_framedecoder tst_framedecoder.cpp)
target_compile_features(tst_framedecoder PRIVATE cxx_std_17)
target_link_libraries(tst_framedecoder PRIVATE dms_protocol Qt6::Test)
add_test(NAME tst_framedecoder COMMAND tst_framedecoder)

add_executable(tst_messagecodec tst_messagecodec.cpp)
target_compile_features(tst_messagecodec PRIVATE cxx_std_17)
target_link_libraries(tst_messagecodec PRIVATE dms_protocol Qt6::Test)
add_test(NAME tst_messagecodec COMMAND tst_messagecodec)

add_executable(tst_logging tst_logging.cpp)
target_compile_features(tst_logging PRIVATE cxx_std_17)
target_link_libraries(tst_logging PRIVATE dms_common Qt6::Test)
add_test(NAME tst_logging COMMAND tst_logging)

# /utf-8 与警告选项已随 dms_protocol PUBLIC 传播
