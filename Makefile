TARGET = psxsplash
TYPE = ps-exe

SRCS = \
src/main.cpp \
src/renderer.cpp \
src/splashpack.cpp

include third_party/nugget/psyqo/psyqo.mk
