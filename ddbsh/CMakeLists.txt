# set(CMAKE_VERBOSE_MAKEFILE ON)

cmake_minimum_required(VERSION 3.21)
set(CMAKE_C_COMPILER "gcc")
set(CMAKE_CXX_COMPILER "g++")

# add_compile_options(-g)
# add_link_options(-lc -g)

option(BUILD_SHARED_LIBS "Build shared libraries" ON)

project(ddbsh
        VERSION 0.6.0.0
        DESCRIPTION "A DynamoDB interactive shell"
        HOMEPAGE_URL "https://www.ddbsh.org/")

find_package(AWSSDK REQUIRED COMPONENTS dynamodb)
find_package(BISON)
find_package(FLEX)
find_package(ZLIB)

configure_file(version.h.in version.h)

# flex generates code that includes the register hint and this is not
# valid after CXX_STANDARD 11 so make the lexer a library, and set the
# compile standard to 11 for that library. Add the library to the
# target link libraries.

# add_library(Lexer STATIC ${CMAKE_CURRENT_BINARY_DIR}/lexer.cpp)
# set_property(TARGET Lexer PROPERTY CXX_STANDARD 11)

include_directories(command)
include_directories(dynamodb)
include_directories(ddbsh)
include_directories(parser)
include_directories(util)
include_directories(include)
include_directories(expressions)

FLEX_TARGET(ddbsh_Scanner
            parser/lexer.l
            ${CMAKE_CURRENT_BINARY_DIR}/lexer.cpp)

BISON_TARGET(ddbsh_Parser
             parser/parser.y
             ${CMAKE_CURRENT_BINARY_DIR}/parser.cpp VERBOSE ${CMAKE_CURRENT_BINARY_DIR}/parser-verbose.txt)

ADD_FLEX_BISON_DEPENDENCY(ddbsh_Scanner ddbsh_Parser)

add_executable(ddbsh
               ${BISON_ddbsh_Parser_OUTPUTS}
               ${CMAKE_CURRENT_BINARY_DIR}/parser.cpp
               ${FLEX_ddbsh_Scanner_OUTPUTS}
               command/connect.cpp
               command/create_backup.cpp
               command/create_table.cpp
               command/delete.cpp
               command/describe.cpp
               command/describe_backup.cpp
               command/drop_backup.cpp
               command/drop_table.cpp
               command/insert.cpp
               command/pitr.cpp
               command/quit.cpp
               command/restore_backup.cpp
               command/restore_pitr.cpp
               command/select.cpp
               command/show_backups.cpp
               command/show_limits.cpp
               command/show_tables.cpp
               command/show_create_table.cpp
               command/update.cpp
               command/update_table.cpp
               command/update_table_ttl.cpp
               dynamodb/describe_helper.cpp
               dynamodb/get_key_schema.cpp
               dynamodb/ddbsh_ddb_client_config.cpp
               dynamodb/select_helper.cpp
               dynamodb/serialize_attribute.cpp
               dynamodb/table_exists.cpp
               ddbsh/ddbsh.cpp
               ddbsh/main.cpp
               ddbsh/version.cpp
               parser/input-source.cpp
               parser/keywords.cpp
               util/logging.cpp
               util/strip_newlines.cpp
               util/trim_double.cpp
               util/unquote.cpp)

add_executable(ddbshtest
               ${BISON_ddbsh_Parser_OUTPUTS}
               ${CMAKE_CURRENT_BINARY_DIR}/parser.cpp
               ${FLEX_ddbsh_Scanner_OUTPUTS}
               command/connect.cpp
               command/create_backup.cpp
               command/create_table.cpp
               command/delete.cpp
               command/describe.cpp
               command/describe_backup.cpp
               command/drop_backup.cpp
               command/drop_table.cpp
               command/insert.cpp
               command/pitr.cpp
               command/quit.cpp
               command/restore_backup.cpp
               command/restore_pitr.cpp
               command/select.cpp
               command/show_backups.cpp
               command/show_limits.cpp
               command/show_tables.cpp
               command/show_create_table.cpp
               command/update.cpp
               command/update_table.cpp
               command/update_table_ttl.cpp
               dynamodb/describe_helper.cpp
               dynamodb/get_key_schema.cpp
               dynamodb/ddbsh_ddb_client_config.cpp
               dynamodb/select_helper.cpp
               dynamodb/serialize_attribute.cpp
               dynamodb/table_exists.cpp
               ddbsh/ddbsh.cpp
               ddbsh/main.cpp
               ddbsh/version.cpp
               parser/input-source.cpp
               parser/keywords.cpp
               util/allocation.cpp
               util/ll.cpp
               util/logging.cpp
               util/strip_newlines.cpp
               util/trim_double.cpp
               util/unquote.cpp)

set_target_properties(ddbshtest PROPERTIES COMPILE_DEFINITIONS "TEST_HARNESS=1")

target_compile_features(ddbsh PUBLIC cxx_std_11)
target_compile_features(ddbshtest PUBLIC cxx_std_11)

target_link_libraries(ddbsh readline ${AWSSDK_LINK_LIBRARIES})
target_include_directories(ddbsh PUBLIC "${PROJECT_BINARY_DIR}")

target_link_libraries(ddbshtest readline ${AWSSDK_LINK_LIBRARIES})
target_include_directories(ddbshtest PUBLIC "${PROJECT_BINARY_DIR}")

include(${CMAKE_SOURCE_DIR}/tests/ddbsh-tests.txt)

install(TARGETS ddbsh DESTINATION bin)
