CC = gcc

all: test mtest stress_test stress_test64 stresstest1 stresstest2

test: ../my_vm.h test.c
	$(CC) test.c -I.. -L.. -lmy_vm -o test

mtest: ../my_vm.h multi_test.c
	$(CC) multi_test.c -I.. -L.. -lmy_vm -lpthread -o mtest

stress_test: ../my_vm.h stress_test.c
	$(CC) stress_test.c -I.. -L.. -lmy_vm -lpthread -o stress_test

stress_test64: stress_test64.c
	$(CC) -g stress_test64.c -I.. -L.. -lmy_vm64 -lpthread -o stress_test64

stresstest1: ../my_vm3.h stresstest1.c
	$(CC) -g stresstest1.c -I.. -L.. -lmy_vm3 -lpthread -o stresstest1

stresstest2: ../my_vm3.h stresstest2.c
	$(CC) -g stresstest2.c -I.. -L.. -lmy_vm3 -lpthread -o stresstest2

clean:
	rm -rf test mtest stress_test stress_test64 stresstest1 stresstest2
