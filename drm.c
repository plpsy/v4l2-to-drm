#define _GNU_SOURCE
#define _XOPEN_SOURCE 701

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libdrm/drm.h>
#include "drm.h"
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

struct color_rgb32 {
	uint32_t value;
};

struct util_color_component {
        unsigned int length;
        unsigned int offset;
};

struct util_rgb_info {
        struct util_color_component red;
        struct util_color_component green;
        struct util_color_component blue;
        struct util_color_component alpha;
};

#define MAKE_RGB_INFO(rl, ro, gl, go, bl, bo, al, ao) \
        { { (rl), (ro) }, { (gl), (go) }, { (bl), (bo) }, { (al), (ao) } }

#define MAKE_RGBA(rgb, r, g, b, a) \
        ((((r) >> (8 - (rgb)->red.length)) << (rgb)->red.offset) | \
         (((g) >> (8 - (rgb)->green.length)) << (rgb)->green.offset) | \
         (((b) >> (8 - (rgb)->blue.length)) << (rgb)->blue.offset) | \
         (((a) >> (8 - (rgb)->alpha.length)) << (rgb)->alpha.offset))

#define MAKE_RGB24(rgb, r, g, b) \
        { .value = MAKE_RGBA(rgb, r, g, b, 0) }

enum {
	DEPTH = 24,
	BPP = 32,
};

static int eopen(const char *path, int flag)
{
	int fd;

	if ((fd = open(path, flag)) < 0) {
		fprintf(stderr, "cannot open \"%s\"\n", path);
		error("open");
	}
	return fd;
}

static void *emmap(int addr, size_t len, int prot, int flag, int fd, off_t offset)
{
	uint32_t *fp;

	if ((fp = (uint32_t *) mmap(0, len, prot, flag, fd, offset)) == MAP_FAILED)
		error("mmap");
	return fp;
}

int drm_open(const char *path, int need_dumb, int need_prime)
{
	int fd, flags;
	uint64_t has_it;

	fd = eopen(path, O_RDWR);

	/* set FD_CLOEXEC flag */
	if ((flags = fcntl(fd, F_GETFD)) < 0
		|| fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0)
		fatal("fcntl FD_CLOEXEC failed");

	if (need_dumb) {
		if (drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &has_it) < 0)
			error("drmGetCap DRM_CAP_DUMB_BUFFER failed!");
		if (has_it == 0)
			fatal("can't give us dumb buffers");
	}

	if (need_prime) {
		/* check prime */
		if (drmGetCap(fd, DRM_CAP_PRIME, &has_it) < 0)
			error("drmGetCap DRM_CAP_PRIME failed!");
		if (!(has_it & DRM_PRIME_CAP_EXPORT))
			fatal("can't export dmabuf");
	}

	return fd;
}

struct drm_dev_t *drm_find_dev(int fd)
{
	int i, m;
	struct drm_dev_t *dev = NULL, *dev_head = NULL;
	drmModeRes *res;
	drmModeConnector *conn;
	drmModeEncoder *enc;
	drmModeModeInfo *mode = NULL, *preferred = NULL;

	if ((res = drmModeGetResources(fd)) == NULL)
		fatal("drmModeGetResources() failed");

	/* find all available connectors */
	for (i = 0; i < res->count_connectors; i++) {
		conn = drmModeGetConnector(fd, res->connectors[i]);

		if (conn != NULL && conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
			dev = (struct drm_dev_t *) malloc(sizeof(struct drm_dev_t));
			memset(dev, 0, sizeof(struct drm_dev_t));

			/* find preferred mode */
			for (m = 0; m < conn->count_modes; m++) {
				mode = &conn->modes[m];
				if (mode->type & DRM_MODE_TYPE_PREFERRED)
					preferred = mode;
				fprintf(stdout, "mode: %dx%d %s\n", mode->hdisplay, mode->vdisplay, mode->type & DRM_MODE_TYPE_PREFERRED ? "*" : "");
			}

			if (!preferred)
				preferred = &conn->modes[0];

			dev->conn_id = conn->connector_id;
			dev->enc_id = conn->encoder_id;
			dev->next = NULL;

			memcpy(&dev->mode, preferred, sizeof(drmModeModeInfo));
			dev->width = preferred->hdisplay;
			dev->height = preferred->vdisplay;

			/* FIXME: use default encoder/crtc pair */
			if ((enc = drmModeGetEncoder(fd, dev->enc_id)) == NULL)
				fatal("drmModeGetEncoder() faild");
			dev->crtc_id = enc->crtc_id;
			drmModeFreeEncoder(enc);

			dev->saved_crtc = NULL;

			/* create dev list */
			dev->next = dev_head;
			dev_head = dev;
		}
		drmModeFreeConnector(conn);
	}

