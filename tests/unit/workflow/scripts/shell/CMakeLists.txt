# Shell Test Scripts CMake Configuration

cmake_minimum_required(VERSION 3.10)
project(agentflow_test_shell_scripts)

# Install shell test script
install(FILES
    test_script.sh
    DESTINATION scripts/shell
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                GROUP_READ GROUP_EXECUTE
                WORLD_READ WORLD_EXECUTE
)
