/*
 * Copyright 2017 Advanced Micro Devices. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#if defined(DRI_GENERIC_DRV) || defined(DRV_AMDGPU)

#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>

#include "dri.h"
#include "drv_priv.h"
#include "helpers.h"
#include "util.h"

#ifdef DRI_GENERIC_DRV
#include <loader.h>
#endif

static const struct {
	uint32_t drm_format;
	int dri_image_format;
} drm_to_dri_image_formats[] = {
	{ DRM_FORMAT_R8, __DRI_IMAGE_FORMAT_R8 },
	{ DRM_FORMAT_GR88, __DRI_IMAGE_FORMAT_GR88 },
	{ DRM_FORMAT_RGB565, __DRI_IMAGE_FORMAT_RGB565 },
	{ DRM_FORMAT_XRGB8888, __DRI_IMAGE_FORMAT_XRGB8888 },
	{ DRM_FORMAT_ARGB8888, __DRI_IMAGE_FORMAT_ARGB8888 },
	{ DRM_FORMAT_XBGR8888, __DRI_IMAGE_FORMAT_XBGR8888 },
	{ DRM_FORMAT_ABGR8888, __DRI_IMAGE_FORMAT_ABGR8888 },
	{ DRM_FORMAT_XRGB2101010, __DRI_IMAGE_FORMAT_XRGB2101010 },
	{ DRM_FORMAT_XBGR2101010, __DRI_IMAGE_FORMAT_XBGR2101010 },
	{ DRM_FORMAT_ARGB2101010, __DRI_IMAGE_FORMAT_ARGB2101010 },
	{ DRM_FORMAT_ABGR2101010, __DRI_IMAGE_FORMAT_ABGR2101010 },
};

static int drm_format_to_dri_format(uint32_t drm_format)
{
	uint32_t i;
	for (i = 0; i < ARRAY_SIZE(drm_to_dri_image_formats); i++) {
		if (drm_to_dri_image_formats[i].drm_format == drm_format)
			return drm_to_dri_image_formats[i].dri_image_format;
	}

	return 0;
}

static bool lookup_extension(const __DRIextension *const *extensions, const char *name,
			     int min_version, const __DRIextension **dst)
{
	while (*extensions) {
		if ((*extensions)->name && !strcmp((*extensions)->name, name) &&
		    (*extensions)->version >= min_version) {
			*dst = *extensions;
			return true;
		}

		extensions++;
	}

	return false;
}

int dri_bo_get_plane_fd(struct bo *bo, size_t plane)
{
	struct dri_driver *dri = bo->drv->priv;
	__DRIimage *plane_image = NULL;
	int fd = -1;

	plane_image = dri->image_extension->fromPlanar(bo->priv, plane, NULL);
	__DRIimage *image = plane_image ? plane_image : bo->priv;

	dri->image_extension->queryImage(image, __DRI_IMAGE_ATTRIB_FD, &fd);

	if (plane_image)
		dri->image_extension->destroyImage(plane_image);

	return fd;
}
/*
 * The DRI GEM namespace may be different from the minigbm's driver GEM namespace. We have
 * to import into minigbm.
 */
