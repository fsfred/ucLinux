cmd_/work/psl/SF/release/cm-2.0.0/linux-cortexm-2.0.0/A2F/root/usr//include/linux/spi/.install := /bin/sh scripts/headers_install.sh /work/psl/SF/release/cm-2.0.0/linux-cortexm-2.0.0/A2F/root/usr//include/linux/spi ./include/uapi/linux/spi spidev.h; /bin/sh scripts/headers_install.sh /work/psl/SF/release/cm-2.0.0/linux-cortexm-2.0.0/A2F/root/usr//include/linux/spi ./include/linux/spi ; /bin/sh scripts/headers_install.sh /work/psl/SF/release/cm-2.0.0/linux-cortexm-2.0.0/A2F/root/usr//include/linux/spi ./include/generated/uapi/linux/spi ; for F in ; do echo "\#include <asm-generic/$$F>" > /work/psl/SF/release/cm-2.0.0/linux-cortexm-2.0.0/A2F/root/usr//include/linux/spi/$$F; done; touch /work/psl/SF/release/cm-2.0.0/linux-cortexm-2.0.0/A2F/root/usr//include/linux/spi/.install
