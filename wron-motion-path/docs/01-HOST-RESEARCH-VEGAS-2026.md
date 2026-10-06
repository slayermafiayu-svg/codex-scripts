# 01 — VEGAS Pro 2026 host araştırması

Kaynaklar ikincildir: resmî sürüm notu sayfaları bu ortamın ağ politikası nedeniyle doğrudan açılamadı ve bilgiler arama sonucu özetlerinden derlendi. Aşağıdakilerin hiçbiri gerçek bir VEGAS kurulumunda doğrulanmadı. Doğrulama planı için bkz. `03-HOST-VERIFICATION.md`.

## 1. VEGAS Pro 2026 hakkında bulunanlar

| Bulgu | Kaynak | Güven | Tasarıma etkisi |
| --- | --- | --- | --- |
| 27 Mart 2026'da Boris FX tarafından yayımlandı. Güncellemeler: Build 105, 143 ve 2026.0.3 Build 189 | [toolfarm](https://www.toolfarm.com/news/boris-fx-vegas-pro-2026/), [Boris FX sürüm notları](https://borisfx.com/release-notes/vegas-pro-2026-build-2026-0-3-189-release-notes/), [wkconquer](https://www.wkconquer.com/2026/07/boris-fx-vegas-pro-2026-0-0-143.html) | Orta | Hedef host |
| Windows 11 x64 gerekiyor | [Thomann ürün sayfası](https://www.thomann.co.uk/boris_fx_vegas_pro.htm) | Orta | Yalnızca Win64 bundle |
| "Improved OFX identifier hashing to avoid collisions (Glow vs Crop)" | Build 105/143 sürüm notu özeti ([Boris FX](https://borisfx.com/release-notes/vegas-pro-2026-build-2026-0-3-189-release-notes/)) | Orta | Benzersiz ve kalıcı bir plugin identifier gerekiyor (`params.h`: `kPluginIdentifier`) |
| "BCC crash fix: updated OFX string/custom parameter temp buffer handling to prevent use-after-free" | aynı | Orta | Yol verisi string parametrede. Host'un döndürdüğü işaretçi hemen kopyalanıyor ve saklanmıyor (`readPathData`) |
| "Updated OFX description properties to indicate unsafe cloning behavior" | aynı | Düşük (hangi özellik olduğu bilinmiyor) | Tüm durum parametrelerde tutulduğu için klonlama güvenli. Eklenti hiçbir şey bildirmiyor |
| "Improved OFX preset uplift compatibility from older versions" | aynı | Orta | — |
| OFX envelope noktaları sağ tıkla eklendiğinde artık varsayılan olarak **Linear** (eskiden Hold) | Build 105 özeti | Orta | Progress keyframe'leri varsayılan olarak sabit hız verir |
| Build 189: OFX envelope eğrilerinde çizim, seçim ve yapıştırma düzeltmeleri; OFX tarama sırasında başlangıç kararlılığı | Build 189 özeti | Orta | Progress zaman çizelgesinde envelope olarak düzenlenir |
| Crop OFX, Video Preview'da interact (ekran üstü tutamak) kullanıyor | [VEGAS 2026 kılavuzu – Crop OFX](https://cdn.borisfx.com/borisfx/Documentation/vegas/2026/en/content/topics/7-edit/crop-ofx.htm) | Orta | Host overlay çiziyor. Üçüncü taraf eklentiler için ayrıca doğrulanmalı |
| VEGAS 21 Build 187/300: "improvements to the OFX Interact Suite for third party video plugins" | Arama özeti ([moviestudiozen](https://www.moviestudiozen.com/free-tutorials/vegas-pro/vegas-pro-21-update-history)) | Orta | Üçüncü taraf overlay desteği var gibi görünüyor |
| VEGAS 15 Update 2: PiP ve Crop OFX interact hizalaması; VEGAS 16 Update 5: OFX "manual" keyframe eğrisi yorumu düzeltmesi | [VEGAS forumu sürüm geçmişi](https://www.vegascreativesoftware.info/us/forum/vegas-pro-vegas-post-release-history--104998/?page=2) | Orta | — |
| Betik API'si `ScriptPortal.Vegas.dll`, .NET Framework 4.8; OFX parametreleri `FindParameterByName` ile erişilebilir | [unpkg skill özeti](https://unpkg.com/major-ai-skills@2.3.0/skills/vegas-pro/SKILL.md), [forum](https://www.vegascreativesoftware.info/us/forum/scripting-changes-to-ofx-parameters--104734) | Orta | Main Panel köprüsü yol verisini `wmpPathData` string parametresinden okuyup yazabilir (doğrulanmalı) |
| Magix: "OFX API'de değişiklik yapmadık" (2019) | [forum](https://www.vegascreativesoftware.info/us/forum/what-is-vegas-ofx-host-name--106761) | Düşük | Host adı `com.sonycreativesoftware.vegas` olarak kalmış olabilir. Eklenti davranışı host adına **bağlı değil** |

## 2. VEGAS OFX uzantıları (açık OpenFX çatallarındaki `ofxSonyVegas.h`, 2010)

| Uzantı | Eklentide kullanımı |
| --- | --- |
| `OfxImageEffectPropPixelOrder` = RGBA/BGRA (görüntü özelliği) | Okunuyor. BGRA ise kanallar yer değiştiriyor (`ofx_util.cpp`). Testi var |
| `OfxImageEffectPropRenderQuality` = Draft/Preview/Good/Best (render girdisi) | Okunuyor; önizleme bulanıklık kalitesini seçiyor. **Dikkat:** aynı yazımdaki standart OFX 1.4 tamsayı özelliği `kOfxImageEffectPropRenderQualityDraft` ayrı okunuyor ve ikisi karıştırılmıyor |
| `OfxImageEffectPropVegasContext` (Media/Track/Event/…) | Tanı etiketine yazılıyor; davranışı değiştirmiyor |
| `OfxVegasKeyframeSuite` (keyframe interpolasyon tipi ve eğim okuma/yazma) | **Kullanılmıyor.** Zamanlama presetlerini OFX içinden uygulamak için aday; host'ta doğrulanmalı |
| `OfxHWndInteractSuite` (efekt penceresine Win32 alt pencere) | Kullanılmıyor. Main Panel varken gerek yok |
| BGR bit derinliği etiketleri | Desteklenmiş gibi bildirilmiyor; gelirse savunmacı olarak tanınıyor |

Başlık "All rights reserved" notu taşıdığı için depoya kopyalanmadı. Yalnızca kullanılan tanımlayıcılar `ofx/src/vegas_ext.h` içinde yeniden yazıldı. Wron'un elindeki resmî başlıkla değiştirilmeli.

## 3. Eski bir VEGAS host bildirimi (karşılaştırma için)

openfx-misc'in host listesinde (Miscz deposu, `README-hosts.txt`) **Sony Vegas 12** için şunlar bildiriliyor: OFX API 1.1, `supportsOverlays=0`, `supportsMultiResolution=0`, `supportsTiles=0`, `temporalClipAccess=1`, piksel derinlikleri Byte ve Float, `supportsStringAnimation=1`, `supportsCustomInteract=0`, suite'ler arasında `OfxVegasProgressSuite`, `OfxVegasStereoscopicImageEffectSuite`, `OfxVegasKeyframeSuite`, `OfxOpenCLProgramSuite`. Bu bilgi VEGAS 12'ye ait; 2026 için geçerli olduğu **varsayılmadı**. Eklenti bu yeteneklerin her birini çalışma anında okuyor.

## 4. Host'ta doğrulanması gereken açık sorular

| Soru | Neden önemli | Nasıl doğrulanır |
| --- | --- | --- |
| Üçüncü taraf eklentide `kOfxImageEffectPropSupportsOverlays` = 1 mi? V1 (OpenGL) mi, V2 (Draw Suite) mi çağrılıyor? | Önizlemede doğrudan yol düzenleme | Tanı etiketindeki "overlays" alanı; Video Preview'da yolun çizilip çizilmediği |
| `kOfxImageEffectPropFrameRange` event süresini veriyor mu, değerler event başına mı göreli? | "Fit to event" süresinin OFX'ten okunup okunamayacağı | Tanı etiketindeki "frame range" alanı, farklı uzunluktaki event'lerde |
| OFX zamanı event başlangıcına göre mi, proje zamanına göre mi? | Progress keyframe zamanları | Keyframe'leri event sonuna koyup event'i taşıyarak |
| Gizli (`secret`) string parametre projeye kaydediliyor ve event kopyalanınca kopyalanıyor mu? | Yolun kalıcılığı | Kaydet/aç ve event kopyalama adımları (`03`) |
| Önizleme kalitesi düşükken `renderScale` < 1 geliyor mu, yoksa yalnızca `RenderQuality` mı değişiyor? | Önizleme performansı | Preview/Draft modlarında tanı etiketi ve render süresi |
| Görüntüler premultiplied mı, straight mı? 8 bit projede piksel sırası BGRA mı? | Alpha ve kanal doğruluğu | Yarı saydam PNG kaynakla kenar testi |
| Interlaced projede alan bazlı render (`FieldToRender` Lower/Upper) geliyor mu? | Alan eşlemesi | Interlaced proje ve tanı amaçlı log |
| Host `paramSetValue` içinden `instanceChanged`'i eşzamanlı çağırıyor mu? | Kilitlenme riski | Eklenti bunu zaten tolere ediyor (mock host bu şekilde test ediyor) |
| Proje kapanırken `DestroyInstance` / `Unload` sırası | Kaynak temizliği | Eklenti kalıcı iş parçacığı tutmuyor; Process Explorer ile iş parçacığı ve tanıtıcı sayısı izlenir |
