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


CC     = gcc
CFLAGS = -g -c
AR     = ar -rc
RANLIB = ranlib

all: my_vm.a my_vm64.a test_vm stress_test64

# 32-bit style library (your original project)
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

# Existing 32-bit test program
test_vm: test_vm.c my_vm.a
	$(CC) -g -o test_vm test_vm.c -L. -lmy_vm -lpthread

# New 64-bit stress test
stress_test64: stress_test64.c my_vm64.a
	$(CC) -g -o stress_test64 stress_test64.c -L. -lmy_vm64 -lpthread

clean:
	rm -rf *.o *.a test_vm stress_test64
