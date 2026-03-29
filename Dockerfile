# ============================================================================
# SOFTWARE: OSL: Sovereign Accounting Suite
# AUTHOR & COPYRIGHT: Cel-Tech-Serv Pty Ltd
# MODULE: Landlord Plugin Build Container
# ============================================================================

# STAGE 1: Compiler
FROM alpine:3.21 AS landlord-compiler

RUN apk add --no-cache \
    build-base \
    cmake \
    git \
    postgresql-dev \
    linux-headers \
    nlohmann-json

# Grab the single-header httplib directly and place it where the C++ compiler expects it
RUN wget https://raw.githubusercontent.com/yhirose/cpp-httplib/master/httplib.h -O /usr/include/httplib.h

WORKDIR /tmp
RUN git clone https://github.com/jtv/libpqxx.git && \
    cd libpqxx && \
    mkdir build && cd build && \
    cmake -DSKIP_BUILD_TEST=ON -DBUILD_SHARED_LIBS=ON -DCMAKE_POSITION_INDEPENDENT_CODE=ON .. && \
    make -j$(nproc) && \
    make install

WORKDIR /build_context
COPY src/plugins/official/osl-plugin-landlord/ ./src/plugins/official/osl-plugin-landlord/
COPY src/plugins/interface/ ./src/plugins/interface/

WORKDIR /build_context/src/plugins/official/osl-plugin-landlord
RUN mkdir -p build && cd build && \
    cmake .. && \
    make

# STAGE 2: Publisher
FROM alpine:3.21 AS landlord-publisher

# Create a safe source directory to hold the binary before it is moved to the volume
RUN mkdir -p /source_bin /app/dist

# Copy the binary from the compiler stage to the source directory
COPY --from=landlord-compiler /build_context/src/plugins/official/osl-plugin-landlord/build/landlord.so /source_bin/landlord.so

# At runtime, create the bin sub-folder in the shared volume and publish the binary
CMD ["sh", "-c", "mkdir -p /app/dist/bin && cp /source_bin/landlord.so /app/dist/bin/landlord.so && tail -f /dev/null"]