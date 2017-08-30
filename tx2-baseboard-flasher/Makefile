KERNEL_PATH=../kernel/kernel-4.4
DTB_FILE=tegra186-quill-p3310-1000-c03-00-base.dtb
CUR_DIR=$(shell pwd)

all: modules create-pkg

modules:
	mkdir -p mod-out
	ARCH=arm64 INSTALL_MOD_PATH=$(CUR_DIR)/mod-out make modules_install -C $(KERNEL_PATH)
	rm mod-out/lib/modules/4.4.38-antmicro/build
	rm mod-out/lib/modules/4.4.38-antmicro/source

linux-headers:
	rsync -avz --existing ../kernel/kernel-4.4/ resources/linux-headers-4.4.38-antmicro

create-pkg: modules linux-headers
	# populate resources
	cp $(KERNEL_PATH)/arch/arm64/boot/Image resources/Image
	cp $(KERNEL_PATH)/arch/arm64/boot/dts/$(DTB_FILE) resources/$(DTB_FILE)
	cp -r mod-out/lib/modules/4.4.38-antmicro resources/modules-4.4.38-antmicro
	zip -r tx2-bb-flasher.zip tx2-flash.sh README-*.rst resources

clean:
	rm -rf resources/Image
	rm -rf resources/$(DTB_FILE)
	rm -rf resources/modules-4.4.38-antmicro
	rm -rf tx2-bb-flasher.zip
	rm -rf mod-out
