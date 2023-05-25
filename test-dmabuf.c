
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>

#include "videodev2.h"
#include "drm.h"
#include "v4l2.h"

static const char *dri_path = "/dev/dri/card0";
static char v4l2_path[128];
static int next_buffer_index = -1;
static int curr_buffer_index = 0;

static void page_flip_handler(int fd, unsigned int frame,
			unsigned int sec, unsigned int usec,
			void *data)
{
	struct drm_dev_t *dev = data;

	/* If we have a next buffer, then let's return the current one,
	 * and grab the next one.
	 */
	if (next_buffer_index > 0) {
//		v4l2_queue_buffer(dev->v4l2_fd, curr_buffer_index, dev->bufs[curr_buffer_index].dmabuf_fd);
//		v4l2_queue_buffer(dev->v4l2_fd, curr_buffer_index, dev->plane1bufs[curr_buffer_index].dmabuf_fd);		
		curr_buffer_index = next_buffer_index;
		next_buffer_index = -1;

	}

	drmModePageFlip(fd, dev->crtc_id, dev->plane1bufs[curr_buffer_index].fb_id,
			      DRM_MODE_PAGE_FLIP_EVENT, dev);
}


static void mainloop(int v4l2_fd, int drm_fd, struct drm_dev_t *dev)
{
	struct v4l2_buffer buf;
	drmEventContext ev;
	int r;

        memset(&ev, 0, sizeof ev);
        ev.version = DRM_EVENT_CONTEXT_VERSION;
        ev.vblank_handler = NULL;
        ev.page_flip_handler = page_flip_handler;

	struct pollfd fds[] = {
		{ .fd = STDIN_FILENO, .events = POLLIN },
		{ .fd = v4l2_fd, .events = POLLIN },
		{ .fd = drm_fd, .events = POLLIN },
	};

	while (1) {
		r = poll(fds, 3, 3000);
		if (-1 == r) {
			if (EINTR == errno)
				continue;
			printf("error in poll %d", errno);
			return;
		}

		if (0 == r) {
			fprintf(stderr, "timeout\n");
			return;
		}

		if (fds[0].revents & POLLIN) {
			fprintf(stdout, "User requested exit\n");
			return;
		}
		if (fds[1].revents & POLLIN) {
			/* Video buffer captured, dequeue it
			 * and store it for scanout.
			 */
			int dequeued = v4l2_dequeue_buffer(v4l2_fd, &buf);
			if (dequeued) {
				next_buffer_index = buf.index;
			}

			static int aaa = 1;
			if(aaa) {
				int ret = drmModeSetPlane(drm_fd, dev->plane_res->planes[0], dev->crtc_id, dev->plane1bufs[next_buffer_index].fb_id, 0,
						0, 0, 480, 270,
						0, 0, 1920 << 16, (1080) << 16);		
				if(ret < 0)
					printf("drmModeSetPlane err %d\n",ret);
				aaa = 0;
			}

			int ret = drmModeSetPlane(drm_fd, dev->plane_res->planes[1], dev->crtc_id, dev->plane1bufs[next_buffer_index].fb_id, 0,
					480*2, 270*2, 480, 270,
					0, 0, 1920 << 16, (1080) << 16);		
			if(ret < 0)
				printf("drmModeSetPlane err %d\n",ret);		
			
			v4l2_queue_buffer(dev->v4l2_fd, next_buffer_index, dev->plane1bufs[next_buffer_index].dmabuf_fd);		
		

		}
		if (fds[2].revents & POLLIN) {
			drmHandleEvent(drm_fd, &ev);
		}
	}
}

int main(int argc, char *argv[])
{
	struct drm_dev_t *dev_head, *dev;
	int v4l2_fd, drm_fd;
	int dmabufs[BUFCOUNT];

	drm_fd = drm_open(dri_path, 1, 1);
	dev_head = drm_find_dev(drm_fd);

	if (dev_head == NULL) {
		fprintf(stderr, "available drm_dev not found\n");
		return EXIT_FAILURE;
	}

	if(argc >= 2) {
		strcpy(v4l2_path, argv[1]);
	} else {
		strcpy(v4l2_path, "/dev/video22");
	}

	printf("v4l2_path=%s\n", v4l2_path);

	/*****
	connector id:208
			encoder id:207 crtc id:119
			width:800 height:1280
	connector id:205
			encoder id:204 crtc id:102
			width:800 height:1280
	connector id:195
			encoder id:194 crtc id:85
			width:1920 height:1080
	***********/
	for (dev = dev_head; dev != NULL; dev = dev->next) {
		if(dev->conn_id == 195) {
			printf("select connector id:%d\n", dev->conn_id);
			printf("\tencoder id:%d crtc id:%d\n", dev->enc_id, dev->crtc_id);
			printf("\twidth:%d height:%d\n", dev->width, dev->height);			
			break;
		}
	}

	drm_setup_fb(drm_fd, dev, 1, 1);

	dmabufs[0] = dev->plane1bufs[0].dmabuf_fd;
	dmabufs[1] = dev->plane1bufs[1].dmabuf_fd;
	dmabufs[2] = dev->plane1bufs[2].dmabuf_fd;
	dmabufs[3] = dev->plane1bufs[3].dmabuf_fd;
	// dmabufs[0] = dev->bufs[0].dmabuf_fd;
	// dmabufs[1] = dev->bufs[1].dmabuf_fd;
	// dmabufs[2] = dev->bufs[2].dmabuf_fd;
	// dmabufs[3] = dev->bufs[3].dmabuf_fd;	

	v4l2_fd = v4l2_open(v4l2_path);
	//因摄像头和显示器不一定能设置位相同的模式，后面三个参数保留
	v4l2_init(v4l2_fd, dev->width, dev->height, dev->pitch);
	v4l2_init_dmabuf(v4l2_fd, dmabufs, BUFCOUNT);
	v4l2_start_capturing_dmabuf(v4l2_fd);

	dev->v4l2_fd = v4l2_fd;
	dev->drm_fd = drm_fd;

	mainloop(v4l2_fd, drm_fd, dev);

	drm_destroy(drm_fd, dev_head);
	return 0;
}
