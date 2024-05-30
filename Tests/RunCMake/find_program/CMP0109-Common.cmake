file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/ExeNoRead" "#!/bin/sh\n")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/ReadNoExe" "#ReadNoExe")
execute_process(COMMAND chmod -r+x "${CMAKE_CURRENT_BINARY_DIR}/ExeNoRead")
find_program(ExeNoRead_EXECUTABLE NAMES ExeNoRead NO_DEFAULT_PATH PATHS "${CMAKE_CURRENT_BINARY_DIR}")
message(STATUS "ExeNoRead_EXECUTABLE='${ExeNoRead_EXECUTABLE}'")
find_program(ReadNoExe_EXECUTABLE NAMES ReadNoExe NO_DEFAULT_PATH PATHS "${CMAKE_CURRENT_BINARY_DIR}")
message(STATUS "ReadNoExe_EXECUTABLE='${ReadNoExe_EXECUTABLE}'")
