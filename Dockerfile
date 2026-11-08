FROM docker.io/library/debian:bullseye-slim

RUN sed -i 's|deb.debian.org/debian-security|archive.debian.org/debian-security|g' /etc/apt/sources.list && apt-get -o Acquire::Check-Valid-Until=false update && apt-get install -y --no-install-recommends build-essential bzip2 ca-certificates gcc-arm-none-eabi libc6-i386 openssl python2 python3 && rm -rf /var/lib/apt/lists/*
RUN python3 -c 'import sys, urllib.request; urllib.request.urlretrieve(sys.argv[1], "/tmp/gcce-linux.tar.bz2")' 'https://archive.org/download/symbian_dev/Compilers.zip/Compilers%2FGCCE%204.6.3%2Farm-2012.03-42-arm-none-symbianelf-i686-pc-linux-gnu.tar.bz2' && echo 'b341215df8732fc0d39054b6946efb30ba1eea806adfe8fe299290d6d7c0ac74  /tmp/gcce-linux.tar.bz2' | sha256sum -c - && mkdir -p /opt/gcce && tar -xjf /tmp/gcce-linux.tar.bz2 -C /opt/gcce && rm /tmp/gcce-linux.tar.bz2
COPY tools/src/elf2e32_next /tmp/elf2e32_next
RUN cd /tmp/elf2e32_next && gcc -O2 -c lib/elf/elf_hash.c -o /tmp/elf_hash.o && g++ -O2 -fpermissive -w -include cstdint -Iinclude -Ilib/e32 -Ilib/elf -Ilib/getopt -Isrc src/*.cpp lib/*/*.cpp /tmp/elf_hash.o -o /usr/local/bin/elf2e32_next && rm -rf /tmp/elf2e32_next /tmp/elf_hash.o

COPY tools/src/ensymble /opt/ensymble
ENV GCCE=/opt/gcce/arm-2012.03/bin/arm-none-symbianelf E2E=/usr/local/bin/elf2e32_next ENSYMBLE=/opt/ensymble/ensymble.py PATH=/opt/gcce/arm-2012.03/bin:$PATH PYTHONDONTWRITEBYTECODE=1
COPY tools/container/check.sh /usr/local/bin/check-symbian-toolchain
RUN bash /usr/local/bin/check-symbian-toolchain

WORKDIR /workspace
CMD ["bash"]
