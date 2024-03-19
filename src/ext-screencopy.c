/*
 * Copyright (c) 2022 - 2023 Andri Yngvason
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include <unistd.h>
#include <stdlib.h>
#include <assert.h>
#include <stdbool.h>
#include <inttypes.h>
#include <sys/mman.h>
#include <wayland-client.h>
#include <libdrm/drm_fourcc.h>
#include <aml.h>
#include <neatvnc.h>

#include "screencopy-interface.h"
#include "ext-screencopy-v1.h"
#include "ext-image-source-v1.h"
#include "buffer.h"
#include "shm.h"
#include "time-util.h"
#include "usdt.h"
#include "pixels.h"
#include "config.h"
#include "logging.h"

extern struct ext_image_source_manager_v1* ext_image_source_manager;
extern struct ext_screencopy_manager_v1* ext_screencopy_manager;

struct ext_screencopy {
	struct screencopy parent;
	struct wl_output* wl_output;
	struct ext_screencopy_session_v1* session;
	struct ext_screencopy_cursor_session_v1* cursor;
	bool render_cursors;
	struct wv_buffer_pool* pool;
	struct wv_buffer* buffer;
	bool have_buffer_info;
	bool should_start;
	bool shall_be_immediate;
	bool capture_cursor;

	uint32_t width, height;
	uint32_t wl_shm_stride, wl_shm_format;

	bool have_wl_shm;
	bool have_linux_dmabuf;
	uint32_t dmabuf_format;
};

struct screencopy_impl ext_screencopy_impl;

static struct ext_screencopy_session_v1_listener session_listener;
static struct ext_screencopy_cursor_session_v1_listener cursor_listener;

static int ext_screencopy_init_session(struct ext_screencopy* self)
{
	if (self->session)
		ext_screencopy_session_v1_destroy(self->session);

	struct ext_image_source_v1* source;
	source = ext_image_source_manager_v1_create_output_source(
			ext_image_source_manager, self->wl_output);
	if (!source)
		return -1;

	enum ext_screencopy_manager_v1_options options = 0;
	if (self->render_cursors)
		options |= EXT_SCREENCOPY_MANAGER_V1_OPTIONS_RENDER_CURSORS;

	if (self->capture_cursor) {
		self->cursor = ext_screencopy_manager_v1_capture_cursor(
				ext_screencopy_manager, source, NULL,
				EXT_SCREENCOPY_MANAGER_V1_INPUT_TYPE_POINTER,
				options);
		ext_image_source_v1_destroy(source);
		if (!self->cursor)
			return -1;

		self->session =
			ext_screencopy_cursor_session_v1_get_screencopy_session(
				self->cursor);
	} else {
		self->session = ext_screencopy_manager_v1_capture(
				ext_screencopy_manager, source, options);
		ext_image_source_v1_destroy(source);
	}
	if (!self->session)
		return -1;

	ext_screencopy_session_v1_add_listener(self->session,
			&session_listener, self);

	if (self->capture_cursor) {
		ext_screencopy_cursor_session_v1_add_listener(self->cursor,
				&cursor_listener, self);
	}

	return 0;
}

// TODO: Throttle capturing to max_fps
static void ext_screencopy_schedule_capture(struct ext_screencopy* self,
		bool immediate)
{
	self->buffer = wv_buffer_pool_acquire(self->pool);
	self->buffer->domain = self->capture_cursor ? WV_BUFFER_DOMAIN_CURSOR :
		WV_BUFFER_DOMAIN_OUTPUT;

	ext_screencopy_session_v1_attach_buffer(self->session,
			self->buffer->wl_buffer);

	int n_rects = 0;
	struct pixman_box16* rects =
		pixman_region_rectangles(&self->buffer->buffer_damage, &n_rects);

	for (int i = 0; i < n_rects; ++i) {
		uint32_t x = rects[i].x1;
		uint32_t y = rects[i].y1;
		uint32_t width = rects[i].x2 - x;
		uint32_t height = rects[i].y2 - y;

		ext_screencopy_session_v1_damage_buffer(self->session, x, y,
				width, height);
	}

	uint32_t flags = 0;

	if (!immediate)
		flags |= EXT_SCREENCOPY_SESSION_V1_OPTIONS_ON_DAMAGE;

	ext_screencopy_session_v1_commit(self->session, flags);

	nvnc_log(NVNC_LOG_DEBUG, "Committed buffer%s: %p\n", immediate ? " immediately" : "",
			self->buffer);
}

static void session_handle_format_shm(void *data,
		struct ext_screencopy_session_v1 *session,
		uint32_t format)
{
	struct ext_screencopy* self = data;

	self->have_wl_shm = true;
	self->wl_shm_format = format;
}

static void session_handle_format_drm(void *data,
		struct ext_screencopy_session_v1 *session,
		uint32_t format)
{
	struct ext_screencopy* self = data;

#ifdef ENABLE_SCREENCOPY_DMABUF
	self->have_linux_dmabuf = true;
	self->dmabuf_format = format;
#endif
}

static void session_handle_dimensions(void *data,
		struct ext_screencopy_session_v1 *session, uint32_t width,
		uint32_t height)
{
	struct ext_screencopy* self = data;

	self->width = width;
	self->height = height;
	self->wl_shm_stride = width * 4;
}

static void session_handle_constraints_done(void *data,
		struct ext_screencopy_session_v1 *session)
{
	struct ext_screencopy* self = data;
	uint32_t width, height, stride, format;
	enum wv_buffer_type type = WV_BUFFER_UNSPEC;

	width = self->width;
	height = self->height;

#ifdef ENABLE_SCREENCOPY_DMABUF
	if (self->have_linux_dmabuf && self->parent.enable_linux_dmabuf) {
		format = self->dmabuf_format;
		stride = 0;
		type = WV_BUFFER_DMABUF;
	} else
#endif
	{
		format = self->wl_shm_format;
		stride = self->wl_shm_stride;
		type = WV_BUFFER_SHM;
	}

	wv_buffer_pool_resize(self->pool, type, width, height, stride, format);

	if (self->should_start) {
		ext_screencopy_schedule_capture(self, self->shall_be_immediate);

		self->should_start = false;
		self->shall_be_immediate = false;
	}

	self->have_buffer_info = true;

	nvnc_log(NVNC_LOG_DEBUG, "Init done\n");
}

static void session_handle_transform(void *data,
		struct ext_screencopy_session_v1 *session,
		int32_t transform)
{
	struct ext_screencopy* self = data;

	assert(self->buffer);

	// TODO: Tell main.c not to override this transform
	nvnc_fb_set_transform(self->buffer->nvnc_fb, transform);
}

static void session_handle_ready(void *data,
		struct ext_screencopy_session_v1 *session)
{
	struct ext_screencopy* self = data;

	nvnc_log(NVNC_LOG_DEBUG, "Ready!\n");

	assert(self->buffer);

	enum wv_buffer_domain domain = self->capture_cursor ?
		WV_BUFFER_DOMAIN_CURSOR : WV_BUFFER_DOMAIN_OUTPUT;
	wv_buffer_registry_damage_all(&self->buffer->frame_damage, domain);
	pixman_region_clear(&self->buffer->buffer_damage);

	struct wv_buffer* buffer = self->buffer;
	self->buffer = NULL;

	self->parent.on_done(SCREENCOPY_DONE, buffer, self->parent.userdata);
}

static void session_handle_failed(void *data,
		struct ext_screencopy_session_v1 *session,
		enum ext_screencopy_session_v1_failure_reason reason)
{
	struct ext_screencopy* self = data;

	nvnc_log(NVNC_LOG_DEBUG, "Failed!\n");

	assert(self->buffer);

	wv_buffer_pool_release(self->pool, self->buffer);
	self->buffer = NULL;

	if (reason == EXT_SCREENCOPY_SESSION_V1_FAILURE_REASON_INVALID_BUFFER) {
		ext_screencopy_init_session(self);
	}

	self->parent.on_done(SCREENCOPY_FAILED, NULL, self->parent.userdata);
}

static void session_handle_damage(void *data,
		struct ext_screencopy_session_v1 *session,
		uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
	struct ext_screencopy* self = data;

	wv_buffer_damage_rect(self->buffer, x, y, width, height);
}

static void session_handle_presentation_time(void *data,
		struct ext_screencopy_session_v1 *session,
		uint32_t sec_hi, uint32_t sec_lo, uint32_t nsec)
{
	// TODO
}

static struct ext_screencopy_session_v1_listener session_listener = {
	.format_shm = session_handle_format_shm,
	.format_drm = session_handle_format_drm,
	.dimensions = session_handle_dimensions,
	.constraints_done = session_handle_constraints_done,
	.damage = session_handle_damage,
	.presentation_time = session_handle_presentation_time,
	.transform = session_handle_transform,
	.ready = session_handle_ready,
	.failed = session_handle_failed,
};

static void cursor_handle_enter(void* data,
		struct ext_screencopy_cursor_session_v1* cursor)
{
	struct ext_screencopy* self = data;
	if (self->parent.cursor_enter)
		self->parent.cursor_enter(self->parent.userdata);
}

static void cursor_handle_leave(void* data,
		struct ext_screencopy_cursor_session_v1* cursor)
{
	struct ext_screencopy* self = data;
	if (self->parent.cursor_leave)
		self->parent.cursor_leave(self->parent.userdata);
}

static void cursor_handle_position(void* data,
		struct ext_screencopy_cursor_session_v1* cursor, int x, int y)
{
	// Don't care
}

static void cursor_handle_hotspot(void* data,
		struct ext_screencopy_cursor_session_v1* cursor, int x, int y)
{
	struct ext_screencopy* self = data;
	if (self->parent.cursor_hotspot)
		self->parent.cursor_hotspot(x, y, self->parent.userdata);
}

static struct ext_screencopy_cursor_session_v1_listener cursor_listener = {
	.enter = cursor_handle_enter,
	.leave = cursor_handle_leave,
	.position = cursor_handle_position,
	.hotspot = cursor_handle_hotspot,
};

static int ext_screencopy_start(struct screencopy* ptr, bool immediate)
{
	struct ext_screencopy* self = (struct ext_screencopy*)ptr;

	if (!self->have_buffer_info) {
		self->should_start = true;
		self->shall_be_immediate = immediate;
	} else {
		ext_screencopy_schedule_capture(self, immediate);
	}

	return 0;
}

static void ext_screencopy_stop(struct screencopy* self)
{
	// Nothing to stop?
}

static struct screencopy* ext_screencopy_create(struct wl_output* output,
		bool render_cursor)
{
	struct ext_screencopy* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->parent.impl = &ext_screencopy_impl;
	self->parent.rate_limit = 30;

	self->wl_output = output;
	self->render_cursors = render_cursor;

	self->pool = wv_buffer_pool_create(0, 0, 0, 0, 0);
	if (!self->pool)
		goto failure;

	if (ext_screencopy_init_session(self) < 0)
		goto session_failure;

	return (struct screencopy*)self;

session_failure:
	wv_buffer_pool_destroy(self->pool);
failure:
	free(self);
	return NULL;
}

static struct screencopy* ext_screencopy_create_cursor(struct wl_output* output)
{
	struct ext_screencopy* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->parent.impl = &ext_screencopy_impl;
	self->parent.rate_limit = 30;

	self->wl_output = output;
	self->capture_cursor = true;

	self->pool = wv_buffer_pool_create(0, 0, 0, 0, 0);
	if (!self->pool)
		goto failure;

	if (ext_screencopy_init_session(self) < 0)
		goto session_failure;

	return (struct screencopy*)self;

session_failure:
	wv_buffer_pool_destroy(self->pool);
failure:
	free(self);
	return NULL;
}

void ext_screencopy_destroy(struct screencopy* ptr)
{
	struct ext_screencopy* self = (struct ext_screencopy*)ptr;

	if (self->session)
		ext_screencopy_session_v1_destroy(self->session);
	if (self->buffer)
		wv_buffer_pool_release(self->pool, self->buffer);
	free(self);
}

struct screencopy_impl ext_screencopy_impl = {
	.create = ext_screencopy_create,
	.create_cursor = ext_screencopy_create_cursor,
	.destroy = ext_screencopy_destroy,
	.start = ext_screencopy_start,
	.stop = ext_screencopy_stop,
};
