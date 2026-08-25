FROM debian:bookworm-slim

# Das ist jetzt nur eine Optimierung, aber ich installiere mir hier gern alle
# moeglichen netzwerk-tools, damit man sich bei Bedarf einfach per kubectl exec
# in den pod einloggen und tcpdump usw laufen lassen kann.

ENV DEBIAN_FRONTEND=noninteractive
ENV DISPLAY=0

COPY . /app
WORKDIR /app

RUN apt-get update && \
    apt-get upgrade -y && \
    apt-get install -y devscripts && \
    mk-build-deps \
        -ir \
        -t "apt-get \
        -o Debug::pkgProblemResolver=yes \
        --no-install-recommends \
        -o APT::Get::Assume-Yes=1" && \
    apt-get install -y libelf-dev linuxptp iproute2 net-tools dnsutils pkg-config m4 bpftool libpcap-dev &&\
    rm -rf /var/lib/apt/lists/*

RUN mkdir build &&\
    cd build && \
    cmake .. -DUSE_LIBXDP_FROM_SUBMODULE=ON -DBPF_CFLAGS=-I/usr/include/x86_64-linux-gnu &&\
    cmake --build .
WORKDIR /app/build/src

# With ptp synchronisation
#ENTRYPOINT [ "./start_sync_and_tsn_app_agent.sh" ] 
# Without synchronisation (only TSN App as Agent)
#ENTRYPOINT [ "./tsn_test_app", "-A", "-i", "enp2s0", "-s", "AL"]

EXPOSE 65000

