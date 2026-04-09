obj-m += monitor.o

all:
	gcc engine.c -o engine
	gcc cpu_bound.c -o cpu_bound
	gcc io_bound.c -o io_bound
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	rm -f engine cpu_bound io_bound
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean