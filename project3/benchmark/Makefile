
all : test
test: ../my_vm.h
	gcc test.c -L../ -lmy_vm -o test
	gcc multi_test.c -L../ -lmy_vm -o mtest -lpthread 

clean:
	rm -rf test mtest
