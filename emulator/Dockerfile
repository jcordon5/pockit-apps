FROM --platform=linux/amd64 debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    gcc libc6-dev libgtk2.0-0 libpangoxft-1.0-0 libicu72 libjsoncpp25 libcurl4 libssl3 libsqlite3-0 libfreetype6 libjpeg62-turbo libpng16-16 \
    xvfb curl ca-certificates x11-apps imagemagick fonts-dejavu-core && rm -rf /var/lib/apt/lists/*
WORKDIR /work
