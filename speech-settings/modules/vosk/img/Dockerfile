FROM sd-build
USER root
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends libonnxruntime-dev libjson-glib-dev pkg-config && rm -rf /var/lib/apt/lists/*
