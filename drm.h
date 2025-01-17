
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define BUFCOUNT 4

struct drm_buffer_t {
	uint32_t pitch, size;

	uint32_t fb_id;
	int dmabuf_fd;
	int bo_handle;
	uint32_t *buf;
};

struct drm_dev_t {
	uint32_t conn_id, enc_id, crtc_id;
	uint32_t width, height, pitch;
	drmModeModeInfo mode;
	drmModeCrtc *saved_crtc;
	struct drm_dev_t *next;

	int v4l2_fd[2];
	int drm_fd;

	drmModePlaneRes *plane_res;
	struct drm_buffer_t bufs[BUFCOUNT];
	struct drm_buffer_t plane1bufs[BUFCOUNT];	
};

inline static void fatal(char *str)
{
	fprintf(stderr, "%s\n", str);
	exit(EXIT_FAILURE);
}

inline static void error(char *str)
{
	perror(str);
	exit(EXIT_FAILURE);
}

int drm_open(const char *path, int need_dumb, int need_prime);
struct drm_dev_t *drm_find_dev(int fd);
void drm_setup_dummy(int fd, struct drm_dev_t *dev, int map, int export);
void drm_setup_fb(int fd, struct drm_dev_t *dev, int map, int export);
void drm_destroy(int fd, struct drm_dev_t *dev_head);
