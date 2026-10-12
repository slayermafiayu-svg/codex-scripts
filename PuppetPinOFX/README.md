# Puppet Pin – VEGAS Pro için OFX kukla (puppet) efekti

**Puppet Pin**, VEGAS Pro'ya (hedef: VEGAS Pro 2026 / 26) eklenen gerçek bir **OpenFX (OFX) video efektidir**.
Resmin yüzeyini (alfa kanalındaki şekli ya da tüm kareyi) algılar, onu **üçgenlerden oluşan bir ağ (mesh)** ile
kaplar ve koyduğun **pinleri** hareket ettirdiğinde resmi bu ağ üzerinden, olabildiğince katı (as-rigid-as-possible)
biçimde büker. After Effects'teki Puppet Pin aracına benzer şekilde çalışır.

- Şekil algılama: alfa kanalı (dekupe karakter, logo, PNG…) veya tüm kare
- Üçgen mesh + ARAP çözücü (kollar/bacaklar esnemeden, doğal biçimde bükülür)
- 20 pin; her pinin konumu VEGAS'ta normal şekilde keyframe'lenebilir
- Önizlemede pin ekleme/sürükleme (VEGAS'ın OFX overlay desteğiyle) ve VEGAS'ın kendi crosshair noktaları
- 8-bit (BGRA) ve 32-bit float projeler, önizleme çözünürlükleri (Full/Half/Quarter), straight/premultiplied alfa
- Ek bağımlılık yok: tek bir `.ofx` dosyası, Visual C++ redistributable gerekmez

## Kurulum

1. `PuppetPin.ofx.bundle` klasörünü edinin:
   - GitHub Actions'ın ürettiği **PuppetPin-OFX-Win64** paketinden (Actions → son çalıştırma → Artifacts), ya da
   - kaynaktan derleyin (aşağıda *Derleme*).
2. Klasörü olduğu gibi şuraya kopyalayın (yönetici izni ister):

   `C:\Program Files\Common Files\OFX\Plugins\PuppetPin.ofx.bundle\Contents\Win64\PuppetPin.ofx`

   Bunu otomatik yapmak için **yönetici** PowerShell'de:

   ```powershell
   powershell -ExecutionPolicy Bypass -File install.ps1
   ```

   (VEGAS'ın kendi `...\OFX Video Plug-Ins` klasörüne kurmak isterseniz `-Destination "<klasör>"` verin;
   kaldırmak için `-Uninstall`.)
3. **VEGAS Pro'yu yeniden başlatın.** Efekt **Video FX → Distort → Puppet Pin** altında görünür.

## Kullanım

1. Efekti bir video event'ine (ör. alfa kanallı bir PNG karaktere) uygulayın.
   **Setup Mode** varsayılan olarak açıktır: resim bükülmez, algılanan yüzey ve üçgen ağ resmin üzerine çizilir.
2. Pinleri yerleştirin – Setup Mode açıkken pin nereye giderse resme oraya yapışır:
   - **Önizlemede tıklayın**: ilk boş pin tıkladığınız yere eklenir; sürükleyerek yerini değiştirin.
   - veya **Pin N → Enabled** kutusunu işaretleyip **Position**'ı (önizlemedeki crosshair ya da X/Y değerleri) ayarlayın.
   Pinleri eklem gibi düşünün: baş, gövde, dirsek, el, diz, ayak…
3. **Setup Mode'u kapatın.** Artık bir pini sürüklediğinizde resim bükülür; diğer pinler resmi yerinde tutar.
4. Animasyon için pinlerin **Position** parametresini VEGAS'ta normal şekilde keyframe'leyin
   (FX penceresindeki animasyon/keyframe düğmesi).

Önizleme overlay kısayolları: boş yere tıkla = pin ekle, pine tıkla-sürükle = taşı,
**Ctrl+tık** veya seçiliyken **Delete** = pini kaldır.

İpuçları:
- Pin, **Enabled** yapıldığı anda bulunduğu yere yapışır; bu yüzden pin eklemek resmi zıplatmaz.
- **Reset Position** pini kendi yapışma noktasına geri götürür; **Reset All Pins** tüm pinleri.
- Sonradan bir pinin resim üzerindeki yerini değiştirmek için Setup Mode'u tekrar açıp pini taşıyın.
- Kırmızı pin = ağın dışında kaldı (resmi etkilemiyor); onu şeklin üzerine taşıyın.
- Tek pinli bir parça olduğu gibi kayar; bükmek için en az iki pin gerekir.
- Pin içermeyen ayrı parçalar (ör. karakterden kopuk bir nesne) yerinde kalır.

## Parametreler

| Parametre | Açıklama |
| --- | --- |
| Setup Mode (place pins) | Açık: resim bükülmez, pin taşımak pini resme yeniden yapıştırır. Kapalı: pin taşımak resmi büker. |
| Reset All Pins | Etkin tüm pinleri yapışma noktalarına döndürür (o karede). |
| Mesh → Mesh Area | *Alpha (detect shape)*: sadece opak kısım ağlanır. *Whole frame*: tüm kare. |
| Mesh → Alpha Threshold | Bu değerden daha opak pikseller yüzeye dahil edilir. |
| Mesh → Mesh Density | Kare genişliği boyunca hücre sayısı (her hücre 2 üçgen). Yüksek = daha yumuşak bükülme, daha yavaş. |
| Mesh → Expansion | Algılanan şekli bu kadar piksel büyütür (yumuşak kenarlar ağın içinde kalsın diye). |
| Mesh → Rigidity | Çözücü iterasyonu. Yüksek = daha katı ve doğal bükülme, biraz daha yavaş. |
| Display → Show Mesh On Preview | Bükülmüş ağı önizleme overlay'inde gösterir (render'a girmez). |
| Display → Draw Mesh & Pins Into Video | Ağı ve numaralı pinleri görüntünün **içine** çizer. Overlay görünmüyorsa kullanın; final render'dan önce kapatın. |
| Display → Invert Pin Y Axis | Yalnızca bir host'ta crosshair'ler resme göre ters (yukarı-aşağı) görünürse. |
| Pin N → Enabled / Position / Reset Position | Pinin kendisi. Position 0..1 aralığında normalize kare koordinatıdır (keyframe'lenir). |

Not: Setup Mode açıkken ağ ve pinler her zaman görüntüye çizilir (hangi yüzeyin algılandığını görmeniz için).
Final render'dan önce Setup Mode'u kapatın.

## VEGAS scriptlerinden erişim

Parametre adları VEGAS Script API'deki `OFXEffect.FindParameterByName` ile kullanılabilir:
`setupMode`, `resetAllPins`, `meshArea`, `alphaThreshold`, `meshDensity`, `expansion`, `rigidity`,
`overlayMesh`, `burnOverlay`, `invertPinY`, ve her pin için `pin01_on`, `pin01_pos`, `pin01_anchor` (gizli
yapışma noktası), `pin01_reset` … `pin20_*`. `pinNN_pos` / `pinNN_anchor` birer `OFXDouble2DParameter`'dır.

## Sorun giderme

- **Efekt listede yok:** VEGAS'ı yeniden başlatın; dosyanın tam olarak
  `...\OFX\Plugins\PuppetPin.ofx.bundle\Contents\Win64\PuppetPin.ofx` yolunda olduğunu kontrol edin.
  Gerekirse VEGAS'ı **Ctrl+Shift** basılıyken açıp önbelleği temizleyin.
- **Güncelledim ama değişmedi:** Efekti event'ten kaldırıp yeniden ekleyin (VEGAS örnek ayarlarını saklar).
- **Önizlemede pinler/ağ görünmüyor:** FX penceresi açıkken overlay görünür. Görünmüyorsa
  *Display → Draw Mesh & Pins Into Video*'yu açın veya pinleri parametrelerden/crosshair'den ayarlayın.
- **Crosshair ile çizilen pin farklı yerde:** *Display → Invert Pin Y Axis*'i deneyin ve lütfen bildirin.
- **Yavaş:** *Mesh Density* veya *Rigidity*'yi düşürün; önizlemede Preview/Half kalite kullanın.

## Derleme

Gereken: Visual Studio 2019/2022 (veya Build Tools), "Desktop development with C++" iş yükü.

```bat
build.bat          :: derler + testleri çalıştırır
build.bat clean    :: build\ klasörünü silip baştan
```

Çıktı: `build\PuppetPin.ofx.bundle`. CMake de desteklenir:

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release
```

### Proje yapısı

```
src/PuppetCore.h    şekil algılama, üçgen mesh, ARAP çözücü, rasterizer (host'tan bağımsız, test edilebilir)
src/PuppetPin.cpp   OFX katmanı: parametreler, render, instanceChanged, önizleme overlay'i
tests/core_test.cpp çekirdek birim testleri
tests/mock_host.cpp VEGAS gibi davranan sahte OFX host: .ofx'i yükleyip tüm akışı test eder
third_party/openfx  OpenFX başlık dosyaları (BSD-3-Clause)
```

### Teknik notlar (VEGAS'a özgü)

- Doğrudan OpenFX C API'si kullanılır. Pin noktaları `kOfxParamDoubleTypeNormalisedXYAbsolute` tipindedir,
  böylece VEGAS her pin için önizlemede kendi crosshair'ini çizer.
- VEGAS 8-bit görüntüleri BGRA sırasıyla verebilir (`OfxImageEffectPropPixelOrder`). Bükme işlemi kanal
  sırasından bağımsızdır, overlay renkleri buna göre ayarlanır.
- Render ölçeği yalnızca render argümanlarından okunur (VEGAS görüntülere yazmaz).
- `createInstance` içinde yalnızca handle'lar alınır. VEGAS orada edit block'a izin vermez.
- Eksik host özellikleri hata sayılmaz; varsayılan değerle devam edilir.
- Sabit pin bağlama (anchor) gizli parametrelerde tutulur ve `instanceChanged` ile güncellenir.

Lisans: OpenFX başlıkları BSD-3-Clause (`third_party/openfx/LICENSE.md`).
