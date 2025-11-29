all: test mtest stress_test

test: ../my_vm.h test.c
	gcc test.c -L../ -lmy_vm -o test

mtest: ../my_vm.h multi_test.c
	gcc multi_test.c -L../ -lmy_vm -o mtest -lpthread

stress_test: ../my_vm.h stress_test.c
	gcc stress_test.c -L../ -lmy_vm -o stress_test -lpthread

stress_test64: stress_test64.c
	$(CC) -g stress_test64.c -I.. -L.. -lmy_vm64 -lpthread -o stress_test64
clean:
	rm -rf test mtest stress_test
