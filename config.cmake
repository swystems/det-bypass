# create a variable SERVER; if given in the command-line, the given value will be used; otherwise False will be used
option(SERVER "Compile for the server machine" False)
add_definitions(-DSERVER=${SERVER})

if (SERVER)
    message(STATUS "Compiling for the server machine")
else ()
    message(STATUS "Compiling for the client machine")
endif ()

option(DEBUG "Compile with debug information" False)
add_definitions(-DDEBUG=${DEBUG})

if (DEBUG)
    message(STATUS "Compiling with debug information")
endif ()

