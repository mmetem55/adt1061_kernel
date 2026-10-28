# Aidata ADT1061 (Unisoc UMS512 / sharkl5pro) Android GKI 6.12.93 Linux Kernel

Unisoc UMS512 vendor **4.14.133** çekirdeğinin,
**Android Common Kernel (GKI) 6.12.93** üzerine portudur.

Bu depo bir ACK `common/` ağacıdır. **GKI çekirdeğine dokunulmaz** (`Image` değişmez); vendor sürücüleri
Kleaf/Bazel **DDK modülü** olarak `adt1061/modules/` altında derlenir, cihaz ağacı (DT) ise
`arch/arm64/boot/dts/sprd/` altındadır.

## Durum özeti

| Alan | Durum |
|---|---|
| 8 çekirdek SMP, GIC, arch timer, seri konsol (ttyS1) | Çalışıyor |
| eMMC / SD / SDIO | Çalışıyor |
| Ekran (DPU/DSI, 1200x1920@58) | Çalışıyor |
| GPU Mali-G52 MP2 | Çalışıyor — vendor Mali yerine açık kaynak **Panfrost** |
| WiFi (SC2355) | Çalışıyor — `probe_ie=0` zorunlu (aşağıya bakın); yalnız 2,4 GHz sınandı |
| RTC (SC2730) | Çalışıyor |
| CPU DVFS (iki küme ayrı: CPU0-5 / CPU6-7) | Çalışıyor |
| CPU termal kısma (`sprd_cpu_cooling`) | Çalışıyor — GKI'da `power_allocator` yok, `step_wise` ile 85 °C'de devreye girer |
| Şarj, fuel gauge, PMIC (SC2730), USB, ses, BT/FM | Sürücü bağlı, test edilmedi |
| Kamera zinciri, sensorhub/CM4, Trusty/TEE, parmak izi, CoreSight | henüz modül olarak eklenmedi |
| Modem | IPC sürücüleri bağlı, CP boot edilmiyor |
| CPU derin uyku | Kapalı — DTB'de idle parametresi `0`; açıkken erken açılışta donuyor, hatanın sebebi henüz bulunamadı |

## Depo yapısı

```
adt1061/
  BUILD.bazel          kernel_build(adt1061_kernel): base_kernel=//common:kernel_aarch64, DTB hedefi
  adt1061.fragment     defconfig eki
  keys/                modül imzalama anahtarı
  modules/
    BUILD.bazel        her modül için ddk_module + kernel_module_group(adt1061_modules)
    sprd_*/            vendor SoC sürücüleri (6.12 API'sine uyarlanmış)
    panfrost/ gpu_sched/   GPU (Panfrost + DRM scheduler modül olarak)
    snd_soc_sprd/      ses
arch/arm64/boot/dts/sprd/
  adt1061.dts, aidata-sharkl5Pro.dtsi, aidata-ums512.dtsi, sc2730.dtsi, lcd/ ...
```

Önemli notlar:

- **`sprd_wlan`:** SC2355 firmware'i 128 baytı aşan probe-request IE'si ile çöker. `probe_ie` modül parametresi
  varsayılan `0`'dır (IE gönderilmez); taramaların çalışması için bu gereklidir.
- **U-Boot** `boot.img` komut satırını yok sayar; çekirdek parametrelerini değil DTB'yi (`/chosen`) düzenleyin.

- **Upstream U-Boot** ile --kernel_offset 0x80080000, --ramdisk_offset 0x88000000, --dtb_offset 0x89000000 böyle olmalıdır stok U-Boot ile ise --kernel_offset 0x00008000, --ramdisk_offset 0x05400000, --dtb_offset 0x01204800 olmalıdır.

## Derleme

Ağaç, standart Android çekirdek (Kleaf) kaynak ağacında `common/` olarak bulunmalıdır
(ör. `android16-6.12` manifesti; bu depo `common/` dizinine klonlanır).

```bash
# kaynak ağacın kökünden (common/ = bu depo)
tools/bazel build //common/adt1061/modules:adt1061_modules        # tüm modüller
tools/bazel build //common/adt1061/modules:sprd_gpu_pd           # tek modül
tools/bazel build //common/adt1061:adt1061_kernel                # DTB (adt1061.dtb)
tools/bazel build //common/adt1061/modules:adt1061_modules_install
```

- `adt1061/keys/signing_key.pem` yalnız geliştirme amaçlı bir modül imzalama anahtarıdır

Cihaza yazmadan önce yedek alın; bu depo henüz deneysel aşamadadır.

## Bilinen sorunlar

- CPU derin uyku (PSCI `0x10000`) açıldığında çekirdek ~2 sn'de sessizce donuyor; DTB'de `arm,psci-suspend-param = 0`.
- CPU hotplug/çekirdek izolasyonu soğutma için portlanmadı (`sprd_core_ctl` izolasyonu henüz tam değil).
- Modem CP, kamera, sensorhub, Trusty henüz eklenmedi.
- WiFi 5 GHz ve Bluetooth/FM/ses işlevleri test edilmedi.
