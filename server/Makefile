CC ?= $(CROSS_COMPILE)gcc
CFLAGS ?= -Wall -Werror -g
LDFLAGS ?= -pthread -lrt
TARGET ?= aesdsocket
SRCS = aesdsocket.c
OBJS = $(SRCS:.c=.o)

.PHONY: all default clean

all: $(TARGET)
default: all

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@
clean:
	rm -f $(TARGET) $(OBJS)
