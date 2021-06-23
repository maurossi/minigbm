#include "dmabuf_internals.h"

#include "drv_priv.h"
#include "helpers.h"
#include "util.h"

#ifdef __cplusplus
extern "C" {
#endif

struct backend dmabuf_backend = {
	.name = "dmabuf",
	.init = dmabuf_driver_init,
	.close = dmabuf_driver_close,
	.bo_create = dmabuf_bo_create,
	.bo_destroy = dmabuf_bo_destroy,
	.bo_import = dmabuf_bo_import,
	.bo_map = dmabuf_bo_map,
	.bo_unmap = dmabuf_bo_unmap,
	.resolve_format = dmabuf_resolve_format,
	.bo_get_plane_fd = dmabuf_bo_get_plane_fd,
};

struct backend *init_external_backend(int *fd)
{
	return &dmabuf_backend;
}

#ifdef __cplusplus
}
#endif