	drmModeFreeResources(res);

	printf("selected connector(s)\n");
	for (dev = dev_head; dev != NULL; dev = dev->next) {
		printf("connector id:%d\n", dev->conn_id);
		printf("\tencoder id:%d crtc id:%d\n", dev->enc_id, dev->crtc_id);
		printf("\twidth:%d height:%d\n", dev->width, dev->height);
	}

	return dev_head;
}

static void drm_setup_buffer(int fd, struct drm_dev_t *dev,
		int width, int height,
		struct drm_buffer_t *buffer, int map, int export)
{
	struct drm_mode_create_dumb create_req;
	struct drm_mode_map_dumb map_req;

	buffer->dmabuf_fd = -1;

	memset(&create_req, 0, sizeof(struct drm_mode_create_dumb));
	create_req.width = width;
	create_req.height = height;
	create_req.bpp = BPP;

	if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_req) < 0)
		fatal("drmIoctl DRM_IOCTL_MODE_CREATE_DUMB failed");

	buffer->pitch = create_req.pitch;
	buffer->size = create_req.size;
	/* GEM buffer handle */
	buffer->bo_handle = create_req.handle;

	if (export) {
		int ret;

		ret = drmPrimeHandleToFD(fd, buffer->bo_handle,
			DRM_CLOEXEC | DRM_RDWR, &buffer->dmabuf_fd);
		if (ret < 0)
			fatal("could not export the dump buffer");
	}

	if (map) {
		memset(&map_req, 0, sizeof(struct drm_mode_map_dumb));
		map_req.handle = buffer->bo_handle;

		if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map_req))
			fatal("drmIoctl DRM_IOCTL_MODE_MAP_DUMB failed");
		buffer->buf = (uint32_t *) emmap(0, buffer->size,
			PROT_READ | PROT_WRITE, MAP_SHARED,
			fd, map_req.offset);
	}
}

void drm_setup_dummy(int fd, struct drm_dev_t *dev, int map, int export)
{
	int i;

	for (i = 0; i < BUFCOUNT; i++)
		drm_setup_buffer(fd, dev, dev->width, dev->height,
				 &dev->bufs[i], map, export);

	/* Assume all buffers have the same pitch */
	dev->pitch = dev->bufs[0].pitch;
	printf("DRM: buffer pitch = %d bytes\n", dev->pitch);
}

void get_planes_property(int fd, drmModePlaneRes *pr)
{
	drmModeObjectPropertiesPtr props;
	uint32_t i,j;
	drmModePropertyPtr p;
	
	for(i = 0; i < pr->count_planes; i++) {
		
		printf("planes id %d\n",pr->planes[i]);
		props = drmModeObjectGetProperties(fd, pr->planes[i],
					DRM_MODE_OBJECT_PLANE);
		if(props){		
            for (j = 0;j < props->count_props; j++) {
                p = drmModeGetProperty(fd, props->props[j]);
                printf("get property ,name %s, id %d\n",p->name, p->prop_id);
                drmModeFreeProperty(p);
            }	
            
            
            drmModeFreeObjectProperties(props);
		}
		printf("\n\n");
	}
	
}


