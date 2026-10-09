#define _GNU_SOURCE

#include <dlfcn.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>

static void *(*next_memcpy)(void *, const void *, size_t);
static void *(*next_memmove)(void *, const void *, size_t);
static int barrier_claimed;

__attribute__((constructor)) static void InitializeBarrier(void) {
	next_memcpy = dlsym(RTLD_NEXT, "memcpy");
	next_memmove = dlsym(RTLD_NEXT, "memmove");
}

static void WaitAtBarrier(const void *source, size_t bytes) {
	const unsigned char *pixel = (const unsigned char *)source;
	if (bytes != 800u * 600u * 4u || syscall(SYS_gettid) == getpid() ||
		pixel[0] != 255 || pixel[1] != 0 || pixel[2] != 0 || pixel[3] != 255) return;
	const char *armed = getenv("JPEGVIEW_TEST_FRAME_BARRIER_ARMED");
	const char *started = getenv("JPEGVIEW_TEST_FRAME_BARRIER_STARTED");
	const char *release = getenv("JPEGVIEW_TEST_FRAME_BARRIER_RELEASE");
	if (armed == NULL || started == NULL || release == NULL ||
		access(armed, F_OK) != 0 ||
		!__sync_bool_compare_and_swap(&barrier_claimed, 0, 1)) return;
	const int marker = open(started, O_CREAT | O_WRONLY, 0600);
	if (marker >= 0) close(marker);
	for (int attempt = 0; attempt < 4000 && access(release, F_OK) != 0; ++attempt) {
		usleep(5000);
	}
}

void *memcpy(void *destination, const void *source, size_t bytes) {
	if (next_memcpy == NULL) next_memcpy = dlsym(RTLD_NEXT, "memcpy");
	WaitAtBarrier(source, bytes);
	return next_memcpy(destination, source, bytes);
}

void *memmove(void *destination, const void *source, size_t bytes) {
	if (next_memmove == NULL) next_memmove = dlsym(RTLD_NEXT, "memmove");
	WaitAtBarrier(source, bytes);
	return next_memmove(destination, source, bytes);
}
