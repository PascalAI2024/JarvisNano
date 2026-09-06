# The host-test toolchain for a machine with no C compiler (a Windows desk).
# scripts/host-tests.sh builds this on first use as `jarvisnano-hosttests`:
#
#   docker build -t jarvisnano-hosttests - < scripts/host-tests.Dockerfile
#
# gcc:14 is Debian with GCC 14.4; the four C suites need only cmake, a
# generator and libc. python3 is here so the image can also run the desk
# suite by hand, though host-tests.sh runs that one natively.
FROM gcc:14
RUN apt-get update -qq && apt-get install -y -qq cmake ninja-build python3 >/dev/null \
    && rm -rf /var/lib/apt/lists/*
