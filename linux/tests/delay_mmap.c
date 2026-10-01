#define _GNU_SOURCE

#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <fcntl.h>
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
		const char* started = getenv("JPEGVIEW_TEST_SLOW_MAP_STARTED");
		const char* release = getenv("JPEGVIEW_TEST_SLOW_MAP_RELEASE");
		if (started == NULL || release == NULL) return;
		const char* active = getenv("JPEGVIEW_TEST_SLOW_MAP_ACTIVE");
		if (active != NULL) {
			FILE* marker = fopen(active, "w");
			if (marker != NULL) {
				fputs("blocked\n", marker);
				fclose(marker);
			}
		}
		FILE* signal = fopen(started, "w");
		if (signal == NULL) return;
		fputs("started\n", signal);
		fclose(signal);
		// The smoke harness releases this map only after it observes the visible
		// loading window. The bounded poll prevents a failed harness from hanging.
		int maximum_attempts = 2000;
		const char* attempts_text = getenv("JPEGVIEW_TEST_SLOW_MAP_MAX_ATTEMPTS");
		if (attempts_text != NULL) {
			char* end = NULL;
			const long requested_attempts = strtol(attempts_text, &end, 10);
			if (end != attempts_text && *end == '\0' && requested_attempts > 0 &&
				requested_attempts <= 12000) {
				maximum_attempts = (int)requested_attempts;
			}
		}
		for (int attempt = 0; attempt < maximum_attempts; ++attempt) {
			if (access(release, F_OK) == 0) break;
			usleep(5000);
		}
		if (active != NULL) unlink(active);
	}
}

void* mmap(void* address, size_t length, int protection, int flags,
	int file_descriptor, off_t offset) {
	static mmap_function next_mmap = NULL;
	if (next_mmap == NULL) next_mmap = (mmap_function)dlsym(RTLD_NEXT, "mmap");
	delay_target(file_descriptor);
	return next_mmap(address, length, protection, flags, file_descriptor, offset);
}
