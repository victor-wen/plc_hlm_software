# Integration tests (spec §15.4). Task 6 adds the in-process
# SimulatedPlcGateway flow tests; later tasks add the full application
# integration tests here.

add_executable(test_simulated_gateway_flow test_simulated_gateway_flow.cpp)
target_link_libraries(test_simulated_gateway_flow PRIVATE
    hlm_simulator
    Qt6::Test
)

add_test(NAME simulated_gateway_flow COMMAND test_simulated_gateway_flow)