static int import_into_minigbm(struct dri_driver *dri, struct bo *bo)
{
	int ret, modifier_upper, modifier_lower, num_planes, i, j;
	off_t dmabuf_sizes[DRV_MAX_PLANES];
	__DRIimage *plane_image = NULL;

	if (dri->image_extension->queryImage(bo->priv, __DRI_IMAGE_ATTRIB_MODIFIER_UPPER,
					     &modifier_upper) &&
	    dri->image_extension->queryImage(bo->priv, __DRI_IMAGE_ATTRIB_MODIFIER_LOWER,
					     &modifier_lower))
		bo->meta.format_modifier =
		    ((uint64_t)modifier_upper << 32) | (uint32_t)modifier_lower;
	else
		bo->meta.format_modifier = DRM_FORMAT_MOD_INVALID;

	if (!dri->image_extension->queryImage(bo->priv, __DRI_IMAGE_ATTRIB_NUM_PLANES, &num_planes))
		return -errno;

	for (i = 0; i < num_planes; ++i) {
		int prime_fd, stride, offset;
		plane_image = dri->image_extension->fromPlanar(bo->priv, i, NULL);
		__DRIimage *image = plane_image ? plane_image : bo->priv;

		if (!dri->image_extension->queryImage(image, __DRI_IMAGE_ATTRIB_STRIDE, &stride) ||
		    !dri->image_extension->queryImage(image, __DRI_IMAGE_ATTRIB_OFFSET, &offset)) {
			ret = -errno;
			goto cleanup;
		}

		if (!dri->image_extension->queryImage(image, __DRI_IMAGE_ATTRIB_FD, &prime_fd)) {
			ret = -errno;
			goto cleanup;
		}

		dmabuf_sizes[i] = lseek(prime_fd, 0, SEEK_END);
		if (dmabuf_sizes[i] == (off_t)-1) {
			ret = -errno;
			close(prime_fd);
			goto cleanup;
		}

		lseek(prime_fd, 0, SEEK_SET);

		close(prime_fd);

		if (!dri->image_extension->queryImage(image, __DRI_IMAGE_ATTRIB_HANDLE,
						      &bo->handles[i].s32)) {
			drv_log("queryImage() failed with %s\n", strerror(errno));
			ret = -errno;
			goto cleanup;
		}

		bo->meta.strides[i] = stride;
		bo->meta.offsets[i] = offset;

		if (plane_image)
			dri->image_extension->destroyImage(plane_image);
	}

	for (i = 0; i < num_planes; ++i) {
		off_t next_plane = dmabuf_sizes[i];
		for (j = 0; j < num_planes; ++j) {
			if (bo->meta.offsets[j] < next_plane &&
			    bo->meta.offsets[j] > bo->meta.offsets[i] &&
			    bo->handles[j].u32 == bo->handles[i].u32)
				next_plane = bo->meta.offsets[j];
		}

		bo->meta.sizes[i] = next_plane - bo->meta.offsets[i];

		/* This is kind of misleading if different planes use
		   different dmabufs. */
		bo->meta.total_size += bo->meta.sizes[i];
	}

	return 0;

cleanup:
	if (plane_image)
		dri->image_extension->destroyImage(plane_image);
	while (--i >= 0) {
		for (j = 0; j <= i; ++j)
			if (bo->handles[j].u32 == bo->handles[i].u32)
				break;

		/* Multiple equivalent handles) */
		if (i == j)
			break;
	}
	return ret;
}

/*
 * The caller is responsible for setting drv->priv to a structure that derives from dri_driver.
 */
int dri_init(struct driver *drv, const char *dri_so_path, const char *driver_suffix)
{
	char fname[128];
	const __DRIextension **(*get_extensions)();
	const __DRIextension *loader_extensions[] = { NULL };

	struct dri_driver *dri = drv->priv;

	dri->fd = open(drmGetDeviceNameFromFd(drv_get_fd(drv)), O_RDWR);
	if (dri->fd < 0)
		return -ENODEV;

	dri->driver_handle = dlopen(dri_so_path, RTLD_NOW | RTLD_GLOBAL);
	if (!dri->driver_handle)
		goto close_dri_fd;

	snprintf(fname, sizeof(fname), __DRI_DRIVER_GET_EXTENSIONS "_%s", driver_suffix);
	get_extensions = dlsym(dri->driver_handle, fname);
	if (!get_extensions)
		goto free_handle;

	dri->extensions = get_extensions();
	if (!dri->extensions)
		goto free_handle;

	if (!lookup_extension(dri->extensions, __DRI_CORE, 2,
			      (const __DRIextension **)&dri->core_extension))
		goto free_handle;

	/* Version 4 for createNewScreen2 */
	if (!lookup_extension(dri->extensions, __DRI_DRI2, 4,
			      (const __DRIextension **)&dri->dri2_extension))
		goto free_handle;

	dri->device = dri->dri2_extension->createNewScreen2(0, dri->fd, loader_extensions,
							    dri->extensions, &dri->configs, NULL);
	if (!dri->device)
		goto free_handle;

	dri->context =
	    dri->dri2_extension->createNewContext(dri->device, *dri->configs, NULL, NULL);

	if (!dri->context)
		goto free_screen;

	if (!lookup_extension(dri->core_extension->getExtensions(dri->device), __DRI_IMAGE, 12,
			      (const __DRIextension **)&dri->image_extension))
		goto free_context;

	if (!lookup_extension(dri->core_extension->getExtensions(dri->device), __DRI2_FLUSH, 4,
			      (const __DRIextension **)&dri->flush_extension))
		goto free_context;

	return 0;

free_context:
	dri->core_extension->destroyContext(dri->context);
free_screen:
	dri->core_extension->destroyScreen(dri->device);
free_handle:
	dlclose(dri->driver_handle);
	dri->driver_handle = NULL;
close_dri_fd:
	close(dri->fd);
	return -ENODEV;
}
/*
 * The caller is responsible for setting drv->priv to a structure that derives from dri_driver.
 */
