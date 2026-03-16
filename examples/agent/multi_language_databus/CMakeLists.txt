# CMakeLists.txt for Multi-Language DataBus Test Agent
cmake_minimum_required(VERSION 3.14)

# Build SO libraries for C++ nodes
add_library(chain_cpp_node SHARED
    nodes/chain_cpp_node.cpp
)
target_link_libraries(chain_cpp_node PRIVATE agentflow)

add_library(chain_coordinator_node SHARED
    nodes/chain_coordinator_node.cpp
)
target_link_libraries(chain_coordinator_node PRIVATE agentflow)

# Install libraries to lib directory
install(TARGETS chain_cpp_node chain_coordinator_node
    LIBRARY DESTINATION lib
)