void drm_setup_fb(int fd, struct drm_dev_t *dev, int map, int export)
{
	int i;
	int ret;

	uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};

	printf("drm dev crtid=%d, connnecter=%d(%d,%d)\n", dev->crtc_id, dev->conn_id, dev->width, dev->height);

	for (i = 0; i < BUFCOUNT; i++) {

		// drm_setup_buffer(fd, dev, dev->width, dev->height,
		// 		 &dev->bufs[i], map, export);
		// 摄像头的分辨率高于显示输出,申请更大缓存		 
		drm_setup_buffer(fd, dev, 1920, 1080,
				 &dev->bufs[i], map, export);

		handles[0] = dev->bufs[i].bo_handle;
		pitches[0] = dev->width*2;	
		offsets[0] = 0;
		#if 0
		ret = drmModeAddFB(fd, dev->width, dev->height,
			DEPTH, BPP, dev->bufs[i].pitch,
			dev->bufs[i].bo_handle, &dev->bufs[i].fb_id);
		if (ret)
			fatal("drmModeAddFB failed");
		#endif
		
		ret = drmModeAddFB2(fd, dev->width, dev->height, DRM_FORMAT_UYVY, handles, pitches, offsets, &dev->bufs[i].fb_id, 0);
//		ret = drmModeAddFB2(fd, dev->width, dev->height, DRM_FORMAT_XRGB8888, handles, pitches, offsets, &dev->bufs[i].fb_id, 0);		
		if(ret) {
			printf("drmModeAddFB2 return err %d\n",ret);
			fatal("drmModeAddFB2 failed");			
		}
	}

	/* Assume all buffers have the same pitch */
	dev->pitch = pitches[0];
	printf("DRM: buffer pitch %d bytes\n", dev->pitch);

	dev->saved_crtc = drmModeGetCrtc(fd, dev->crtc_id); /* must store crtc data */

	// 申请plane的buffer
	for (i = 0; i < BUFCOUNT; i++) {	 
		drm_setup_buffer(fd, dev, 1920, 1080,
				 &dev->plane1bufs[i], map, export);
		handles[0] = dev->plane1bufs[i].bo_handle;
		pitches[0] = 1920*2;
		offsets[0] = 0;
		ret = drmModeAddFB2(fd, 1920, 1080, DRM_FORMAT_UYVY, handles, pitches, offsets, &dev->plane1bufs[i].fb_id, 0);		
		if(ret) {
			printf("plane drmModeAddFB2 return err %d\n",ret);
			fatal("plane drmModeAddFB2 failed");			
		}
	}

	/* Stop before screwing up the monitor */
	getchar();

	/* First buffer to DRM */
	// if (ret = drmModeSetCrtc(fd, dev->crtc_id, dev->plane1bufs[0].fb_id, 0, 0, &dev->conn_id, 1, &dev->mode)) {
	// 	printf("drmModeSetCrtc ret=%d", ret);
	// 	fatal("drmModeSetCrtc() failed");
	// }

	ret = drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
	if (ret) {
		printf("failed to set client cap\n");
	}

	dev->plane_res = drmModeGetPlaneResources(fd);

	/* First flip */
	// drmModePageFlip(fd, dev->crtc_id,
    //                     dev->plane1bufs[0].fb_id, DRM_MODE_PAGE_FLIP_EVENT,
    //                     dev);
}

void drm_destroy(int fd, struct drm_dev_t *dev_head)
{
	struct drm_dev_t *devp, *devp_tmp;
	int i;

	for (devp = dev_head; devp != NULL;) {
		if (devp->saved_crtc) {
			drmModeSetCrtc(fd, devp->saved_crtc->crtc_id, devp->saved_crtc->buffer_id,
				devp->saved_crtc->x, devp->saved_crtc->y, &devp->conn_id, 1, &devp->saved_crtc->mode);
			drmModeFreeCrtc(devp->saved_crtc);
		}

		for (i = 0; i < BUFCOUNT; i++) {
			struct drm_mode_destroy_dumb dreq = { .handle = devp->bufs[i].bo_handle };

			if (devp->bufs[i].buf)
				munmap(devp->bufs[i].buf, devp->bufs[i].size);
			if (devp->bufs[i].dmabuf_fd >= 0)
				close(devp->bufs[i].dmabuf_fd);
			drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
			drmModeRmFB(fd, devp->bufs[i].fb_id);
		}

		for (i = 0; i < BUFCOUNT; i++) {
			struct drm_mode_destroy_dumb dreq = { .handle = devp->plane1bufs[i].bo_handle };

			if (devp->plane1bufs[i].buf)
				munmap(devp->plane1bufs[i].buf, devp->plane1bufs[i].size);
			if (devp->plane1bufs[i].dmabuf_fd >= 0)
				close(devp->plane1bufs[i].dmabuf_fd);
			drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
			drmModeRmFB(fd, devp->plane1bufs[i].fb_id);
		}

		if (devp->plane_res) {
			drmModeFreePlaneResources(devp->plane_res);
		}

		devp_tmp = devp;
		devp = devp->next;
		free(devp_tmp);

	}

	close(fd);
}

