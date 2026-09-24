# 协议层:帧边界、CRC、载荷编解码
# 约束(HLD §1):不依赖 socket/SQL/UI;仅依赖 Qt Core 值类型
add_library(dms_protocol STATIC
    Crc32.cpp
    Frame.cpp
    FrameDecoder.cpp
)

target_include_directories(dms_protocol PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(dms_protocol PUBLIC cxx_std_17)
target_link_libraries(dms_protocol PUBLIC Qt6::Core)

# MSVC 默认按本地代码页解释源文件,必须强制 UTF-8,
# 否则中文注释在中文 Windows(GBK)与 Ubuntu(UTF-8)下行为不一致
target_compile_options(dms_protocol PUBLIC
    "$<$<CXX_COMPILER_ID:MSVC>:/utf-8;/W4>"
    "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra>"
)
