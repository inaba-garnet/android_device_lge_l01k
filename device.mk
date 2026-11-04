#
# Copyright (C) 2018-2024 The LineageOS Project
#
# SPDX-License-Identifier: Apache-2.0
#

DEVICE_PATH := device/lge/l01k

# Inherit common repository
$(call inherit-product, device/lge/joan-common/joan-common.mk)

# NFC
PRODUCT_PACKAGES += \
	libhidlbase_shim

# L-01K TV RMP(保護視聴)経路。製品設定として常に有効(環境変数に依存させない)。
# 外したい試験ビルドだけ、この行を false にする。HAL/player が入ると AIDL(V1 ndk)と VINTF fragment は
# 依存として入る。policy(joan-common public/private, sepolicy/vendor)と secure MPEG-2 codec 宣言は
# 常時ビルドされ、RMP を外した場合も型が未使用になるだけで動作に影響しない。
# 旧 producer l01ktv_rf_secure は廃止(l01k-tv/archive/rmp-direct-exec)。戻さないこと。
L01KTV_RMP := true
ifeq ($(L01KTV_RMP),true)
PRODUCT_PACKAGES += \
	l01ktv_player \
	l01ktv_rmp_hal \
	l01ktv_rflock_init
endif

PRODUCT_SOONG_NAMESPACES += \
    $(DEVICE_PATH)

# Inherit proprietary blobs
$(call inherit-product, vendor/lge/l01k/l01k-vendor.mk)
