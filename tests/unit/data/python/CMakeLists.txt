# Python Test Scripts CMake Configuration

cmake_minimum_required(VERSION 3.10)
project(agentflow_test_python_scripts)

# Install python test script
install(FILES
    test_python.py
    DESTINATION scripts/python
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                GROUP_READ GROUP_EXECUTE
                WORLD_READ WORLD_EXECUTE
)
