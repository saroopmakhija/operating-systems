CC     = gcc
CFLAGS = -g -c
AR     = ar -rc
RANLIB = ranlib

all: my_vm.a my_vm64.a my_vm3.a

# 32-bit style library
my_vm.a: my_vm.o
	$(AR) libmy_vm.a my_vm.o
	$(RANLIB) libmy_vm.a

my_vm.o: my_vm.c my_vm.h
	$(CC) $(CFLAGS) my_vm.c

# 64-bit extra-credit library
my_vm64.a: my_vm64.o
	$(AR) libmy_vm64.a my_vm64.o
	$(RANLIB) libmy_vm64.a

my_vm64.o: my_vm64.c my_vm64.h
	$(CC) $(CFLAGS) my_vm64.c

# Third internal fragmenation fix library
my_vm3.a: my_vm3.o
	$(AR) libmy_vm3.a my_vm3.o
	$(RANLIB) libmy_vm3.a

my_vm3.o: my_vm3.c my_vm3.h
	$(CC) $(CFLAGS) my_vm3.c


clean:
	rm -rf *.o *.a