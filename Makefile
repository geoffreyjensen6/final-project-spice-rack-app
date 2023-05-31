CC ?= gcc
CROSS_CC ?= arm-linux-gnueabihf-
SRC ?= spice_rack_app.c
OBJ ?= spice_rack_app
CROSS_COMPILE ?= none
CFLAGS ?= -g -Wall -Werror
LDFLAGS ?= -pthread -lrt

default: $(SRC)
ifeq ($(CROSS_COMPILE), $(CROSS_CC))
	$(CROSS_CC)$(CC) $(CFLAGS) -o $(OBJ) $(SRC) ${LDFLAGS)
else
	$(CC) $(CFLAGS) -o $(OBJ) $(SRC) ${LDFLAGS}
endif

clean:
	rm -f $(OBJ)
	rm -f *.o *.elf
