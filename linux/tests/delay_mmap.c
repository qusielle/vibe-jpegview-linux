#define _GNU_SOURCE

#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

typedef void* (*mmap_function)(void*, size_t, int, int, int, off_t);
typedef int (*open_function)(const char*, int, ...);
typedef FILE* (*fopen_function)(const char*, const char*);

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

static const char* delay_target(int file_descriptor);

static void wait_for_matching_open(int file_descriptor) {
	const char* active = delay_target(file_descriptor);
	mark_map_complete(active);
}

static const char* delay_target(int file_descriptor) {
	const char* target = getenv("JPEGVIEW_TEST_SLOW_MAP");
	const char* path_prefix = getenv("JPEGVIEW_TEST_SLOW_MAP_PREFIX");
	if ((target == NULL || *target == '\0') &&
		(path_prefix == NULL || *path_prefix == '\0')) return NULL;
	if (file_descriptor < 0) return NULL;
	char descriptor_path[64];
	char pathname[PATH_MAX];
	snprintf(descriptor_path, sizeof(descriptor_path), "/proc/self/fd/%d", file_descriptor);
	const ssize_t length = readlink(descriptor_path, pathname, sizeof(pathname) - 1);
	if (length < 0) return NULL;
	pathname[length] = '\0';
	const int exact_match = target != NULL && strcmp(pathname, target) == 0;
	const int prefix_match = path_prefix != NULL && *path_prefix != '\0' &&
		strncmp(pathname, path_prefix, strlen(path_prefix)) == 0;
	if (!exact_match && !prefix_match) return NULL;
	const char* repeat = getenv("JPEGVIEW_TEST_SLOW_MAP_REPEAT");
	// Cold-load tests hold concurrent metadata and thumbnail maps for the same source.
	if (repeat == NULL || strcmp(repeat, "1") != 0) {
		unsetenv("JPEGVIEW_TEST_SLOW_MAP");
		unsetenv("JPEGVIEW_TEST_SLOW_MAP_PREFIX");
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

int open(const char* pathname, int flags, ...) {
	static open_function next_open = NULL;
	if (next_open == NULL) next_open = (open_function)dlsym(RTLD_NEXT, "open");
	const int needs_mode = (flags & O_CREAT) != 0;
	int file_descriptor = -1;
	if (needs_mode) {
		va_list arguments;
		va_start(arguments, flags);
		const mode_t mode = va_arg(arguments, mode_t);
		va_end(arguments);
		file_descriptor = next_open(pathname, flags, mode);
	} else {
		file_descriptor = next_open(pathname, flags);
	}
	if (file_descriptor >= 0) wait_for_matching_open(file_descriptor);
	return file_descriptor;
}

int open64(const char* pathname, int flags, ...) {
	static open_function next_open64 = NULL;
	if (next_open64 == NULL) next_open64 = (open_function)dlsym(RTLD_NEXT, "open64");
	const int needs_mode = (flags & O_CREAT) != 0;
	int file_descriptor = -1;
	if (needs_mode) {
		va_list arguments;
		va_start(arguments, flags);
		const mode_t mode = va_arg(arguments, mode_t);
		va_end(arguments);
		file_descriptor = next_open64(pathname, flags, mode);
	} else {
		file_descriptor = next_open64(pathname, flags);
	}
	if (file_descriptor >= 0) wait_for_matching_open(file_descriptor);
	return file_descriptor;
}

FILE* fopen(const char* pathname, const char* mode) {
	static fopen_function next_fopen = NULL;
	if (next_fopen == NULL) next_fopen = (fopen_function)dlsym(RTLD_NEXT, "fopen");
	FILE* stream = next_fopen(pathname, mode);
	if (stream != NULL) wait_for_matching_open(fileno(stream));
	return stream;
}

FILE* fopen64(const char* pathname, const char* mode) {
	static fopen_function next_fopen64 = NULL;
	if (next_fopen64 == NULL) next_fopen64 = (fopen_function)dlsym(RTLD_NEXT, "fopen64");
	FILE* stream = next_fopen64(pathname, mode);
	if (stream != NULL) wait_for_matching_open(fileno(stream));
	return stream;
}
