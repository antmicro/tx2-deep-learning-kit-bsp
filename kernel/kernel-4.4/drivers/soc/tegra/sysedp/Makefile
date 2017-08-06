GCOV_PROFILE := y

obj-$(CONFIG_TEGRA_SYS_EDP) += sysedp.o
obj-$(CONFIG_TEGRA_SYS_EDP) += sysedp_sysfs.o
obj-$(CONFIG_TEGRA_SYS_EDP) += sysedp_dynamic_capping.o
obj-$(CONFIG_TEGRA_SYS_EDP) += sysedp_reactive_capping.o
obj-$(CONFIG_TEGRA_SYS_EDP) += sysedp_batmon_calc.o
ifdef CONFIG_DEBUG_FS
obj-$(CONFIG_TEGRA_SYS_EDP) += sysedp_debug.o
endif
