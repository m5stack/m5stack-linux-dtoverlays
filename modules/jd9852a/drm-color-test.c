// SPDX-License-Identifier: MIT
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <drm.h>
#include <drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

static volatile sig_atomic_t stop;

static void handle_signal(int signo)
{
	(void)signo;
	stop = 1;
}

static drmModeConnector *find_connector(int fd, drmModeRes *res)
{
	drmModeConnector *connector;
	int i;

	for (i = 0; i < res->count_connectors; i++) {
		connector = drmModeGetConnector(fd, res->connectors[i]);
		if (!connector)
			continue;
		if (connector->connection == DRM_MODE_CONNECTED &&
		    connector->count_modes > 0)
			return connector;
		drmModeFreeConnector(connector);
	}

	return NULL;
}

static int find_crtc(int fd, drmModeRes *res, drmModeConnector *connector,
		     uint32_t *crtc_id)
{
	drmModeEncoder *encoder;
	int i, j;

	if (connector->encoder_id) {
		encoder = drmModeGetEncoder(fd, connector->encoder_id);
		if (encoder) {
			if (encoder->crtc_id) {
				*crtc_id = encoder->crtc_id;
				drmModeFreeEncoder(encoder);
				return 0;
			}
			drmModeFreeEncoder(encoder);
		}
	}

	for (i = 0; i < connector->count_encoders; i++) {
		encoder = drmModeGetEncoder(fd, connector->encoders[i]);
		if (!encoder)
			continue;
		for (j = 0; j < res->count_crtcs; j++) {
			if (encoder->possible_crtcs & (1U << j)) {
				*crtc_id = res->crtcs[j];
				drmModeFreeEncoder(encoder);
				return 0;
			}
		}
		drmModeFreeEncoder(encoder);
	}

	return -ENODEV;
}

static void draw_pattern(void *map, uint32_t width, uint32_t height,
			 uint32_t pitch)
{
	static const uint32_t colors[] = {
		0x00ffffff, 0x00ffff00, 0x0000ffff, 0x0000ff00,
		0x00ff00ff, 0x00ff0000, 0x000000ff, 0x00000000,
	};
	uint32_t *row;
	uint32_t x, y, color;

	for (y = 0; y < height; y++) {
		row = (uint32_t *)((uint8_t *)map + y * pitch);
		for (x = 0; x < width; x++) {
			if (y < height * 3 / 4) {
				color = colors[(uint64_t)x * 8 / width];
			} else {
				uint32_t level = 255 -
					(uint64_t)x * 255 / (width - 1);
				color = level | (level << 8) | (level << 16);
			}
			row[x] = color;
		}
	}
}

int main(int argc, char **argv)
{
	const char *device = argc > 1 ? argv[1] : "/dev/dri/card0";
	struct drm_mode_create_dumb create = { 0 };
	struct drm_mode_map_dumb map_req = { 0 };
	struct drm_mode_destroy_dumb destroy = { 0 };
	drmModeConnector *connector = NULL;
	drmModeCrtc *old_crtc = NULL;
	drmModeRes *resources = NULL;
	drmModeModeInfo mode;
	uint32_t connector_id, crtc_id, fb_id = 0;
	void *map = MAP_FAILED;
	struct pollfd pollfd = { .fd = STDIN_FILENO, .events = POLLIN };
	int fd = -1, ret = EXIT_FAILURE;

	fd = open(device, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror(device);
		goto out;
	}

	resources = drmModeGetResources(fd);
	if (!resources) {
		perror("drmModeGetResources");
		goto out;
	}

	connector = find_connector(fd, resources);
	if (!connector) {
		fprintf(stderr, "No connected DRM connector with a mode found\n");
		goto out;
	}

	if (find_crtc(fd, resources, connector, &crtc_id)) {
		fprintf(stderr, "No usable CRTC found\n");
		goto out;
	}

	connector_id = connector->connector_id;
	mode = connector->modes[0];
	old_crtc = drmModeGetCrtc(fd, crtc_id);

	create.width = mode.hdisplay;
	create.height = mode.vdisplay;
	create.bpp = 32;
	if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
		perror("DRM_IOCTL_MODE_CREATE_DUMB");
		goto out;
	}

	if (drmModeAddFB(fd, create.width, create.height, 24, 32,
			 create.pitch, create.handle, &fb_id)) {
		perror("drmModeAddFB");
		goto out;
	}

	map_req.handle = create.handle;
	if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map_req) < 0) {
		perror("DRM_IOCTL_MODE_MAP_DUMB");
		goto out;
	}

	map = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED,
		   fd, map_req.offset);
	if (map == MAP_FAILED) {
		perror("mmap");
		goto out;
	}

	draw_pattern(map, create.width, create.height, create.pitch);
	if (drmModeSetCrtc(fd, crtc_id, fb_id, 0, 0, &connector_id, 1, &mode)) {
		perror("drmModeSetCrtc");
		fprintf(stderr, "Stop the display server and run as root/DRM master\n");
		goto out;
	}

	printf("Showing color bars on %s, %ux%u. Press Enter or Ctrl+C to exit.\n",
	       device, mode.hdisplay, mode.vdisplay);
	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);
	while (!stop && poll(&pollfd, 1, 250) == 0)
		;
	ret = EXIT_SUCCESS;

out:
	if (old_crtc && old_crtc->mode_valid)
		drmModeSetCrtc(fd, old_crtc->crtc_id, old_crtc->buffer_id,
			       old_crtc->x, old_crtc->y, &connector_id, 1,
			       &old_crtc->mode);
	if (map != MAP_FAILED)
		munmap(map, create.size);
	if (fb_id)
		drmModeRmFB(fd, fb_id);
	if (create.handle) {
		destroy.handle = create.handle;
		ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
	}
	drmModeFreeCrtc(old_crtc);
	drmModeFreeConnector(connector);
	drmModeFreeResources(resources);
	if (fd >= 0)
		close(fd);
	return ret;
}
