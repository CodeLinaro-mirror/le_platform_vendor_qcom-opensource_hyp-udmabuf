# SPDX-License-Identifier: GPL-2.0

HYP_UDMABUF_ROOT=$(M)
KBUILD_OPTIONS += HYP_UDMABUF_ROOT=$(HYP_UDMABUF_ROOT)

all: clean modules

modules:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) $(KBUILD_OPTIONS) modules

modules_install:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) modules_install

clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) clean
