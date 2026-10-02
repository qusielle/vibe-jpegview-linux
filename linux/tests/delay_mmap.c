#define _GNU_SOURCE

#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

typedef void* (*mmap_function)(void*, size_t, int, int, int, off_t);

static pthread_mutex_t active_map_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned int active_map_count;

static void mark_map_active(const char* pathname) {
	if (pathname == NULL) return;
	pthread_mutex_lock(&active_map_mutex);
	if (active_map_count++ == 0) {
		FILE* marker = fopen(pathname, "w");
		if (marker != NULL) {
			fputs("blocked\n", marker);
			fclose(marker);
		}
	}
	pthread_mutex_unlock(&active_map_mutex);
}

static void mark_map_complete(const char* pathname) {
	if (pathname == NULL) return;
	pthread_mutex_lock(&active_map_mutex);
	if (active_map_count > 0 && --active_map_count == 0) unlink(pathname);
	pthread_mutex_unlock(&active_map_mutex);
}

static const char* delay_target(int file_descriptor) {
	const char* target = getenv("JPEGVIEW_TEST_SLOW_MAP");
	if (target == NULL || file_descriptor < 0) return NULL;
	char descriptor_path[64];
	char pathname[PATH_MAX];
	snprintf(descriptor_path, sizeof(descriptor_path), "/proc/self/fd/%d", file_descriptor);
	const ssize_t length = readlink(descriptor_path, pathname, sizeof(pathname) - 1);
	if (length < 0) return NULL;
	pathname[length] = '\0';
	if (strcmp(pathname, target) != 0) return NULL;
	const char* repeat = getenv("JPEGVIEW_TEST_SLOW_MAP_REPEAT");
	// Cold-load tests hold concurrent metadata and thumbnail maps for the same source.
	if (repeat == NULL || strcmp(repeat, "1") != 0) {
		unsetenv("JPEGVIEW_TEST_SLOW_MAP");
	}
	const char* started = getenv("JPEGVIEW_TEST_SLOW_MAP_STARTED");
	const char* release = getenv("JPEGVIEW_TEST_SLOW_MAP_RELEASE");
	if (started == NULL || release == NULL) return NULL;
	const char* active = getenv("JPEGVIEW_TEST_SLOW_MAP_ACTIVE");
	mark_map_active(active);
	FILE* signal = fopen(started, "w");
	if (signal != NULL) {
		fputs("started\n", signal);
		fclose(signal);
	}
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
	return active;
}

void* mmap(void* address, size_t length, int protection, int flags,
	int file_descriptor, off_t offset) {
	static mmap_function next_mmap = NULL;
	if (next_mmap == NULL) next_mmap = (mmap_function)dlsym(RTLD_NEXT, "mmap");
	const char* active_marker = delay_target(file_descriptor);
	void* result = next_mmap(address, length, protection, flags, file_descriptor, offset);
	mark_map_complete(active_marker);
	return result;
}
