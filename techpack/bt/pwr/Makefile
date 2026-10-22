ccflags-y += -I$(BT_ROOT)/include
ifeq ($(CONFIG_MSM_BT_CONVERGED), y)
ccflags-y += -I$(BT_ROOT)/../wlan/platform/inc
ccflags-y += -I$(BT_ROOT)/../wlan/platform/cnss_utils
endif

ifeq ($(MODNAME),)
obj-$(CONFIG_MSM_BT_POWER)  += btpower.o
else
obj-$(CONFIG_MSM_BT_POWER)  += $(MODNAME).o
ccflags-y += -DQCA_AUTO_SECONDARY
$(MODNAME)-y := btpower.o
endif
