# Test Scripts CMake Configuration

cmake_minimum_required(VERSION 3.10)
project(agentflow_test_scripts)

# Install all test scripts to build directory

# Shell scripts
install(FILES
    shell/test_script.sh
    DESTINATION scripts/shell
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                GROUP_READ GROUP_EXECUTE
                WORLD_READ WORLD_EXECUTE
)

# Python scripts
install(FILES
    python/test_python.py
    DESTINATION scripts/python
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                GROUP_READ GROUP_EXECUTE
                WORLD_READ WORLD_EXECUTE
)
