# ============================================================================
# SOFTWARE: OSL: Sovereign Accounting Suite
# AUTHOR & COPYRIGHT: Cel-Tech-Serv Pty Ltd
# MODULE: Landlord Plugin Build System
# ============================================================================

cmake_minimum_required(VERSION 3.10)
project(osl_plugin_landlord VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- BRAND PROTECTION ---
add_definitions(-DOSL_VENDOR="Cel-Tech-Serv Pty Ltd")
add_definitions(-DOSL_PLUGIN_NAME="Landlord Identity Manager")

# --- DEPENDENCIES ---
# Find PostgreSQL and libpqxx
find_package(PkgConfig REQUIRED)
pkg_check_modules(PQXX REQUIRED libpqxx)

# Include the OSL Plugin SDK interface
include_directories(
    ../../interface
    ${PQXX_INCLUDE_DIRS}
)

# --- TARGET ---
# Compile as a shared library (.so)
add_library(landlord SHARED src/landlord.cpp)

# Link against libpqxx
target_link_libraries(landlord PRIVATE ${PQXX_LIBRARIES})

# --- INSTALLATION ---
# Ensure the output is named landlord.so and placed in the bin directory
set_target_properties(landlord PROPERTIES 
    PREFIX "" 
    SUFFIX ".so"
    OUTPUT_NAME "landlord"
)

# Note: In production, the build artifacts should be moved to /plugins/bin/
# as defined in the osl-manifest.json