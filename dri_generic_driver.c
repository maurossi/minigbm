/*
 * Copyright 2020 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifdef DRI_GENERIC_DRV

#include <string.h>
#include <xf86drm.h>

#include "dri.h"
#include "drv_priv.h"
#include "helpers.h"
#include "util.h"
#include <errno.h>

static const uint32_t scanout_render_formats[] = { DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888,
						   DRM_FORMAT_ABGR8888, DRM_FORMAT_XBGR8888,
						   DRM_FORMAT_RGB565 };

static const uint32_t texture_only_formats[] = { DRM_FORMAT_NV12, DRM_FORMAT_NV21,
						 DRM_FORMAT_YVU420, DRM_FORMAT_YVU420_ANDROID };

static int dri_generic_init(struct driver *drv)
{
	struct dri_driver *dri;

	dri = calloc(1, sizeof(struct dri_driver));
	if (!dri)
		return -ENOMEM;

	drv->priv = dri;

	if (dri_init2(drv)) {
		free(dri);
		drv->priv = NULL;
		return -ENODEV;
	}

	drv_add_combinations(drv, scanout_render_formats, ARRAY_SIZE(scanout_render_formats),
			     &LINEAR_METADATA, BO_USE_RENDER_MASK | BO_USE_SCANOUT);

	drv_add_combinations(drv, texture_only_formats, ARRAY_SIZE(texture_only_formats),
			     &LINEAR_METADATA, BO_USE_TEXTURE_MASK);

	drv_modify_combination(drv, DRM_FORMAT_NV12, &LINEAR_METADATA,
			       BO_USE_HW_VIDEO_ENCODER | BO_USE_HW_VIDEO_DECODER |
				   BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE);
	drv_modify_combination(drv, DRM_FORMAT_NV21, &LINEAR_METADATA, BO_USE_HW_VIDEO_ENCODER);

	return drv_modify_linear_combinations(drv);
}

static uint32_t dri_generic_resolve_format(struct driver *drv, uint32_t format, uint64_t use_flags)
{
	switch (format) {
	case DRM_FORMAT_FLEX_IMPLEMENTATION_DEFINED:
		/* Camera subsystem requires NV12. */
		if (use_flags & (BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE))
			return DRM_FORMAT_NV12;
		/*HACK: See b/28671744 */
		return DRM_FORMAT_XBGR8888;
	case DRM_FORMAT_FLEX_YCbCr_420_888:
		return DRM_FORMAT_NV12;
	case DRM_FORMAT_BGR565:
		/* mesa3d doesn't support BGR565 */
		return DRM_FORMAT_RGB565;
	default:
		return format;
	}
}

const struct backend backend_dri_generic = {
	.name = "dri_generic",
	.init = dri_generic_init,
	.close = dri_close,
	.bo_create = dri_bo_create,
	.bo_create_with_modifiers = dri_bo_create_with_modifiers,
	.bo_destroy = dri_bo_destroy,
	.bo_import = dri_bo_import,
	.bo_map = dri_bo_map,
	.bo_unmap = dri_bo_unmap,
	.bo_get_plane_fd = dri_bo_get_plane_fd,
	.resolve_format = dri_generic_resolve_format,
	.num_planes_from_modifier = dri_num_planes_from_modifier,
};

#endif
