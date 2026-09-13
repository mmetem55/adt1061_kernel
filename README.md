# Aidata ADT1061 Kernel

Linux Kernel **4.14.133** source code for the **Aidata ADT1061** tablet.

## Device Information ##

| Item | Value |
|------|------|
| Device | Aidata ADT1061 |
| Kernel Version | 4.14.133 |
| Platform | Unisoc UMS512 |

## Building ##

Use Ubuntu 20.04 LTS for the best result

### Clone repository
```bash
git clone https://github.com/mmetem55/adt1061-kernel.git && cd adt1061-kernel
```

### Download and extract the prebuilt toolchain
```bash
mkdir -p prebuilts/gcc prebuilts/clang/host/linux-x86 && git clone -b android-10.0.0_r47 https://android.googlesource.com/platform/prebuilts/gcc/linux-x86/aarch64/aarch64-linux-android-4.9 prebuilts/gcc/aarch64-linux-android-4.9 && git clone -b android-10.0.0_r47 https://android.googlesource.com/platform/prebuilts/gcc/linux-x86/arm/arm-linux-androideabi-4.9 prebuilts/gcc/arm-linux-androideabi-4.9 && mkdir -p prebuilts/clang/host/linux-x86/clang-r353983c && curl -L "https://android.googlesource.com/platform/prebuilts/clang/host/linux-x86/+archive/f8901db697a294e418813287043562caa29b4614/clang-r353983c.tar.gz" | tar -xz -C prebuilts/clang/host/linux-x86/clang-r353983c
```

### Script permission configuration ###
```bash
chmod +x build-aidata-adt1061.sh && chmod +x scripts/* && chmod +x arch/arm64/kernel/vdso/gen_vdso_offsets.sh
```
### Build the kernel ###
```bash
./build-aidata-adt1061.sh
```

## Status ##

🟢 Active development

## Credits ##

- Linux Kernel Developers
- Spreadtrum/Unisoc Developer
- strongtz/linux-sprd
- mmetem55

## License ##

This project follows the original Linux Kernel (GPL-2.0) licensing.
