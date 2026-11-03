# Fixed multi-architecture Debian 10 base; compatible with the UOS 20 ABI baseline.
FROM debian:buster-slim@sha256:bb3dc79fddbca7e8903248ab916bb775c96ec61014b3d02b4f06043b604726dc
ENV DEBIAN_FRONTEND=noninteractive QT_QPA_PLATFORM=offscreen LANG=C.UTF-8
COPY install-build-deps.sh /opt/dcc-proxy-ci/
RUN bash /opt/dcc-proxy-ci/install-build-deps.sh
COPY build-dtk.sh dtk-sources.tsv /opt/dcc-proxy-ci/
RUN bash /opt/dcc-proxy-ci/build-dtk.sh && rm -rf /tmp/dcc-proxy-dtk
WORKDIR /project
