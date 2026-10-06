# 04 — Wron ürün ailesine entegrasyon

Bu bölüm, eksik Wron kaynakları geldiğinde yapılacak bağlama işlerini tanımlar. Buradaki C# veya köprü kodu **yazılmadı**. Aşağıdaki eşleme, VEGAS betik API'sinin bilinen yüzeyine dayanan bir **öneridir** ve doğrulanmalıdır.

## 1. Main Panel köprüsü (editörün host adaptörü)

Editör yalnızca `editor/src/host/adapter.js` sözleşmesini kullanır:

| Adaptör metodu | Main Panel / VEGAS tarafında önerilen karşılık | Not |
| --- | --- | --- |
| `getContext()` → `{frame:{width,height,par}, fps:{num,den}, eventDuration, time}` | Proje `VideoSettings` (Width, Height, PixelAspectRatio, FrameRate); seçili event'in `Length`'i; imleç zamanı − event başlangıcı | `eventDuration` saniye. Bilinmiyorsa `null` verin; zamanlama presetleri o zaman devre dışı kalır |
| `readPathData()` / `writePathData(json, label)` | Seçili event'in efekt zincirinde Wron Motion Path'i bulun; `OFXEffect.FindParameterByName("wmpPathData")` → `OFXStringParameter.Value` | Yazmayı `UndoBlock(label)` içinde yapın. Editör sürükleme başına **bir** yazma yapar |
| `readParams(ids)` / `writeParam(id, value, label)` | `OFXDoubleParameter`, `OFXDouble2DParameter`, `OFXBooleanParameter`, `OFXChoiceParameter`, `OFXIntegerParameter` | Animasyonlu parametrelere statik yazmayın; editör bunları zaten salt okunur gösterir |
| `readKeyframes(ids)` / `writeKeyframes(id, keys, label)` | `OFXDoubleParameter.Keyframes` (Time, Value, Type: Linear/Fast/Slow/Smooth/Sharp/Hold/Manual/Split, eğimler) | Zamanlar event başlangıcına göre saniye. `interp` adları küçük harf |
| `listPresets/savePreset/loadPreset/deletePreset` | Wron'un mevcut kullanıcı veri klasörü (ör. `%APPDATA%\WronSVP\MotionPath\Presets\*.json`) | Biçim: `wron.motionpath.preset` v1 |
| `getFrameImage()` | İsteğe bağlı: imleçteki karenin küçük bir görüntüsü | Yoksa editör düz bir arka plan çizer |
| `subscribe(listener)` | VEGAS olayları: proje değişti, undo/redo, seçim değişti, event süresi değişti | `{type:'pathData'|'params'|'context'}` gönderin; editör kendi yazdığı veriyi tanır, yankı yazması yapmaz |

Ek kurallar:

- **Örnek kimliği:** Main Panel'in hangi event ve efekt örneğini düzenlediğini, seçim değişiminde güvenle değiştirebilmesi gerekir. Editörü yeniden oluşturun (`destroy()` → `createPathEditor`) ya da adaptörden `context` olayı gönderin.
- **Render ile UI ayrımı:** editör hiçbir zaman render'ı beklemez. Yazma işlemleri `async`'tir ve sıralanır (`writeChain`).
- **Odak:** WebView, VEGAS kısayollarını engellememeli. Editör yalnızca işlediği tuşlarda `preventDefault` çağırır; Boşluk tuşunu asla almaz. Köprü, işlenmeyen tuşları VEGAS'a iletmelidir (WebView2 accelerator ayarı).
- **Dil:** `createPathEditor(el, { locale })` en, tr, es, pt, fr, de, ru ve zh-Hans'ı kabul eder. Wron'un i18n sistemine taşırken `editor/src/view/strings.js` anahtarları korunmalıdır.

## 2. Wron Transform OFX ile hizalama listesi

| Konu | Bu çalışmadaki değer | Yapılacak |
| --- | --- | --- |
| Plugin identifier | `com.wronsvp.ofx.MotionPath` (geçici) | Wron'un ad alanıyla hizalayın; ilk sürümden sonra **asla** değiştirmeyin |
| Grouping | `WronSVP` | Transform'un kullandığı grupla aynı yapın |
| Parametre kimlikleri | `wmp*` önekli, `params.h` | Transform'un adlandırma kuralına göre karar verin, sonra dondurun |
| Transform sırası / rotation yönü / pivot birimi | `02-CONTRACTS.md` §2 | Transform'unkiyle karşılaştırın; gerekiyorsa `poseToCanonical`'da değiştirin |
| Koordinat birimi | Normalize, Y aşağı | Transform piksel veya Y yukarı kullanıyorsa kullanıcı arayüzünde aynısını gösterin |
| Motion blur | Kendi planlayıcısı | Transform'un altyapısı varsa `planFrame` → örnek zamanları ve pozlar ona verilebilir |
| GPU | Yok | Transform'un GPU yolu (hangi API, hangi OFX uzantısı) incelenip aynı yöntemle eklenmeli. CPU yolu referans olarak kalmalı |
| OFX etiketlerinin dili | İngilizce | Transform'un OFX etiketlerini yerelleştirip yerelleştirmediğine bakın |

## 3. Kurulum

- Bundle yapısı: `WronMotionPath.ofx.bundle\Contents\Win64\WronMotionPath.ofx` (CMake bunu doğrudan üretir; Visual Studio'nun `Release\` alt klasörü engellenir).
- Hedef: `C:\Program Files\Common Files\OFX\Plugins\`. Bu yol VEGAS sürümünden bağımsızdır; tüm OFX host'ları paylaşır.
- Bağımlılıklar: statik CRT. DLL bağımlılıkları yalnızca `KERNEL32` ve `OPENGL32` (MinGW kontrolünde ayrıca `msvcrt`). CI, MSVC derlemesinde `dumpbin /dependents` çıktısını gösterir.
- **VEGAS sürüm seçimi korunmalı.** Mevcut Wron kurulum betiği VEGAS sürümüne göre farklı dosyalar kuruyorsa, Motion Path bundle'ı da aynı koşul altına eklenmeli. Aynı ikili dosyanın bütün sürümlerde çalıştığı **varsayılmadı**. Eklenti Sony Vegas 12 ile 2026 arasındaki farkları (overlay, tiles, multi-resolution) çalışma anında okur, ancak eski sürümlerde test edilmedi.
- Main Panel ve Transform OFX kurulumlarına dokunulmadı.

## 4. Lisans

- Lisans kontrolü **yok** ve taklit de edilmedi. `CMakeLists.txt` müşteri derlemesini (`-DWMP_DEV_BUILD=OFF`) bilerek reddeder. Eklenti etiketi "DEV build - not for distribution" der. `plugin.cpp`, `WMP_DEV_BUILD` tanımlı değilse derlenmez.
- Entegrasyon: Transform OFX'in lisans modülünü nasıl çağırdığı görüldükten sonra aynı çağrı **Load/CreateInstance/Render** akışında aynı şekilde yapılmalı (lisanssız davranış: Transform'unki ne ise).
- CI artefaktının adı `WronMotionPath-DEV-build-not-for-distribution`; 7 gün saklanır.