int dri_init2(struct driver *drv)
{
#ifdef DRI_GENERIC_DRV
	char dri_pathname[64];
	char drv_suffix[32];

	char *drv_name = loader_get_driver_for_fd(drv->fd);

	sprintf(dri_pathname, STRINGIZE(DRI_DRIVER_DIR) "/%s_dri.so", drv_name);

	strcpy(drv_suffix, drv_name);
	free(drv_name);

	/* replace all '-' chars with '_' in drv_suffix to use in dlsym() */
	char *current_pos = strchr(drv_suffix, '-');
	while (current_pos) {
		*current_pos = '_';
		current_pos = strchr(current_pos, '-');
	}

	if (dri_init(drv, dri_pathname, drv_suffix)) {
		drv_log("dri_init failed for (%s) , (%s)", dri_pathname, drv_suffix);
		return -ENODEV;
	}

	return 0;
#else
	return -ENOTSUPP;
#endif
}

/*
 * The caller is responsible for freeing drv->priv.
 */
void dri_close(struct driver *drv)
{
	struct dri_driver *dri = drv->priv;

	dri->core_extension->destroyContext(dri->context);
	dri->core_extension->destroyScreen(dri->device);
	dlclose(dri->driver_handle);
	dri->driver_handle = NULL;
	close(dri->fd);
}

int dri_bo_create_common(struct bo *bo, uint32_t width, uint32_t height, uint32_t format,
			 uint64_t use_flags, const uint64_t *modifiers, uint32_t modifier_count)
{
	uint32_t width_ = width;
	uint32_t height_ = height;
	unsigned int dri_use;
	int ret, dri_format;
	int dri_format_unavailable = false;
	struct dri_driver *dri = bo->drv->priv;

	dri_format = drm_format_to_dri_format(format);
	/* Video buffers can't be allocated using DRI */
	if (!dri_format)
		dri_format_unavailable = true;

	/* Gallium drivers require shared to get the handle and stride. */
	dri_use = __DRI_IMAGE_USE_SHARE;
	if (use_flags & BO_USE_SCANOUT)
		dri_use |= __DRI_IMAGE_USE_SCANOUT;
	if (use_flags & BO_USE_CURSOR)
		dri_use |= __DRI_IMAGE_USE_CURSOR;
	if (use_flags & (BO_USE_LINEAR | BO_USE_SW_MASK))
		dri_use |= __DRI_IMAGE_USE_LINEAR;

	if (dri_format_unavailable) {
		int stride = drv_stride_from_format(format, width, 0);
		drv_bo_from_format(bo, stride, height, format);
		dri_format = __DRI_IMAGE_FORMAT_R8;
		dri_use |= __DRI_IMAGE_USE_LINEAR;
		width_ = stride /  drv_bytes_per_pixel_from_format(format, 0);
		height_ = DIV_ROUND_UP(bo->meta.total_size, width_);
	}

	if (modifier_count == 0) {
		bo->priv = dri->image_extension->createImage(dri->device, width_, height_,
							     dri_format, dri_use, NULL);
	} else {
		if (!dri->image_extension->createImageWithModifiers) {
			return -ENOENT;
		}
		bo->priv = dri->image_extension->createImageWithModifiers(
		    dri->device, width, height, dri_format, modifiers, modifier_count, NULL);
	}

	if (!bo->priv) {
		ret = -errno;
		return ret;
	}

	if (dri_format_unavailable) {
		__DRIimage *plane_image = dri->image_extension->fromPlanar(bo->priv, 0, NULL);
		__DRIimage *image = plane_image ? plane_image : bo->priv;

		if (!dri->image_extension->queryImage(image, __DRI_IMAGE_ATTRIB_HANDLE,
						      &bo->handles[0].s32)) {
			drv_log("queryImage() failed with error %s\n", strerror(errno));
			ret = -errno;
			goto free_image;
		}

		if (!dri->image_extension->queryImage(image, __DRI_IMAGE_ATTRIB_STRIDE,
						      (int32_t *)&bo->meta.strides[0])) {
			drv_log("queryImage() failed with error %s\n", strerror(errno));
			ret = -errno;
			goto free_image;
		}

		drv_bo_from_format(bo, bo->meta.strides[0], height, format);

		if (plane_image)
			dri->image_extension->destroyImage(plane_image);

		for (size_t plane = 1; plane < bo->meta.num_planes; plane++)
			bo->handles[plane].u32 = bo->handles[0].u32;
	} else {
		ret = import_into_minigbm(dri, bo);
		if (ret)
			goto free_image;
	}

	return 0;

free_image:
	dri->image_extension->destroyImage(bo->priv);
	return ret;
}

