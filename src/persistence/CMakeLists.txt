# 持久化层:SQLite 连接/建库/查询 + DB 线程 worker(LLD §7)
# 依赖方向(HLD §1):persistence 只用领域值类型与 common 基础设施,由 app 组合
add_library(dms_persistence STATIC
    Database.cpp
)

target_include_directories(dms_persistence PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_features(dms_persistence PUBLIC cxx_std_17)
target_link_libraries(dms_persistence PUBLIC dms_domain Qt6::Sql)
