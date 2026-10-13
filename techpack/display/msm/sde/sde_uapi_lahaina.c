// SPDX-License-Identifier: GPL-2.0-only
/*
 * The vendor composer speaks the lahaina display UAPI: five of its structs only grew at the end
 * since, so their shorter blobs are taken and what they lack is zeroed.
 */

#include <linux/build_bug.h>
#include <linux/stddef.h>
#include <drm/drm_print.h>
#include <drm/msm_drm_pp.h>
#include "sde_uapi_lahaina.h"

struct sde_uapi_lahaina_layout {
	const char *name;
	size_t len;
	size_t kept;
	size_t size;
};

/* len = the lahaina sizeof, kept = the bytes before the first field added since (asserted below) */
static const struct sde_uapi_lahaina_layout layouts[] = {
	{ "SDE_DSPP_IGC_V4", 0xc18, 0xc18, sizeof(struct drm_msm_igc_lut) },
	{ "SDE_DGM_1D_LUT_IGC_V5", 0xc18, 0xc18, sizeof(struct drm_msm_igc_lut) },
	{ "SDE_VIG_1D_LUT_IGC_V6", 0xc18, 0xc18, sizeof(struct drm_msm_igc_lut) },
	{ "SDE_DSPP_GC_V1", 0x1808, 0x1808, sizeof(struct drm_msm_pgc_lut) },
	{ "SDE_DGM_1D_LUT_GC_V5", 0x1808, 0x1808, sizeof(struct drm_msm_pgc_lut) },
	{ "SDE_DSPP_PA_SIXZONE_V1", 0xc20, 0xc1c, sizeof(struct drm_msm_sixzone) },
	{ "SDE_PP_DITHER_V1", 0x60, 0x5c, sizeof(struct drm_msm_dither) },
};

static_assert(offsetof(struct drm_msm_igc_lut, c0_extended) == 0xc18);
static_assert(offsetof(struct drm_msm_pgc_lut, c0_extended) == 0x1808);
static_assert(offsetof(struct drm_msm_sixzone, sat_adjust_p0) == 0xc1c);
static_assert(offsetof(struct drm_msm_dither, dither_matrix_select) == 0x5c);
static_assert(offsetof(struct sde_drm_scaler_v2, de_lpf_flags) == SDE_UAPI_LAHAINA_SCALER_V2_LEN);

/* Returns the blob to keep: the same one, a zero-padded copy of a lahaina one, or NULL on failure */
struct drm_property_blob *sde_uapi_lahaina_blob(struct drm_device *dev,
		const char *name, struct drm_property_blob *blob)
{
	const struct sde_uapi_lahaina_layout *l;
	struct drm_property_blob *copy;

	for (l = layouts; l < layouts + ARRAY_SIZE(layouts); l++)
		if (blob->length == l->len && !strcmp(name, l->name))
			break;
	if (l == layouts + ARRAY_SIZE(layouts))
		return blob;

	copy = drm_property_create_blob(dev, l->size, NULL);
	if (IS_ERR(copy)) {
		DRM_ERROR("%s: no memory for the lahaina blob copy\n", name);
		drm_property_blob_put(blob);
		return NULL;
	}

	memcpy(copy->data, blob->data, l->kept);
	drm_property_blob_put(blob);
	return copy;
}
