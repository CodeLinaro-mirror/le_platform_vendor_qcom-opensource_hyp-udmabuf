# SPDX-License-Identifier: GPL-2.0

HYP_UDMABUF_ROOT=$(PWD)
KBUILD_OPTIONS += HYP_UDMABUF_ROOT=$(HYP_UDMABUF_ROOT)

all: clean modules

modules:
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) $(KBUILD_OPTIONS) modules

modules_install:
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) modules_install

clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) clean
