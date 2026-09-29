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
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential pkg-config ca-certificates curl \
      libssl-dev libexpat1-dev libavcodec-dev zlib1g-dev \
      meson ninja-build libglib2.0-dev libjson-glib-dev \
    && rm -rf /var/lib/apt/lists/*

# MakeMKV (makemkvcon only: no GUI).
WORKDIR /src/makemkv
RUN curl -fsSL "https://www.makemkv.com/download/makemkv-oss-${MAKEMKV_VERSION}.tar.gz" | tar xz \
 && curl -fsSL "https://www.makemkv.com/download/makemkv-bin-${MAKEMKV_VERSION}.tar.gz" | tar xz
RUN cd "makemkv-oss-${MAKEMKV_VERSION}" \
 && ./configure --prefix=/usr --disable-gui \
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
      libjson-glib-1.0-0 libexpat1 zlib1g \
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
