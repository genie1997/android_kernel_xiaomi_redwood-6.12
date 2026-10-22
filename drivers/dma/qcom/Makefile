# SPDX-License-Identifier: GPL-2.0
ccflags-y += -I$(srctree)
obj-$(CONFIG_QCOM_ADM) += qcom_adm.o
obj-$(CONFIG_QCOM_BAM_DMA) += bam_dma.o
CFLAGS_bam_dma.o := -I$(src)
obj-$(CONFIG_QCOM_GPI_DMA) += gpi.o
obj-$(CONFIG_QCOM_HIDMA_MGMT) += hdma_mgmt.o
hdma_mgmt-objs	 := hidma_mgmt.o hidma_mgmt_sys.o
obj-$(CONFIG_QCOM_HIDMA) +=  hdma.o
hdma-objs        := hidma_ll.o hidma.o hidma_dbg.o
obj-$(CONFIG_MSM_GPI_DMA) += msm_gpi.o
