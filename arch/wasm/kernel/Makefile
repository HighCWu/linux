extra-y := vmlinux.lds

obj-y += \
	binfmt_wasm.o \
	cpuinfo.o \
	early_printk.o \
	fork.o \
	globals.o \
	idle.o \
	irq.o \
	process.o \
	ptrace.o \
	remote_vm.o \
	semaphore.o \
	sections.o \
	setup.o \
	signal.o \
	stacktrace.o \
	syscall.o \
	time.o \
	user_context.o

obj-$(CONFIG_SMP) += smp.o
obj-$(CONFIG_WASM_MMAP_COPY_TEST) += mmap_test.o
obj-$(CONFIG_WASM_FILE_COPY_CORE) += mmap_file.o
