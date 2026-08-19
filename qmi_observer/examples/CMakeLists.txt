add_executable(qmi_observer_hello_world hello_world.cpp)
target_link_libraries(qmi_observer_hello_world PRIVATE qcom::qmi)

add_executable(qmi_observer_devices devices.cpp)
target_link_libraries(qmi_observer_devices PRIVATE qcom::qmi)
