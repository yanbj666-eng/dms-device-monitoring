# src/:按 HLD 分层逐阶段加入;依赖方向 ui -> app -> domain,
# transport/protocol/persistence 只依赖领域值类型/接口(HLD §1)
add_subdirectory(common)
add_subdirectory(protocol)
add_subdirectory(transport)
add_subdirectory(domain)
add_subdirectory(ui)
add_subdirectory(app)
