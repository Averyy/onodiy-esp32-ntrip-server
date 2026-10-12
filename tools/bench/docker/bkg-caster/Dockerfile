# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# BKG's Standard NtripCaster (NTRIP 1.0, GPL) as packaged by
# github.com/goblimey/ntripcaster, built at a pinned commit the way its
# README describes (./configure, make, make install; configuration files in
# /usr/local/ntripcaster/conf; started from the logs directory so the log
# lands there). A local test peer for the station's caster client: our own
# configuration (README.md here), every credential made up. Built and run by
# build.sh / run.sh; nothing of the caster is copied into this repository.

# debian:bookworm-slim, pinned by digest (multi-platform index).
ARG BASE=debian@sha256:7c7b2c966bc9ee8cedfeef67e0e279108992c77681fa595db4a9d65c06ccc587

FROM ${BASE} AS build
ARG CASTER_REPO=https://github.com/goblimey/ntripcaster.git
# master on 2026-10-12 ("Merge pull request #6 ... Update README.md for git clone command", 2024-07-27).
ARG CASTER_COMMIT=fefcbaf34eba9b0533bed9c0451cbaeecdf08e3f
RUN apt-get update \
    && apt-get install -y --no-install-recommends build-essential ca-certificates git \
    && rm -rf /var/lib/apt/lists/*
RUN git clone "$CASTER_REPO" /src \
    && git -C /src checkout --detach "$CASTER_COMMIT" \
    && test "$(git -C /src rev-parse HEAD)" = "$CASTER_COMMIT"
WORKDIR /src/ntripcaster
# The README's step: the configuration files go into conf/ before the build
# (ours instead of edited copies of the examples).
COPY ntripcaster.conf sourcetable.dat conf/
# 2008-era C: today's GCC defaults to -fno-common and newer C standards;
# ask for the old behaviour instead of changing the code.
RUN ./configure CFLAGS="-O2 -fcommon -std=gnu89 -w" && make && make install

FROM ${BASE}
COPY --from=build /usr/local/ntripcaster /usr/local/ntripcaster
WORKDIR /usr/local/ntripcaster/logs
EXPOSE 2101
CMD ["/usr/local/ntripcaster/bin/ntripcaster"]
