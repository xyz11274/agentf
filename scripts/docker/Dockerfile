# AgentFlow Server Dockerfile
#
# Multi-stage build for optimized image size

# Build stage
FROM ubuntu:22.04 AS builder

# Install build dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    pkg-config \
    libssl-dev \
    libevent-dev \
    libprotobuf-dev \
    protobuf-compiler \
    rapidjson-dev \
    && rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /build

# Copy source code
COPY . .

# Build
RUN mkdir -p build && cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=Release \
             -DAGENTFLOW_BUILD_TESTS=OFF \
             -DAGENTFLOW_BUILD_EXAMPLES=OFF && \
    make -j$(nproc)

# Runtime stage
FROM ubuntu:22.04 AS runtime

# Install runtime dependencies
RUN apt-get update && apt-get install -y \
    libssl3 \
    libevent-2.1-7 \
    libprotobuf23 \
    curl \
    && rm -rf /var/lib/apt/lists/*

# Create non-root user
RUN useradd -r -s /bin/false agentflow

# Set working directory
WORKDIR /app

# Copy binaries from builder
COPY --from=builder /build/build/agentflow_server /app/bin/ 2>/dev/null || true
COPY --from=builder /build/build/libagentflow.a /app/lib/ 2>/dev/null || true

# Copy configuration files
COPY configs/ /app/configs/

# Create data directory
RUN mkdir -p /app/data && chown -R agentflow:agentflow /app

# Switch to non-root user
USER agentflow

# Expose ports
EXPOSE 50051 8080 8081 9090

# Health check
HEALTHCHECK --interval=30s --timeout=10s --start-period=5s --retries=3 \
    CMD curl -f http://localhost:8080/health || exit 1

# Set environment variables
ENV AGENTFLOW_CONFIG_PATH=/app/configs/server.yaml
ENV AGENTFLOW_LOG_LEVEL=INFO

# Entry point
ENTRYPOINT ["/app/bin/agentflow_server"]
CMD ["--config", "/app/configs/server.yaml"]
