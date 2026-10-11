LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_MODULE :=Leanscan

LOCAL_SRC_FILES := main.cpp \
    src/memmap.cpp \
    src/scan.cpp \
    src/hitfile.cpp \

LOCAL_C_INCLUDES := \
    $(LOCAL_PATH)/src \
    $(LOCAL_PATH)/include \

# 设置编译器标志
#LOCAL_CPPFLAGS := -Wall -O3 -std=c++17
# C 文件用 C 标准
LOCAL_CFLAGS   := -std=c11 -fvisibility=hidden -O3
# C++ 文件用 C++17
LOCAL_CPPFLAGS := -std=c++17 -fvisibility=hidden -O3

include $(BUILD_EXECUTABLE)