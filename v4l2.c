#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <fcntl.h>              /* low-level i/o */
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <poll.h>

#include "videodev2.h"
#include "v4l2.h"

#define CLEAR(x) memset(&(x), 0, sizeof(x))
#define PCLEAR(x) memset(x, 0, sizeof(*x))

// 支持两个摄像头的缓存
struct buffer *buffers[2];
static unsigned int n_buffers[2];
static enum v4l2_memory memory_type[2];

void v4l2_queue_buffer(int fd, int index, int dmabuf_fd, int camera_id)
{
	struct v4l2_buffer buf;
	struct v4l2_plane plane;

	CLEAR(buf);
	CLEAR(plane);
	plane.m.fd = dmabuf_fd;

	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	buf.memory = memory_type[camera_id];
	buf.index = index;
	if (memory_type[camera_id] == V4L2_MEMORY_DMABUF) {
		// buf.m.fd = dmabuf_fd;
		buf.length = 1;
		buf.m.planes = &plane;
	}

	if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
		errno_print("VIDIOC_QBUF");
}

int v4l2_dequeue_buffer(int fd, struct v4l2_buffer *buf, int camera_id)
{	
	struct v4l2_plane plane;

	PCLEAR(buf);
	PCLEAR(&plane);

	buf->length = 1;
	buf->m.planes = &plane;

	buf->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	buf->memory = memory_type[camera_id];

	if (-1 == xioctl(fd, VIDIOC_DQBUF, buf)) {
		switch (errno) {
		case EAGAIN:
			return 0;
		case EIO:
			/* Could ignore EIO, see spec. */
			/* fall through */
		default:
			errno_print("VIDIOC_DQBUF");
		}
	}

	assert(buf->index < n_buffers[camera_id]);
	return 1;
}

void v4l2_stop_capturing(int fd)
{
	enum v4l2_buf_type type;

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (-1 == xioctl(fd, VIDIOC_STREAMOFF, &type))
		errno_print("VIDIOC_STREAMOFF");
}

void v4l2_start_capturing_dmabuf(int fd, int camera_id)
{
	enum v4l2_buf_type type;
	unsigned int i;

	/* One buffer held by DRM, the rest queued to video4linux */
	for (i = 1; i < n_buffers[camera_id]; ++i)
		v4l2_queue_buffer(fd, i, buffers[camera_id][i].dmabuf_fd, camera_id);

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (-1 == xioctl(fd, VIDIOC_STREAMON, &type))
		errno_print("VIDIOC_STREAMON");
}

void v4l2_start_capturing_mmap(int fd)
{
	enum v4l2_buf_type type;
	unsigned int i;

	for (i = 0; i < n_buffers[0]; ++i) {
		struct v4l2_buffer buf;

		CLEAR(buf);
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;

		if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
			errno_print("VIDIOC_QBUF");
	}

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (-1 == xioctl(fd, VIDIOC_STREAMON, &type))
		errno_print("VIDIOC_STREAMON");
}

void v4l2_uninit_device(void)
{
	unsigned int i;
	unsigned int camera_id;	

	for(camera_id = 0; camera_id < 2; camera_id++){
		for (i = 0; i < n_buffers[camera_id]; ++i)
			if (-1 == munmap(buffers[camera_id][i].start, buffers[camera_id][i].length))
				errno_print("munmap");
		free(buffers[camera_id]);
	}

}

void v4l2_init_dmabuf(int fd, int *dmabufs, int count, int camera_id)
{
	struct v4l2_requestbuffers req;

	CLEAR(req);

	req.count = count;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	req.memory = V4L2_MEMORY_DMABUF;
	memory_type[camera_id] = req.memory;

	if (-1 == xioctl(fd, VIDIOC_REQBUFS, &req)) {
		if (EINVAL == errno) {
			fprintf(stderr, "does not support dmabuf\n");
			exit(EXIT_FAILURE);
		} else {
			errno_print("VIDIOC_REQBUFS");
		}
	}

	if (req.count < 2) {
		fprintf(stderr, "Insufficient buffer memory\n");
		exit(EXIT_FAILURE);
	}

	buffers[camera_id] = calloc(req.count, sizeof(*buffers[camera_id]));

	if (!buffers[camera_id]) {
		fprintf(stderr, "Out of memory\n");
		exit(EXIT_FAILURE);
	}

	for (n_buffers[camera_id] = 0; n_buffers[camera_id] < req.count; ++n_buffers[camera_id]) {
		struct v4l2_buffer buf;
		struct v4l2_plane plane;

		CLEAR(buf);
		CLEAR(plane);


		buf.type        = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		buf.memory      = V4L2_MEMORY_DMABUF;
		buf.index       = n_buffers[camera_id];

		buf.length		= 1;
		buf.m.planes	= &plane;

		if (-1 == xioctl(fd, VIDIOC_QUERYBUF, &buf))
			errno_print("VIDIOC_QUERYBUF");
		buffers[camera_id][n_buffers[camera_id]].index = buf.index;
		buffers[camera_id][n_buffers[camera_id]].dmabuf_fd = dmabufs[n_buffers[camera_id]];
	}
}

