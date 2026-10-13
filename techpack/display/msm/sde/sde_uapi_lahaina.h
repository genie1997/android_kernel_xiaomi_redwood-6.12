/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _SDE_UAPI_LAHAINA_H
#define _SDE_UAPI_LAHAINA_H

#include <linux/string.h>
#include <linux/uaccess.h>
#include <drm/drm_property.h>
#include <drm/sde_drm.h>

/* scaler_v2 bytes a lahaina composer passes: everything before de_lpf_flags */
#define SDE_UAPI_LAHAINA_SCALER_V2_LEN	0x1a8

#if IS_ENABLED(CONFIG_DRM_SDE_LAHAINA_UAPI)
struct drm_property_blob *sde_uapi_lahaina_blob(struct drm_device *dev,
		const char *name, struct drm_property_blob *blob);

static inline unsigned long sde_uapi_copy_scaler_v2(struct sde_drm_scaler_v2 *dst,
		const void __user *usr)
{
	memset(dst, 0, sizeof(*dst));
	return copy_from_user(dst, usr, SDE_UAPI_LAHAINA_SCALER_V2_LEN);
}
#else
static inline struct drm_property_blob *sde_uapi_lahaina_blob(struct drm_device *dev,
		const char *name, struct drm_property_blob *blob)
{
	return blob;
}

static inline unsigned long sde_uapi_copy_scaler_v2(struct sde_drm_scaler_v2 *dst,
		const void __user *usr)
{
	return copy_from_user(dst, usr, sizeof(*dst));
}
#endif

#endif /* _SDE_UAPI_LAHAINA_H */
