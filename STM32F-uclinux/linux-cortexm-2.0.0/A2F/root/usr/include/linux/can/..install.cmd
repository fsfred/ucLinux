cmd_/work/psl/SF/release/cm-2.0.0/linux-cortexm-2.0.0/A2F/root/usr//include/linux/can/.install := /bin/sh scripts/headers_install.sh /work/psl/SF/release/cm-2.0.0/linux-cortexm-2.0.0/A2F/root/usr//include/linux/can ./include/uapi/linux/can bcm.h error.h gw.h netlink.h raw.h; /bin/sh scripts/headers_install.sh /work/psl/SF/release/cm-2.0.0/linux-cortexm-2.0.0/A2F/root/usr//include/linux/can ./include/linux/can ; /bin/sh scripts/headers_install.sh /work/psl/SF/release/cm-2.0.0/linux-cortexm-2.0.0/A2F/root/usr//include/linux/can ./include/generated/uapi/linux/can ; for F in ; do echo "\#include <asm-generic/$$F>" > /work/psl/SF/release/cm-2.0.0/linux-cortexm-2.0.0/A2F/root/usr//include/linux/can/$$F; done; touch /work/psl/SF/release/cm-2.0.0/linux-cortexm-2.0.0/A2F/root/usr//include/linux/can/.install
