LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
    src/AndroidDesktop.cpp \
    src/AndroidPixelBuffer.cpp \
    src/AndroidSocket.cpp \
    src/InputDevice.cpp \
    src/VirtualDisplay.cpp \
    src/main.cpp

#LOCAL_SRC_FILES += \
#    aidl/org/chemlab/IVNCService.aidl

LOCAL_C_INCLUDES += \
    $(LOCAL_PATH)/src \
    external/tigervnc/common \

LOCAL_SHARED_LIBRARIES := \
    libbinder \
    libcrypto \
    libcutils \
    libgui \
    libjpeg \
    liblog \
    libssl \
    libui \
    libutils \
    libz

LOCAL_STATIC_LIBRARIES += \
    libtigervnc

LOCAL_CFLAGS := -DVNCFLINGER_VERSION="1.0"
LOCAL_CFLAGS += -Ofast -Werror -std=c++14 -fexceptions

#LOCAL_CFLAGS += -DLOG_NDEBUG=0
#LOCAL_CXX := /usr/bin/include-what-you-use

LOCAL_INIT_RC := etc/vncflinger.rc

LOCAL_MODULE := vncflinger

LOCAL_MODULE_TAGS := optional

# Mason product builds only
ifneq ($(VNCFLINGER_DESKTOP_NAME),)
	LOCAL_CFLAGS += -DDESKTOP_NAME=\"$(VNCFLINGER_DESKTOP_NAME)\"
endif

LOCAL_VENDOR_MODULE := true

include $(BUILD_EXECUTABLE)
