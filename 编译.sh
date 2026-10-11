# 用法：
# 不改就用下面这个默认路径（改成你自己的 NDK 路径）：
NDK=${NDK:-/data/AAXX/android-ndk-r24}

# 分享一个支持 MT管理器 命令调用编译的 ndk-r24
# https://www.123912.com/s/RNc8Vv-1FD23
# 欢迎加群交流 614302184

"$NDK/ndk-build" clean
"$NDK/ndk-build"