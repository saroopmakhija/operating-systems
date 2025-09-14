CC = gcc
CFLAGS = -Wall -Wextra -O2

# Targets
all: thread bitops ucontext

threads: threads.c
	$(CC) $(CFLAGS) -lpthread -o threads threads.c

bitops: bitops.c
	$(CC) $(CFLAGS) -o bitops bitops.c

ucontext: ucontext.c
	$(CC) $(CFLAGS) -o ucontext ucontext.c

clean:
	rm -f thread bitops ucontext
