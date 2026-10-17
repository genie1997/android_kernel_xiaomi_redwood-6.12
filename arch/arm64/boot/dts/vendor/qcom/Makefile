# SPDX-License-Identifier: GPL-2.0
# Qualcomm's DT headers come before ACK's, as in Qualcomm's own DT build
DTC_INCLUDE := $(srctree)/arch/arm64/boot/dts/vendor/include $(DTC_INCLUDE)
# The audio nodes use the audio drivers' own binding numbers
DTC_INCLUDE += $(srctree)/techpack/audio/include

# Applied at build time, so a target missing from the base fails here and not in ABL
yupik-redwood-sm7325-overlay-dtbs := yupik-redwood.dtb redwood-sm7325-overlay.dtbo

dtb-$(CONFIG_ARCH_YUPIK) += yupik-redwood-sm7325-overlay.dtb yupik-redwood.dtb redwood-sm7325-overlay.dtbo
