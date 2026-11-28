add_executable(tcpbench tcpbench.c)
set_module_type(tcpbench win32cui)
add_importlibs(tcpbench ws2_32 msvcrt kernel32)
