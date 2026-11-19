FROM gcc:13-bookworm
WORKDIR /app
COPY ludo_server.c index.html style.css ./
RUN gcc -O2 -pthread ludo_server.c -o ludo_server
EXPOSE 8080
CMD ["./ludo_server"]