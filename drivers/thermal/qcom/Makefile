# SPDX-License-Identifier: GPL-2.0-only
ccflags-y += -I$(srctree)
obj-$(CONFIG_QCOM_TSENS)	+= qcom_tsens.o

qcom_tsens-y			+= tsens.o tsens-v2.o tsens-v1.o tsens-v0_1.o \
				   tsens-8960.o
obj-$(CONFIG_QCOM_SPMI_ADC_TM5)	+= qcom-spmi-adc-tm5.o
obj-$(CONFIG_QCOM_SPMI_TEMP_ALARM)	+= qcom-spmi-temp-alarm.o
obj-$(CONFIG_QCOM_LMH)		+= lmh.o
obj-$(CONFIG_QTI_THERMAL_LIMITS_DCVS)	+= msm_lmh_dcvs.o
