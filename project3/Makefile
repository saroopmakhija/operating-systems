# CC = gcc
# CFLAGS = -g -c #-m32
# AR = ar -rc
# RANLIB = ranlib

# all: my_vm.a

# my_vm.a: my_vm.o
# 	$(AR) libmy_vm.a my_vm.o
# 	$(RANLIB) libmy_vm.a

# my_vm.o: my_vm.h

# 	$(CC)	$(CFLAGS)  my_vm.c

# clean:
# 	rm -rf *.o *.a


CC = gcc
CFLAGS = -g -c #-m32
AR = ar -rc
RANLIB = ranlib

all: my_vm.a test_vm

my_vm.a: my_vm.o
	$(AR) libmy_vm.a my_vm.o
	$(RANLIB) libmy_vm.a

my_vm.o: my_vm.h

	$(CC)	$(CFLAGS)  my_vm.c

test_vm: test_vm.c my_vm.a
	$(CC) -g -o test_vm test_vm.c -L. -lmy_vm -lpthread

clean:
	rm -rf *.o *.a test_vm
