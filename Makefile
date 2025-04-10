TARGET = psxsplash
TYPE = ps-exe

SRCS = \
src/main.cpp \
src/renderer.cpp \
src/splashpack.cpp \
src/camera.cpp \
src/gtemath.cpp \
src/navmesh.cpp \
output.o

include third_party/nugget/psyqo/psyqo.mk
include third_party/nugget/psyqo-lua/psyqo-lua.mk

%.o: %.bin
	$(PREFIX)-objcopy -I binary --set-section-alignment .data=4 --rename-section .data=.rodata,alloc,load,readonly,data,contents -O $(FORMAT) -B mips $< $@
