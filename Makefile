
CFLAGS = -Wall -Wextra

build ?= debug
buildflags = $(build)
buildflags := $(buildflags:debug=-D_DEBUG -O0)
buildflags := $(buildflags:release=-DNDEBUG -O2)
CFLAGS += $(buildflags)

libfmd_sources = fmd.c fmd_priv.c fmd_audio.c fmd_bmff.c
libfmd_objects = $(libfmd_sources:.c=.o)
libfmd_so = libfmd.so.0
libfmd_a = libfmd.a

fmdscan_sources = fmdscan.c
fmdscan_objects = $(fmdscan_sources:.c=.o)
fmdscan = fmdscan

all: $(fmdscan) $(libfmd_so) $(libfmd_a)

clean:
	rm -f $(libfmd_objects) $(libfmd_so) $(libfmd_a) $(fmdscan) $(fmdscan_objects)

$(fmdscan): $(fmdscan_objects) $(libfmd_a)
	$(CC) $(LDFLAGS) -g -o $@ $(fmdscan_objects) -L. -lfmd

$(libfmd_so): $(libfmd_objects)
	$(CC) -fPIC -shared -g $(libfmd_objects) -o $@

$(libfmd_a): $(libfmd_objects)
	$(AR) crs $@ $(libfmd_objects)

fmdscan.o: fmd.h
fmd.o: fmd.c fmd.h fmd_priv.h
fmd_priv.o: fmd_priv.c fmd.h fmd_priv.h
fmd_audio.o: fmd_audio.c fmd.h fmd_priv.h
fmd_bmff.o: fmd_bmff.c fmd.h fmd_priv.h

.c.o:
	$(CC) $(CFLAGS) -g -fPIC -c $< -o $@

test: $(fmdscan)
	lldb -f ./$(fmdscan) -- -rm samples
