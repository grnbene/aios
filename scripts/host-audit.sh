#!/usr/bin/env sh
# Where does the OS touch the hardware host? Lists every non-standard-library
# header included under include/ and src/. The answer must be: only hal/ (the
# host boundary) and the intelligence adapters (dlopen/fork, outside the kernel).
cd "$(dirname "$0")/.."
grep -rnoE '#include <(sys/[a-z_/]+|linux/[a-z_/]+|unistd|fcntl|dirent|dlfcn|poll|signal|termios|netinet/[a-z]+|arpa/[a-z]+|netdb|CL/cl)\.h>' include src | sort | awk -F: '{print $1": "$3}' | uniq
