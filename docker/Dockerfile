# Bromelia without a window: watches the drives, rips discs as the configuration says and serves the web page.
#
# MakeMKV is built from makemkv.com's sources. Its binary part is under MakeMKV's own licence (EULA), so the build
# only runs when you accept it:
#
#   docker build -f docker/Dockerfile --build-arg ACCEPT_MAKEMKV_EULA=yes -t bromelia .
#
# See docs/docker.md for running it (drives, folders, web page token).

ARG DEBIAN=trixie

FROM debian:${DEBIAN}-slim AS build
ARG ACCEPT_MAKEMKV_EULA=no
ARG MAKEMKV_VERSION=2.0.0
RUN if [ "$ACCEPT_MAKEMKV_EULA" != "yes" ]; then \
      echo "MakeMKV's binary part is covered by its EULA (src/eula_en_linux.txt in makemkv-bin-${MAKEMKV_VERSION}.tar.gz)."; \
      echo "Build with --build-arg ACCEPT_MAKEMKV_EULA=yes to accept it."; \
      exit 1; \
    fi
# No libavcodec-dev: Debian's FFmpeg is a GPL build, and MakeMKV's library must not link it (below).
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential pkg-config ca-certificates curl xz-utils nasm \
      libssl-dev libexpat1-dev zlib1g-dev \
      meson ninja-build libglib2.0-dev libjson-glib-dev libsqlite3-dev libsoup-3.0-dev libsecret-1-dev \
    && rm -rf /var/lib/apt/lists/*

# FFmpeg for MakeMKV's library, built as LGPL. makemkvcon is closed source and loads libmakemkv, which uses FFmpeg's
# audio codecs; linking it to Debian's GPL libavcodec would make an image that can't be published. Without
# --enable-gpl or --enable-nonfree FFmpeg is LGPL 2.1+, and MakeMKV's audio codecs (FLAC, AAC, AC-3, DTS, TrueHD) are
# all FFmpeg's own. Static, so libmakemkv can't pick up Debian's GPL libavcodec.so (the ffmpeg command needs it).
# The same version as Debian's, so audio conversion matches. The checksum is of the tarball whose signature by
# FFmpeg's release key (FCF986EA15E6E293A5644F10B4322F04D67658D8) was checked when it was pinned.
ARG FFMPEG_VERSION=7.1.5
ARG FFMPEG_SHA256=de668509caf9e35e3cd162473441fdb29538c6d96ed080292b3cf9e6fc5d558f
WORKDIR /src/ffmpeg
RUN curl -fsSL "https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.xz" -o ffmpeg.tar.xz \
 && echo "${FFMPEG_SHA256}  ffmpeg.tar.xz" | sha256sum -c - \
 && tar xJf ffmpeg.tar.xz \
 && cd "ffmpeg-${FFMPEG_VERSION}" \
 && ./configure --prefix=/opt/ffmpeg-lgpl --enable-static --disable-shared --enable-pic \
      --disable-autodetect --disable-programs --disable-doc --disable-network \
      --disable-avformat --disable-avdevice --disable-avfilter --disable-swscale --disable-swresample \
 && grep -q '#define FFMPEG_LICENSE "LGPL version 2.1 or later"' config.h \
 && make -j"$(nproc)" \
 && make install \
 && install -D -m 644 -t /out/usr/share/doc/makemkv/ffmpeg LICENSE.md COPYING.LGPLv2.1 \
 && echo "FFmpeg ${FFMPEG_VERSION} (https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.xz), built into libmakemkv.so.1 as LGPL 2.1+." \
      > /out/usr/share/doc/makemkv/ffmpeg/README

# MakeMKV (makemkvcon only: no GUI), its library built against the FFmpeg above.
WORKDIR /src/makemkv
RUN curl -fsSL "https://www.makemkv.com/download/makemkv-oss-${MAKEMKV_VERSION}.tar.gz" | tar xz \
 && curl -fsSL "https://www.makemkv.com/download/makemkv-bin-${MAKEMKV_VERSION}.tar.gz" | tar xz
RUN cd "makemkv-oss-${MAKEMKV_VERSION}" \
 && export PKG_CONFIG_PATH=/opt/ffmpeg-lgpl/lib/pkgconfig \
 && ./configure --prefix=/usr --disable-gui \
      ffmpeg_CFLAGS="$(pkg-config --cflags libavcodec libavutil)" \
      ffmpeg_LIBS="$(pkg-config --static --libs libavcodec libavutil)" \
 && make -j"$(nproc)" \
 && make install DESTDIR=/out
# makemkv-bin asks for the EULA interactively; it was accepted above.
RUN cd "makemkv-bin-${MAKEMKV_VERSION}" \
 && mkdir -p tmp && echo accepted > tmp/eula_accepted \
 && make install PREFIX=/usr DESTDIR=/out

# Bromelia's core and daemon.
COPY linux /src/bromelia/linux
COPY shared /src/bromelia/shared
RUN cd /src/bromelia/linux \
 && meson setup build -Dui=false --prefix=/usr --buildtype=release \
 && ninja -C build \
 && meson test -C build --print-errorlogs \
 && DESTDIR=/out meson install -C build

FROM debian:${DEBIAN}-slim
# mkvmerge (checks and episode splitting), ffmpeg + tesseract (episode numbers from DVD menus), eject (trays),
# udev (disc types), curl (notifications, online lookup, beta key), abcde / cyanrip (audio CDs).
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates curl eject udev mkvtoolnix ffmpeg tesseract-ocr tesseract-ocr-eng \
      libjson-glib-1.0-0 libexpat1 zlib1g glib-networking \
      abcde cd-discid cdparanoia flac libmusicbrainz-discid-perl libwebservice-musicbrainz-perl \
    && (apt-get install -y --no-install-recommends cyanrip || echo "cyanrip is not available; abcde rips audio CDs") \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /out/usr /usr
COPY docker/entrypoint.sh /usr/local/bin/bromelia-entrypoint
RUN chmod 755 /usr/local/bin/bromelia-entrypoint && ldconfig

# /config: Bromelia's configuration and history, and MakeMKV's settings (~/.MakeMKV). /output: the rips.
ENV HOME=/config XDG_CONFIG_HOME=/config XDG_DATA_HOME=/config/data
VOLUME ["/config", "/output"]
EXPOSE 51280
ENTRYPOINT ["/usr/local/bin/bromelia-entrypoint"]
