TARGET = psxsplash
TYPE = ps-exe

SRCS = \
src/main.cpp \
src/renderer.cpp \
src/splashpack.cpp \
src/camera.cpp \
src/gtemath.cpp \
output.o

include third_party/nugget/psyqo/psyqo.mk

%.o: %.bin
	$(PREFIX)-objcopy -I binary --set-section-alignment .data=4 --rename-section .data=.rodata,alloc,load,readonly,data,contents -O $(FORMAT) -B mips $< $@
