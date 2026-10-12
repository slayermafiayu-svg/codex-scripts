# Puppet Pin – VEGAS Pro için OFX kukla (puppet) efekti

**Puppet Pin**, VEGAS Pro'ya (hedef: VEGAS Pro 2026 / 26) eklenen gerçek bir **OpenFX (OFX) video efektidir**.
Resmin yüzeyini (alfa kanalındaki şekli ya da tüm kareyi) algılar, onu **üçgenlerden oluşan bir ağ (mesh)** ile
kaplar ve koyduğun **pinleri** hareket ettirdiğinde resmi bu ağ üzerinden, olabildiğince katı (as-rigid-as-possible)
biçimde büker. After Effects'teki Puppet Pin aracına benzer şekilde çalışır.

- Şekil algılama: alfa kanalı (dekupe karakter, logo, PNG…) veya tüm kare
- Üçgen mesh + ARAP çözücü (kollar/bacaklar esnemeden, doğal biçimde bükülür)
- 20 pin; her pinin konumu VEGAS'ta normal şekilde keyframe'lenebilir
- İki bölüm: **1 - Mesh** (ağ görünür, pinler yerleştirilir) ve **2 - Animate** (ağ ve mesh ayarları gizlenir,
  pinler resmi büker)
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

Efekt iki bölümden oluşur; en üstteki **Mode** listesinden seçilir:

### 1 - Mesh (place pins)

1. Efekti bir video event'ine (ör. alfa kanallı bir PNG karaktere) uygulayın. Efekt bu modda açılır.
2. Resim bükülmez. Algılanan yüzey üçgen ağ olarak resmin üzerine çizilir. Yalnızca **Mesh Settings**
   (Mesh Area, Alpha Threshold, Mesh Density, Expansion) görünür; ağı buradan ayarlayın.
3. Pinleri yerleştirin. Bu modda pin nereye konursa resme oraya yapışır:
   - **Önizlemede tıklayın**: ilk boş pin tıkladığınız yere eklenir; sürükleyerek yerini değiştirin.
   - veya **Pin N → Enabled** kutusunu işaretleyip **Position**'ı (önizlemedeki crosshair ya da X/Y değerleri) ayarlayın.
   Pinleri eklem gibi düşünün: baş, gövde, dirsek, el, diz, ayak…

### 2 - Animate (bend)

1. **Mode → 2 - Animate (bend)** seçin.
2. Ağ ekrandan kalkar ve mesh ayarları gizlenir; yerine **Animate Settings** (Rigidity, Show Pins,
   Reset All Pins) ve pinlerin **Reset Position** düğmeleri gelir.
3. Bir pini sürükleyince (önizlemede ya da Position değeriyle) resim bükülür; diğer pinler resmi yerinde tutar.
4. Animasyon için pinlerin **Position** parametresini VEGAS'ta normal şekilde keyframe'leyin.

Animate modunda pinler önizlemede numaralarıyla görünür. **Final render'a girmezler**: *Show Pins = Auto*
iken pinler yalnızca önizleme penceresi **Draft** veya **Preview** kalitesindeyken çizilir. Önizlemeyi
Good/Best kalitede kullanıyorsanız pinleri VEGAS'ın overlay'i ve crosshair'leri gösterir.

Önizleme overlay kısayolları: boş yere tıkla = pin ekle, pine tıkla-sürükle = taşı,
**Ctrl+tık** veya seçiliyken **Delete** = pini kaldır.

İpuçları:
- Pin, **Enabled** yapıldığı anda bulunduğu yere yapışır; bu yüzden pin eklemek resmi zıplatmaz.
- **Reset Position** pini kendi yapışma noktasına geri götürür; **Reset All Pins** tüm pinleri.
- Bir pinin resim üzerindeki yerini sonradan değiştirmek için Mode'u **1 - Mesh**'e alıp pini taşıyın.
- Kırmızı pin = ağın dışında kaldı (resmi etkilemiyor); onu şeklin üzerine taşıyın.
- Tek pinli bir parça olduğu gibi kayar; bükmek için en az iki pin gerekir.
- Pin içermeyen ayrı parçalar (ör. karakterden kopuk bir nesne) yerinde kalır.

## Parametreler

