FROM buildpack-deps AS builder

RUN apt-get update && \
    apt-get install -y nodejs

WORKDIR /src

ADD . .

RUN ./do



# FROM scratch
FROM gcr.io/distroless/cc

COPY --from=builder /src/cjdroute /cjdroute

ENTRYPOINT ["/cjdroute", "--nobg"]
