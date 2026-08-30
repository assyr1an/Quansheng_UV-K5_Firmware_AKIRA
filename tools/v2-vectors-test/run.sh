#!/usr/bin/env bash
# Compile the FIRMWARE's own v2 codec on the host and check it against the
# reference vectors. No radio needed.
#
# If you have a native gcc, this runs directly. Otherwise it can run inside the
# same container image that builds the firmware:
#
#   docker build -t akira .          # from the repository root
#   bash tools/v2-vectors-test/run.sh --docker
set -e
cd "$(dirname "$0")"
FW="$(cd ../.. && pwd)"

python gen_vectors.py

if [ "$1" = "--docker" ]; then
  MSYS_NO_PATHCONV=1 docker run --rm \
    -v "$(cd "$FW" && pwd -W 2>/dev/null || echo "$FW")":/fw \
    -w /fw/tools/v2-vectors-test \
    akira //bin/bash -c '
      set -e
      gcc -std=c11 -Wall -Wextra -Werror -O2 -I. -I/fw \
          vectest.c /fw/helper/v2frame.c /fw/helper/poly1305.c \
          /fw/external/chacha/chacha.c -o /tmp/vectest
      /tmp/vectest
    '
else
  gcc -std=c11 -Wall -Wextra -Werror -O2 -I. -I"$FW" \
      vectest.c "$FW/helper/v2frame.c" "$FW/helper/poly1305.c" \
      "$FW/external/chacha/chacha.c" -o vectest.bin
  ./vectest.bin
  rm -f vectest.bin
fi