void fill_smpte_rgb32(void *mem,
	unsigned int width, unsigned int height)
{
	struct util_rgb_info _rgb = MAKE_RGB_INFO(8, 16, 8, 8, 8, 0, 0, 0);
	struct util_rgb_info *rgb = &_rgb;
	int stride = width * BPP / 8;
	
	const struct color_rgb32 colors_top[] = {
		MAKE_RGB24(rgb, 192, 192, 192),	/* grey */
		MAKE_RGB24(rgb, 192, 192, 0),	/* yellow */
		MAKE_RGB24(rgb, 0, 192, 192),	/* cyan */
		MAKE_RGB24(rgb, 0, 192, 0),	/* green */
		MAKE_RGB24(rgb, 192, 0, 192),	/* magenta */
		MAKE_RGB24(rgb, 192, 0, 0),	/* red */
		MAKE_RGB24(rgb, 0, 0, 192),	/* blue */
	};
	const struct color_rgb32 colors_middle[] = {
		MAKE_RGB24(rgb, 0, 0, 192),	/* blue */
		MAKE_RGB24(rgb, 19, 19, 19),	/* black */
		MAKE_RGB24(rgb, 192, 0, 192),	/* magenta */
		MAKE_RGB24(rgb, 19, 19, 19),	/* black */
		MAKE_RGB24(rgb, 0, 192, 192),	/* cyan */
		MAKE_RGB24(rgb, 19, 19, 19),	/* black */
		MAKE_RGB24(rgb, 192, 192, 192),	/* grey */
	};
	const struct color_rgb32 colors_bottom[] = {
		MAKE_RGB24(rgb, 0, 33, 76),	/* in-phase */
		MAKE_RGB24(rgb, 255, 255, 255),	/* super white */
		MAKE_RGB24(rgb, 50, 0, 106),	/* quadrature */
		MAKE_RGB24(rgb, 19, 19, 19),	/* black */
		MAKE_RGB24(rgb, 9, 9, 9),	/* 3.5% */
		MAKE_RGB24(rgb, 19, 19, 19),	/* 7.5% */
		MAKE_RGB24(rgb, 29, 29, 29),	/* 11.5% */
		MAKE_RGB24(rgb, 19, 19, 19),	/* black */
	};
	unsigned int x;
	unsigned int y;

	for (y = 0; y < height * 6 / 9; ++y) {
		for (x = 0; x < width; ++x)
			((struct color_rgb32 *)mem)[x] =
				colors_top[x * 7 / width];
		mem += stride;
	}

	for (; y < height * 7 / 9; ++y) {
		for (x = 0; x < width; ++x)
			((struct color_rgb32 *)mem)[x] =
				colors_middle[x * 7 / width];
		mem += stride;
	}

	for (; y < height; ++y) {
		for (x = 0; x < width * 5 / 7; ++x)
			((struct color_rgb32 *)mem)[x] =
				colors_bottom[x * 4 / (width * 5 / 7)];
		for (; x < width * 6 / 7; ++x)
			((struct color_rgb32 *)mem)[x] =
				colors_bottom[(x - width * 5 / 7) * 3
					      / (width / 7) + 4];
		for (; x < width; ++x)
			((struct color_rgb32 *)mem)[x] = colors_bottom[7];
		mem += stride;
	}
}