| Parametre | Mod | Açıklama |
| --- | --- | --- |
| Mode | her ikisi | *1 - Mesh*: ağ görünür, pinler resme yerleştirilir. *2 - Animate*: ağ gizli, pinler resmi büker. |
| Mesh Settings → Mesh Area | Mesh | *Alpha (detect shape)*: sadece opak kısım ağlanır. *Whole frame*: tüm kare. |
| Mesh Settings → Alpha Threshold | Mesh | Bu değerden daha opak pikseller yüzeye dahil edilir. |
| Mesh Settings → Mesh Density | Mesh | Kare genişliği boyunca hücre sayısı (her hücre 2 üçgen). Yüksek = daha yumuşak bükülme, daha yavaş. |
| Mesh Settings → Expansion | Mesh | Algılanan şekli bu kadar piksel büyütür (yumuşak kenarlar ağın içinde kalsın diye). |
| Animate Settings → Rigidity | Animate | Çözücü iterasyonu. Yüksek = daha katı ve doğal bükülme, biraz daha yavaş. |
| Animate Settings → Show Pins | Animate | *Auto*: pinler yalnızca Draft/Preview kalitedeki önizlemede çizilir. *Always*: render'da da. *Never*: hiç. |
| Animate Settings → Show Mesh On Overlay | Animate | Bükülmüş ağı önizleme overlay'inde gösterir (varsayılan kapalı, render'a girmez). |
| Animate Settings → Reset All Pins | Animate | Etkin tüm pinleri yapışma noktalarına döndürür (o karede). |
| Display → Draw Mesh & Pins Into Video | her ikisi | Ağı ve pinleri her modda ve render'da görüntüye çizer (kontrol amaçlı; final render'dan önce kapatın). |
| Display → Invert Pin Y Axis | her ikisi | Yalnızca bir host'ta crosshair'ler resme göre ters (yukarı-aşağı) görünürse. |
| Pin N → Enabled / Position | her ikisi | Pinin kendisi. Position 0..1 aralığında normalize kare koordinatıdır (keyframe'lenir). |
| Pin N → Reset Position | Animate | Pini o karede yapışma noktasına döndürür. |

Not: Mesh modunda ağ ve pinler her zaman görüntüye çizilir (hangi yüzeyin algılandığını görmeniz için).
Final render'dan önce Mode'u **2 - Animate**'e alın.

## VEGAS scriptlerinden erişim

Parametre adları VEGAS Script API'deki `OFXEffect.FindParameterByName` ile kullanılabilir:
`mode` (0 = Mesh, 1 = Animate), `meshArea`, `alphaThreshold`, `meshDensity`, `expansion`, `rigidity`,
`animatePins`, `overlayMesh`, `resetAllPins`, `burnOverlay`, `invertPinY`, ve her pin için `pin01_on`, `pin01_pos`, `pin01_anchor` (gizli
yapışma noktası), `pin01_reset` … `pin20_*`. `pinNN_pos` / `pinNN_anchor` birer `OFXDouble2DParameter`'dır.

## Sorun giderme

- **Efekt listede yok:** VEGAS'ı yeniden başlatın; dosyanın tam olarak
  `...\OFX\Plugins\PuppetPin.ofx.bundle\Contents\Win64\PuppetPin.ofx` yolunda olduğunu kontrol edin.
  Gerekirse VEGAS'ı **Ctrl+Shift** basılıyken açıp önbelleği temizleyin.
- **Güncelledim ama değişmedi:** Efekti event'ten kaldırıp yeniden ekleyin (VEGAS örnek ayarlarını saklar).
- **Animate modunda pinler görünmüyor:** önizleme kalitesini *Preview* veya *Draft* yapın ya da
  *Animate Settings → Show Pins*'i *Always* yapın (render'dan önce *Auto*'ya döndürün). Overlay, FX penceresi
  açıkken görünür.
- **Mode değişince parametreler gizlenmiyor:** bazı VEGAS sürümleri listeyi hemen yenilemeyebilir; FX
  penceresini kapatıp açın. Gizlenmeseler bile o moda ait olmayan ayarlar etkisizdir.
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
- Mod değişince parametreler `kOfxParamPropSecret`/`kOfxParamPropEnabled` ile gizlenir/gösterilir.
- Animate modunda pinlerin yalnızca önizlemede çizilmesi için VEGAS'ın render argümanındaki
  `OfxImageEffectPropRenderQuality` (Draft/Preview = önizleme) kullanılır.

Lisans: OpenFX başlıkları BSD-3-Clause (`third_party/openfx/LICENSE.md`).
