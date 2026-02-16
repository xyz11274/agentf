# JAR Test Files CMake Configuration

cmake_minimum_required(VERSION 3.10)
project(agentflow_test_jars)

# Copy JAR test files to build directory

install(FILES
    HotReloadTest.java
    MANIFEST.MF
    build.sh
    DESTINATION jars
    PERMISSIONS OWNER_READ OWNER_WRITE
                GROUP_READ GROUP_WRITE
                WORLD_READ WORLD_WRITE
)