int dri_bo_create(struct bo *bo, uint32_t width, uint32_t height, uint32_t format,
		  uint64_t use_flags)
{
	return dri_bo_create_common(bo, width, height, format, use_flags, NULL, 0);
}

int dri_bo_create_with_modifiers(struct bo *bo, uint32_t width, uint32_t height, uint32_t format,
				 const uint64_t *modifiers, uint32_t modifier_count)
{
	return dri_bo_create_common(bo, width, height, format, 0, modifiers, modifier_count);
}

int dri_bo_import(struct bo *bo, struct drv_import_fd_data *data)
{
	int ret;
	struct dri_driver *dri = bo->drv->priv;

	if (data->format_modifier != DRM_FORMAT_MOD_INVALID) {
		unsigned error;

		if (!dri->image_extension->createImageFromDmaBufs2)
			return -ENOSYS;

		// clang-format off
		bo->priv = dri->image_extension->createImageFromDmaBufs2(dri->device, data->width, data->height,
									 drv_get_standard_fourcc(data->format),
									 data->format_modifier,
									 data->fds,
									 bo->meta.num_planes,
									 (int *)data->strides,
									 (int *)data->offsets,
									 __DRI_YUV_COLOR_SPACE_UNDEFINED,
									 __DRI_YUV_RANGE_UNDEFINED,
									 __DRI_YUV_CHROMA_SITING_UNDEFINED,
									 __DRI_YUV_CHROMA_SITING_UNDEFINED,
									 &error, NULL);
		// clang-format on

		/* Could translate the DRI error, but the Mesa GBM also returns ENOSYS. */
		if (!bo->priv)
			return -ENOSYS;
	} else {
		// clang-format off
		bo->priv = dri->image_extension->createImageFromFds(dri->device, data->width, data->height,
								    drv_get_standard_fourcc(data->format), data->fds,
								    bo->meta.num_planes,
								    (int *)data->strides,
								    (int *)data->offsets, NULL);
		// clang-format on
		if (!bo->priv)
			return -errno;
	}

	ret = import_into_minigbm(dri, bo);
	if (ret) {
		dri->image_extension->destroyImage(bo->priv);
		return ret;
	}

	return 0;
}

int dri_bo_destroy(struct bo *bo)
{
	struct dri_driver *dri = bo->drv->priv;

	assert(bo->priv);
	dri->image_extension->destroyImage(bo->priv);
	bo->priv = NULL;
	return 0;
}

/*
 * Map an image plane.
 *
 * This relies on the underlying driver to do a decompressing and/or de-tiling
 * blit if necessary,
 *
 * This function itself is not thread-safe; we rely on the fact that the caller
 * locks a per-driver mutex.
 */
void *dri_bo_map(struct bo *bo, struct vma *vma, size_t plane, uint32_t map_flags)
{
	struct dri_driver *dri = bo->drv->priv;

	/* GBM flags and DRI flags are the same. */
	vma->addr = dri->image_extension->mapImage(dri->context, bo->priv, 0, 0, bo->meta.width,
						   bo->meta.height, map_flags,
						   (int *)&vma->map_strides[plane], &vma->priv);
	if (!vma->addr)
		return MAP_FAILED;

	return vma->addr;
}

int dri_bo_unmap(struct bo *bo, struct vma *vma)
{
	struct dri_driver *dri = bo->drv->priv;

	assert(vma->priv);
	dri->image_extension->unmapImage(dri->context, bo->priv, vma->priv);

	/*
	 * From gbm_dri.c in Mesa:
	 *
	 * "Not all DRI drivers use direct maps. They may queue up DMA operations
	 *  on the mapping context. Since there is no explicit gbm flush mechanism,
	 *  we need to flush here."
	 */

	dri->flush_extension->flush_with_flags(dri->context, NULL, __DRI2_FLUSH_CONTEXT, 0);
	return 0;
}

size_t dri_num_planes_from_modifier(struct driver *drv, uint32_t format, uint64_t modifier)
{
	struct dri_driver *dri = drv->priv;
	if (!dri->image_extension->queryDmaBufFormatModifierAttribs) {
		/* We do not do any modifier checks here. The create will fail
		 * later if the modifier is not supported. */
		return drv_num_planes_from_format(format);
	}

	uint64_t planes;
	unsigned char ret = dri->image_extension->queryDmaBufFormatModifierAttribs(
	    dri->device, format, modifier, __DRI_IMAGE_ATTRIB_NUM_PLANES, &planes);
	if (!ret)
		return 0;

	return planes;
}

#endif
