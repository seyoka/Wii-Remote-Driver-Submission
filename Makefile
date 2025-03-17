
obj-m += wii-remote-driver.o


wii-remote-mod-objs := wii-remote-driver.o


all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

# Clean up compiled files
clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
