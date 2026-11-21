# Army Men reverse-engineering / patching / test container
#  - Ghidra (headless) + JDK 21   : disassembly & decompilation
#  - Node 22 + REA (rea-agents)   : MCP analysis server
#  - Python 3                     : patch scripts
#  - Wine (32-bit) + Xvfb         : reproduce Proton-like ddraw behaviour, screenshots
FROM debian:trixie-slim

ARG GHIDRA_VERSION=12.1.4
ARG GHIDRA_BUILD=20260921
ARG REA_VERSION=6.8.0
ARG NODE_MAJOR=22

ENV DEBIAN_FRONTEND=noninteractive \
    LANG=C.UTF-8

RUN dpkg --add-architecture i386 \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates curl unzip xz-utils gnupg \
      openjdk-21-jdk-headless \
      python3 python3-pip python3-venv \
      wine wine32:i386 wine64 \
      xvfb x11-utils xdotool imagemagick \
      libgl1:i386 libgl1-mesa-dri:i386 libglx-mesa0:i386 libvulkan1:i386 mesa-vulkan-drivers:i386 \
      libgl1 libgl1-mesa-dri libglx-mesa0 \
      binutils file procps \
 && rm -rf /var/lib/apt/lists/*

# Node.js from NodeSource
RUN curl -fsSL https://deb.nodesource.com/gpgkey/nodesource-repo.gpg.key \
      | gpg --dearmor -o /usr/share/keyrings/nodesource.gpg \
 && echo "deb [signed-by=/usr/share/keyrings/nodesource.gpg] https://deb.nodesource.com/node_${NODE_MAJOR}.x nodistro main" \
      > /etc/apt/sources.list.d/nodesource.list \
 && apt-get update && apt-get install -y --no-install-recommends nodejs \
 && rm -rf /var/lib/apt/lists/*

# Ghidra
RUN curl -fsSL -o /tmp/ghidra.zip \
      "https://github.com/NationalSecurityAgency/ghidra/releases/download/Ghidra_${GHIDRA_VERSION}_build/ghidra_${GHIDRA_VERSION}_PUBLIC_${GHIDRA_BUILD}.zip" \
 && unzip -q /tmp/ghidra.zip -d /opt \
 && mv /opt/ghidra_${GHIDRA_VERSION}_PUBLIC /opt/ghidra \
 && rm /tmp/ghidra.zip
ENV GHIDRA_INSTALL_DIR=/opt/ghidra \
    PATH=/opt/ghidra/support:$PATH

# REA
RUN npm install --global rea-agents@${REA_VERSION} && npm cache clean --force

# mingw-w64 i686: builds our ddraw.dll replacement (src/ddraw)
RUN apt-get update \
 && apt-get install -y --no-install-recommends gcc-mingw-w64-i686 make \
 && rm -rf /var/lib/apt/lists/*

# Unprivileged user; Wine prefix lives in its home
RUN useradd -m -u 1000 re && mkdir -p /home/re/.wine && chown re:re /home/re/.wine
USER re
ENV WINEPREFIX=/home/re/.wine \
    WINEARCH=win32 \
    WINEDEBUG=-all
WORKDIR /work
