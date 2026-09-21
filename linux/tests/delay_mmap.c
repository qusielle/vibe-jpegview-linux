#define _GNU_SOURCE

#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

typedef void* (*mmap_function)(void*, size_t, int, int, int, off_t);

static void delay_target(int file_descriptor) {
	const char* target = getenv("JPEGVIEW_TEST_SLOW_MAP");
	if (target == NULL || file_descriptor < 0) return;
	char descriptor_path[64];
	char pathname[PATH_MAX];
	snprintf(descriptor_path, sizeof(descriptor_path), "/proc/self/fd/%d", file_descriptor);
	const ssize_t length = readlink(descriptor_path, pathname, sizeof(pathname) - 1);
	if (length < 0) return;
	pathname[length] = '\0';
	if (strcmp(pathname, target) == 0) {
		unsetenv("JPEGVIEW_TEST_SLOW_MAP");
		usleep(3000000);
	}
}

void* mmap(void* address, size_t length, int protection, int flags,
	int file_descriptor, off_t offset) {
	static mmap_function next_mmap = NULL;
	if (next_mmap == NULL) next_mmap = (mmap_function)dlsym(RTLD_NEXT, "mmap");
	delay_target(file_descriptor);
	return next_mmap(address, length, protection, flags, file_descriptor, offset);
}
