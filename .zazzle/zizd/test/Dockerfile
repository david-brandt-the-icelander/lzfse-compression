# The test step of lzfse-compression's zizd pipeline (see ../README.md).
# Toolchain: Ubuntu 24.04 as of the apt snapshot below (the same compilers as GitHub's ubuntu-24.04 runners).
FROM ubuntu:noble-20261110@sha256:73b999393d07f2bf65a40373778a44dc9a6d54ad7718d29247d8ff9413c4d6a8
RUN apt-get update --snapshot 20261115T000000Z \
 && apt-get install -y --no-install-recommends make=4.3-4.1build2 gcc=4:13.2.0-7ubuntu1 libc6-dev \
      clang-18=1:18.1.3-1ubuntu1 clang-format-18=1:18.1.3-1ubuntu1 libclang-rt-18-dev=1:18.1.3-1ubuntu1 \
      shellcheck=0.9.0-1 python3 git ca-certificates xxd \
 && ln -s /usr/bin/clang-18 /usr/local/bin/clang && ln -s /usr/bin/clang-format-18 /usr/local/bin/clang-format \
 && rm -rf /var/lib/apt/lists/*
COPY run.sh /run.sh
CMD ["bash", "/run.sh"]
