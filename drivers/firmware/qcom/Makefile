# SPDX-License-Identifier: GPL-2.0
ccflags-y += -I$(srctree)
#
# Makefile for the linux kernel.
#

obj-$(CONFIG_QCOM_SCM)		+= qcom-scm.o
qcom-scm-objs += qcom_scm.o qcom_scm-smc.o qcom_scm-legacy.o qcom_scm-scm.o
qcom-scm-$(CONFIG_QTEE_SHM_BRIDGE) += qtee_shmbridge.o
obj-$(CONFIG_QCOM_TZMEM)	+= qcom_tzmem.o
obj-$(CONFIG_QCOM_QSEECOM)	+= qcom_qseecom.o
obj-$(CONFIG_QCOM_QSEECOM_UEFISECAPP) += qcom_qseecom_uefisecapp.o
