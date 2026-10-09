#define _GNU_SOURCE

#include <dlfcn.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>

static void *(*next_memcpy)(void *, const void *, size_t);
static void *(*next_memmove)(void *, const void *, size_t);
static int (*next_update_texture)(void *, const void *, const void *, int);
static void (*next_render_present)(void *);
static int barrier_claimed;
static int operation_barrier_claimed;
static int upload_failure_claimed;
static int upload_failure_presented;

__attribute__((constructor)) static void InitializeBarrier(void) {
	next_memcpy = dlsym(RTLD_NEXT, "memcpy");
	next_memmove = dlsym(RTLD_NEXT, "memmove");
	next_update_texture = dlsym(RTLD_NEXT, "SDL_UpdateTexture");
	next_render_present = dlsym(RTLD_NEXT, "SDL_RenderPresent");
}

static void WaitAtBarrier(const void *source, size_t bytes) {
	const unsigned char *pixel = (const unsigned char *)source;
	if (bytes != 800u * 600u * 4u || syscall(SYS_gettid) == getpid() ||
		pixel[1] != 0 || pixel[3] != 255) return;
	const char *armed;
	const char *started;
	const char *release;
	int *claimed;
	if (pixel[0] == 255 && pixel[2] == 0) {
		armed = getenv("JPEGVIEW_TEST_FRAME_BARRIER_ARMED");
		started = getenv("JPEGVIEW_TEST_FRAME_BARRIER_STARTED");
		release = getenv("JPEGVIEW_TEST_FRAME_BARRIER_RELEASE");
		claimed = &barrier_claimed;
	} else if (pixel[0] == 0 && pixel[2] == 255) {
		armed = getenv("JPEGVIEW_TEST_OPERATION_BARRIER_ARMED");
		started = getenv("JPEGVIEW_TEST_OPERATION_BARRIER_STARTED");
		release = getenv("JPEGVIEW_TEST_OPERATION_BARRIER_RELEASE");
		claimed = &operation_barrier_claimed;
	} else {
		return;
	}
	if (armed == NULL || started == NULL || release == NULL ||
		access(armed, F_OK) != 0 ||
		!__sync_bool_compare_and_swap(claimed, 0, 1)) return;
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

int SDL_UpdateTexture(void *texture, const void *rect, const void *pixels, int pitch) {
	if (next_update_texture == NULL) next_update_texture = dlsym(RTLD_NEXT, "SDL_UpdateTexture");
	const unsigned char *pixel = (const unsigned char *)pixels;
	const char *failure = getenv("JPEGVIEW_TEST_FRAME_UPLOAD_FAILURE");
	const char *started = getenv("JPEGVIEW_TEST_OPERATION_BARRIER_STARTED");
	const char *release = getenv("JPEGVIEW_TEST_OPERATION_BARRIER_RELEASE");
	if (pixels != NULL && failure != NULL && started != NULL && release != NULL &&
		access(started, F_OK) == 0 && access(release, F_OK) != 0 &&
		pitch == 800 * 4 && pixel[0] == 255 && pixel[1] == 0 &&
		pixel[2] == 0 && pixel[3] == 255 &&
		__sync_bool_compare_and_swap(&upload_failure_claimed, 0, 1)) {
		return -1;
	}
	return next_update_texture(texture, rect, pixels, pitch);
}

void SDL_RenderPresent(void *renderer) {
	if (next_render_present == NULL) next_render_present = dlsym(RTLD_NEXT, "SDL_RenderPresent");
	next_render_present(renderer);
	if (upload_failure_claimed && !upload_failure_presented) {
		// Presentation follows the failed upload's Viewer failure handling.
		const char *failure = getenv("JPEGVIEW_TEST_FRAME_UPLOAD_FAILURE");
		const int marker = open(failure, O_CREAT | O_WRONLY, 0600);
		if (marker >= 0) close(marker);
		upload_failure_presented = 1;
	}
}