void v4l2_init_mmap(int fd, int count, int camera_id)
{
	struct v4l2_requestbuffers req;

	CLEAR(req);

	req.count = count;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;
	memory_type[camera_id] = req.memory;

	if (-1 == xioctl(fd, VIDIOC_REQBUFS, &req)) {
		if (EINVAL == errno) {
			fprintf(stderr, "does not support mmap\n");
			exit(EXIT_FAILURE);
		} else {
			errno_print("VIDIOC_REQBUFS");
		}
	}

	if (req.count < 2) {
		fprintf(stderr, "Insufficient buffer memory\n");
		exit(EXIT_FAILURE);
	}

	buffers[camera_id] = calloc(req.count, sizeof(*buffers[camera_id]));

	if (!buffers[camera_id]) {
		fprintf(stderr, "Out of memory\n");
		exit(EXIT_FAILURE);
	}

	for (n_buffers[camera_id] = 0; n_buffers[camera_id] < req.count; ++n_buffers[camera_id]) {
		struct v4l2_buffer buf;

		CLEAR(buf);

		buf.type        = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory      = V4L2_MEMORY_MMAP;
		buf.index       = n_buffers[camera_id];

		if (-1 == xioctl(fd, VIDIOC_QUERYBUF, &buf))
			errno_print("VIDIOC_QUERYBUF");

		buffers[camera_id][n_buffers[camera_id]].length = buf.length;
		buffers[camera_id][n_buffers[camera_id]].start =
			mmap(NULL /* start anywhere */,
					buf.length,
					PROT_READ | PROT_WRITE /* required */,
					MAP_SHARED /* recommended */,
					fd, buf.m.offset);

		if (MAP_FAILED == buffers[camera_id][n_buffers[camera_id]].start)
			errno_print("mmap");
	}
}

void v4l2_init(int fd, int width, int height, int pitch)
{
	struct v4l2_capability cap;
	struct v4l2_format fmt;

	if (-1 == xioctl(fd, VIDIOC_QUERYCAP, &cap)) {
		if (EINVAL == errno) {
			fprintf(stderr, "not a V4L2 device\n");
			exit(EXIT_FAILURE);
		} else {
			errno_print("VIDIOC_QUERYCAP");
		}
	}

	if (!(cap.capabilities & (V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_VIDEO_CAPTURE_MPLANE))) {
		fprintf(stderr, "not a video capture device\n");
		exit(EXIT_FAILURE);
	}

	if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
		fprintf(stderr, "does not support streaming i/o\n");
		exit(EXIT_FAILURE);
	}

	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (-1 == xioctl(fd, VIDIOC_G_FMT, &fmt))
		errno_print("VIDIOC_G_FMT1");

	printf("fmt.fmt.pix_mp.num_planes=%d\n", fmt.fmt.pix_mp.num_planes);
	char *p = (char*)&fmt.fmt.pix_mp.pixelformat;
	printf("before: %c%c%c%c\n", *p, *(p+1), *(p+2), *(p+3));

	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

	fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_UYVY;
	fmt.fmt.pix_mp.field       = V4L2_FIELD_NONE;
	fmt.fmt.pix_mp.colorspace  = V4L2_COLORSPACE_RAW;


	if (-1 == xioctl(fd, VIDIOC_S_FMT, &fmt))
		errno_print("VIDIOC_S_FMT");

	p = (char*)&fmt.fmt.pix_mp.pixelformat;
	printf("after: %c%c%c%c\n", *p, *(p+1), *(p+2), *(p+3));

	if (-1 == xioctl(fd, VIDIOC_G_FMT, &fmt))
		errno_print("VIDIOC_G_FMT2");

	printf("v4l2 negotiated format: ");
	printf("size = %dx%d, ", fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height);
	printf("pitch = %d bytes\n", fmt.fmt.pix_mp.plane_fmt[0].bytesperline);

	/* Note VIDIOC_S_FMT may change width and height. */

}

int v4l2_open(const char *dev_name)
{
	struct stat st;
	int fd;

	if (-1 == stat(dev_name, &st)) {
		fprintf(stderr, "Cannot identify '%s': %d, %s\n",
				dev_name, errno, strerror(errno));
		exit(EXIT_FAILURE);
	}

	if (!S_ISCHR(st.st_mode)) {
		fprintf(stderr, "%s is no device\n", dev_name);
		exit(EXIT_FAILURE);
	}

	fd = open(dev_name, O_RDWR /* required */ | O_NONBLOCK, 0);

	if (-1 == fd) {
		fprintf(stderr, "Cannot open '%s': %d, %s\n",
				dev_name, errno, strerror(errno));
		exit(EXIT_FAILURE);
	}
	return fd;
}
