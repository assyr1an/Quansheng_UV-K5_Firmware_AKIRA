# Pinned by digest so local builds and CI share one toolchain. Bump deliberately,
# together with .github/workflows/main.yml.
FROM --platform=amd64 archlinux@sha256:c9dc8b5d1b06d8d50ace6d42b2c93fbb1e34c9e1332d1a2102936e497d3187ae
RUN pacman -Syyu base-devel --noconfirm
RUN pacman -Syyu arm-none-eabi-gcc --noconfirm
RUN pacman -Syyu arm-none-eabi-newlib --noconfirm
RUN pacman -Syyu git --noconfirm
RUN pacman -Syyu python-pip --noconfirm
RUN pacman -Syyu python-crcmod --noconfirm
WORKDIR /app
COPY . .

RUN git submodule update --init --recursive
#RUN make && cp firmware* compiled-firmware/
