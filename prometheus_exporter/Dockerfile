# Executed in the context of the parent directory es_to_es
FROM debian:bookworm-slim

ENV DEBIAN_FRONTEND=noninteractive
ENV DISPLAY=0

COPY . /es_to_es
WORKDIR /es_to_es

RUN apt-get update && \
    apt-get upgrade -y && \
    apt-get install -y cmake gcc llvm clang libelf-dev linuxptp && \
    apt-get install -y sudo && \
    apt-get install -y iproute2 ethtool && \
    apt-get install -y vim && \
    apt-get install -y tcpdump && \
    apt-get install -y pkg-config m4 bpftool libpcap-dev && \
    rm -rf /var/lib/apt/lists/*

RUN mkdir build && \
    cd build && \
    cmake .. -DUSE_LIBXDP_FROM_SUBMODULE=ON -DBPF_CFLAGS=-I/usr/include/x86_64-linux-gnu && \
    cmake --build .

COPY prometheus_exporter/app /app

RUN cp /es_to_es/build/src/tsn_test_app /app

RUN apt-get update && \
    apt-get install -y python3 python3-pip

WORKDIR /app

RUN pip3 install -r requirements.txt --break-system-packages

EXPOSE 9102/tcp

ENV TSN_APP_MODE="-l"
ENV TSN_APP_INTERFACE="enp2s0"
ENV TSN_APP_TIMESTAMPING="AL"
ENV TSN_APP_PROVISION_TIME="0"
ENV TSN_APP_TRANSMISSION_OFFSET="0"
ENV TSN_APP_PAUSE_TIME="0"

# Environment variables only for TSNTest App Prometheus Exporter.
# The exporter only starts if also TSN_APP_MODE="-l"

# container|native|k8s
ENV TSN_APP_EXPORTER_MODE="container"
ENV TSN_APP_EXPORTER_LISTEN_PORT=9102
ENV TSN_APP_EXPORTER_LOG_LEVEL="DEBUG"

# Environment variables only for talkers
ENV TSN_APP_TALKER_PACKETS=15000
ENV TSN_APP_TALKER_VLAN_ID=42
ENV TSN_APP_TALKER_VLAN_PRIORITY=0
ENV TSN_APP_TALKER_DST_MAC="03:7A:CE:01:02:03"
ENV TSN_APP_TALKER_DST_DNS="svc-t1-fabos-lab-jochen.fabos-lab-jochen.svc.cluster.local"
ENV TSN_APP_TALKER_PACKET_LENGTH="1500"
ENV TSN_APP_TALKER_CYCLE_TIME="1000000"

# Environmnet variables only for listeners
ENV TSN_APP_LISTENER_LOGS="-F"
ENV TSN_APP_LISTENER_INFINITY="-I"
ENV TSN_APP_LISTENER_DUMMY_IP="1.2.3.4"
ENV TSN_APP_LISTENER_QUIET="-q"

STOPSIGNAL SIGINT

#ENTRYPOINT ["./start.sh"]
