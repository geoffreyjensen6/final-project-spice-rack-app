#Author: Geoffrey Jensen
#ECEA 5305 Assignment 2
#Date: 1/24/2023

CC ?= gcc
CROSS_CC ?= aarch64-none-linux-gnu-
SRC ?= aesdsocket_client.c
OBJ ?= aesdsocket_client
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
