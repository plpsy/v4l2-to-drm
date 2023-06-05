
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>

#include "videodev2.h"
#include "drm.h"
#include "v4l2.h"
#include <time.h>

static const char *dri_path = "/dev/dri/card0";
static char v4l2_path[2][128];
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


static void mainloop(int v4l2_fd[2], int drm_fd, struct drm_dev_t *dev)
{
	struct v4l2_buffer buf;
	drmEventContext ev;
	int camera_id = 0;
	int r;

        memset(&ev, 0, sizeof ev);
        ev.version = DRM_EVENT_CONTEXT_VERSION;
        ev.vblank_handler = NULL;
        ev.page_flip_handler = page_flip_handler;

	struct pollfd fds[] = {
		{ .fd = STDIN_FILENO, .events = POLLIN },
		{ .fd = v4l2_fd[0], .events = POLLIN },
		{ .fd = v4l2_fd[1], .events = POLLIN },		
		{ .fd = drm_fd, .events = POLLIN },
	};

	while (1) {
		r = poll(fds, 4, 3000);
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
			camera_id = 0;
			/* Video buffer captured, dequeue it
			 * and store it for scanout.
			 */
			int dequeued = v4l2_dequeue_buffer(v4l2_fd[camera_id], &buf, camera_id);
			if (dequeued) {
				next_buffer_index = buf.index;
			}

			static int aaa = 1;
			if(aaa) {
				// struct timespec time1, time2;
				// clock_gettime(CLOCK_MONOTONIC, &time1);

				int ret = drmModeSetPlane(drm_fd, dev->plane_res->planes[0], dev->crtc_id, dev->bufs[next_buffer_index].fb_id, DRM_MODE_PAGE_FLIP_ASYNC,
						0, 0, 1920, 1080,
						0, 0, 1920 << 16, (1080) << 16);		
				if(ret < 0)
					printf("drmModeSetPlane1 err %d\n",ret);

				// int ret = drmModeSetCrtc(drm_fd, dev->crtc_id, dev->bufs[next_buffer_index].fb_id, 0, 0, &dev->conn_id, 1, &dev->mode);
				// if (ret < 0) {
				// 	printf("drmModeSetCrtc() failed, ret=%d\n", ret);
				// }

				// clock_gettime(CLOCK_MONOTONIC, &time2);
				// printf("ProcessTime1:%ld \n", time2.tv_nsec-time1.tv_nsec);

				aaa = 1;
			} 
			
			v4l2_queue_buffer(v4l2_fd[camera_id], next_buffer_index, dev->bufs[next_buffer_index].dmabuf_fd, camera_id);		
		
		}
		if (fds[2].revents & POLLIN) {
			camera_id = 1;
			/* Video buffer captured, dequeue it
			 * and store it for scanout.
			 */
			int dequeued = v4l2_dequeue_buffer(v4l2_fd[camera_id], &buf, camera_id);
			if (dequeued) {
				next_buffer_index = buf.index;
			}

			static int bbb = 1;
			if(bbb) {
				// struct timespec time1, time2;
				// clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &time1);

				int ret = drmModeSetPlane(drm_fd, dev->plane_res->planes[1], dev->crtc_id, dev->plane1bufs[next_buffer_index].fb_id, DRM_MODE_PAGE_FLIP_ASYNC,
						480*2, 270*2, 480*2, 270*2,
						0, 0, 1920 << 16, (1080) << 16);
				if(ret < 0)
					printf("drmModeSetPlane2 err %d\n",ret);

				// clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &time2);
				// printf("ProcessTime2:%ld \n", time2.tv_nsec-time1.tv_nsec);					
				bbb = 1;	
			}	
			
			v4l2_queue_buffer(v4l2_fd[camera_id], next_buffer_index, dev->plane1bufs[next_buffer_index].dmabuf_fd, camera_id);

		}

		if (fds[3].revents & POLLIN) {
			drmHandleEvent(drm_fd, &ev);
		}
	}
}

int main(int argc, char *argv[])
{
	struct drm_dev_t *dev_head, *dev;
	int v4l2_fd, drm_fd;
	int dmabufs[2][BUFCOUNT];
	int i = 0;
	int camera_id = 0;

	drm_fd = drm_open(dri_path, 1, 1);
	dev_head = drm_find_dev(drm_fd);

	if (dev_head == NULL) {
		fprintf(stderr, "available drm_dev not found\n");
		return EXIT_FAILURE;
	}

	if(argc >= 3) {
		strcpy(v4l2_path[0], argv[1]);
		strcpy(v4l2_path[1], argv[2]);		
	} else {
		strcpy(v4l2_path[0], "/dev/video22");
		strcpy(v4l2_path[1], "/dev/video31");		
	}

	printf("v4l2_path[0]=%s, v4l2_path[1]=%s\n", v4l2_path[0], v4l2_path[1]);

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

	for(i = 0; i < BUFCOUNT; i++) {
		dmabufs[0][i] = dev->bufs[i].dmabuf_fd;		
		dmabufs[1][i] = dev->plane1bufs[i].dmabuf_fd;
	}

	for(i = 0; i < 2; i++){
		camera_id = i;
		v4l2_fd = v4l2_open(v4l2_path[i]);
		//因摄像头和显示器不一定能设置位相同的模式，后面三个参数保留
		v4l2_init(v4l2_fd, dev->width, dev->height, dev->pitch);
		v4l2_init_dmabuf(v4l2_fd, dmabufs[camera_id], BUFCOUNT, camera_id);
		v4l2_start_capturing_dmabuf(v4l2_fd, camera_id);
		dev->v4l2_fd[camera_id] = v4l2_fd;
	}

	dev->drm_fd = drm_fd;
	mainloop(dev->v4l2_fd, drm_fd, dev);

	drm_destroy(drm_fd, dev_head);
	return 0;
}
