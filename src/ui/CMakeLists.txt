# UI 层(S4 最小 UI):只依赖 domain 值类型与 Widgets,不依赖 app(HLD §1)
add_library(dms_ui STATIC
    CurveWidget.cpp
    DeviceListModel.cpp
    MainWindow.cpp
    TelemetryBuffer.cpp
)

target_include_directories(dms_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_features(dms_ui PUBLIC cxx_std_17)
target_link_libraries(dms_ui PUBLIC dms_domain Qt6::Widgets)
