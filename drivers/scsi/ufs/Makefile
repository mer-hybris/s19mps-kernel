# SPDX-License-Identifier: GPL-2.0
# UFSHCD makefile
obj-$(CONFIG_SCSI_UFS_DWC_TC_PCI) += tc-dwc-g210-pci.o ufshcd-dwc.o tc-dwc-g210.o
obj-$(CONFIG_SCSI_UFS_DWC_TC_PLATFORM) += tc-dwc-g210-pltfrm.o ufshcd-dwc.o tc-dwc-g210.o
obj-$(CONFIG_SCSI_UFS_CDNS_PLATFORM) += cdns-pltfrm.o
obj-$(CONFIG_SCSI_UFS_QCOM) += ufs_qcom.o
ufs_qcom-y += ufs-qcom.o
ufs_qcom-$(CONFIG_SCSI_UFS_CRYPTO) += ufs-qcom-ice.o
obj-$(CONFIG_SCSI_UFSHCD) += ufshcd-core.o
ufshcd-core-y				+= ufshcd.o ufs-sysfs.o ufs-sprd-debug.o
ufshcd-core-$(CONFIG_SCSI_UFS_BSG)	+= ufs_bsg.o
ufshcd-core-$(CONFIG_SCSI_UFS_CRYPTO) += ufshcd-crypto.o
ufshcd-core-$(CONFIG_SCSI_UFS_HPB)	+= ufshpb.o
obj-$(CONFIG_SCSI_UFSHCD_PCI) += ufshcd-pci.o
obj-$(CONFIG_SCSI_UFSHCD_PLATFORM) += ufshcd-pltfrm.o
obj-$(CONFIG_SCSI_UFS_HISI) += ufs-hisi.o
obj-$(CONFIG_SCSI_UFS_MEDIATEK) += ufs-mediatek.o
obj-$(CONFIG_SCSI_UFS_SPRD_QOGIRL6) += ufs-sprd_qogirl6.o
ufs-sprd_qogirl6-y += ufs-sprd-qogirl6.o ufs-sprd-ioctl.o ufs-sprd-pwr-debug.o
obj-$(CONFIG_SCSI_UFS_SPRD_QOGIRN6PRO) += ufs-sprd-qogirn6pro.o
ufs-sprd-qogirn6pro-y += ufs-sprd_qogirn6pro.o ufs-sprd-pwr-debug.o
