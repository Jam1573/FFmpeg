#!/bin/bash

# 使用 uname 命令获取操作系统类型
os=$(uname)

# 根据操作系统类型输出相应的信息
if [ "$os" == "Linux" ]; then
  echo "您正在使用 Linux 操作系统"
  ./configure --prefix=/usr/local/ffmpeg \
    --disable-static \
    --disable-doc \
    --disable-htmlpages \
    --disable-manpages \
    --disable-podpages \
    --disable-txtpages \
    --enable-gpl \
    --enable-version3 \
    --enable-nonfree \
    --enable-postproc \
    --enable-libass \
    --enable-libfdk-aac \
    --enable-libfreetype \
    --enable-libmp3lame \
    --enable-libopenjpeg \
    --enable-openssl \
    --enable-libopus \
    --enable-libspeex \
    --enable-libtheora \
    --enable-libvorbis \
    --enable-libvpx \
    --enable-libx264 \
    --enable-libxvid \
    --disable-static \
    --enable-shared \
    --enable-libx265 \
    --enable-vaapi \
    --enable-decoder=h264
elif [ "$os" == "Darwin" ]; then
  echo "您正在使用 macOS 操作系统"
  ./configure --prefix=/usr/local/ffmpeg \
    --disable-static \
    --disable-doc \
    --disable-htmlpages \
    --disable-manpages \
    --disable-podpages \
    --disable-txtpages \
    --enable-gpl \
    --enable-version3 \
    --enable-nonfree \
    --enable-postproc \
    --enable-libass \
    --enable-libfdk-aac \
    --enable-libfreetype \
    --enable-libmp3lame \
    --enable-libopenjpeg \
    --enable-openssl \
    --enable-libopus \
    --enable-libspeex \
    --enable-libtheora \
    --enable-libvorbis \
    --enable-libvpx \
    --enable-libx264 \
    --enable-libxvid \
    --disable-static \
    --enable-shared \
    --enable-libx265
elif [ "$os" == "FreeBSD" ]; then
  echo "您正在使用 FreeBSD 操作系统"
elif [ "$os" == "Windows_NT" ]; then
  echo "您正在使用 Windows 操作系统"
else
  echo "未知的操作系统: $os"
fi

