# 00 — Depo durumu ve eksik Wron kaynakları

Tarih: 2026-10-06 · Dal: `claude/tender-albattani-zvq3yq`

## 1. İncelenen yerler

| Yer | İçerik | Wron kodu var mı? |
| --- | --- | --- |
| `slayermafiayu-svg/codex-scripts` `main` | Yalnızca `README.md` (`# codex-scripts`), tek commit | Yok |
| Aynı depo, `claude/magical-dirac-zmgccx` dalı (açık PR #1) | `docs/vegas3d/00-INTAKE.md` (Vegas 3D için kaynak talebi) ve `packages/graph-editor/` (önceki oturumda yazılmış bağımsız keyframe graph editörü) | Yok. Graph editörü **Wron graph sistemi değildir** ve birleştirilmemiştir |
| `slayermafiayu-svg/slayermafiayu-svg.github.io` | `index.html`, `icon-512.png` (oyun tanıtım sayfası) | Yok |

Bu oturumun erişebildiği depolar bunlarla sınırlı (`list_repos` iki depo döndürüyor).

## 2. Brifte adı geçen ama bulunamayan parçalar

| Parça | Durum | Bu çalışmada nasıl ele alındı |
| --- | --- | --- |
| Wron Transform OFX (kaynak, parametre adları, transform sırası, rotation yönü) | **Yok** | Transform sırası, rotation yönü ve koordinat kuralı açıkça tanımlandı (`docs/02-CONTRACTS.md`). **Wron Transform'a göre doğrulanması gerekiyor.** |
| Wron Main Panel (web arayüzü, mevcut 8 dil sistemi, Wron graph sistemi) | **Yok** | Yol editörü, çerçeveden bağımsız ve Main Panel'e gömülebilir bir bileşen olarak yazıldı (`editor/`). Kendi 8 dillik tablosu var. Entegrasyon yapılmadı. |
| WebView köprüsü / VEGAS extension (C#) | **Yok** | Editörün host ile konuştuğu sözleşme tanımlandı (`editor/src/host/adapter.js`). Köprü tarafı **yazılmadı**; testler sahte adaptörle (`MemoryHostAdapter`) yapıldı. |
| Native renderer / Wron motion blur altyapısı | **Yok** | Bağımsız CPU renderer ve hareket bulanıklığı yazıldı (`core/`). Paylaşım için mevcut altyapının görülmesi gerekiyor. |
| GPU motorunun yetenekleri | **Yok** | GPU yolu **yazılmadı**; eklenti yalnızca CPU ile render ediyor ve GPU desteği bildirmiyor. |
| Kurulum sistemi (VEGAS sürüm seçimi dahil) | **Yok** | Kurulum değiştirilmedi. Yalnızca OFX bundle yapısı üretiliyor (`docs/04-INTEGRATION.md`). |
| Lisans doğrulama altyapısı | **Yok** | Lisans **kaldırılmadı, atlanmadı, taklit edilmedi**. Yalnızca geliştirici derlemesi var; müşteri derlemesi CMake'te bilerek hata veriyor. |

## 3. Çalışmaya devam etmek için gereken dosyalar

Adlar tahmindir; gerçek adlar farklı olabilir. Klasörleri olduğu gibi, mümkünse git geçmişiyle eklemeniz en iyisi.

### A. Wron Transform OFX (en yüksek öncelik)
- Proje dosyaları: `CMakeLists.txt` veya `*.vcxproj`/`*.sln`, derleyici ayarları, CRT seçimi (/MT veya /MD), çıktı bundle yapısı.
- Eklenti kaynağı: describe/describeInContext (parametre adları, gruplar, etiketler, plugin identifier ve grouping), render, RoD/RoI, overlay (varsa).
- Transform matematiği: transform sırası, rotation yönü, anchor/pivot tanımı, koordinat birimleri (normalize mi piksel mi, Y yönü), PAR ve render scale işleme.
- Motion blur ve GPU kodu (OpenCL/CUDA/D3D, hangi OFX GPU uzantısı), CPU fallback.
- Kullanılan OFX SDK başlıkları ve **VEGAS OFX uzantı başlığının** sürümü (`ofxSonyVegas.h` veya eşdeğeri).
- Varsa testler veya referans render görüntüleri.

### B. Wron Main Panel
- Web projesi: `package.json`, lock dosyası, bundler ayarları, kullanılan UI çerçevesi.
- Mevcut **8 dil sistemi**: sözlük dosyaları, anahtar biçimi, dil seçimi.
- **Wron graph sistemi** (easing/keyframe editörü) ve veri formatı.
- Köprünün JS tarafı: mesaj biçimi, istek/yanıt, olay aboneliği, hata kodları.
- Tema ve tasarım tokenları.

### C. VEGAS extension / WebView köprüsü (C#)
- `*.csproj`, hedef .NET Framework sürümü (VEGAS 2026 betik ana makinesi .NET Framework 4.8 kullanıyor), `ScriptPortal.Vegas.dll` referansı.
- WebView2 barındırma kodu ve mesaj protokolünün C# tarafı.
- Seçili event'i ve OFX efektini bulma; `OFXStringParameter`, `OFXDoubleParameter` okuma/yazma; keyframe ve curve tipi erişimi; event süresi; undo bloklarının kullanımı.
- Proje kapanma ve VEGAS'ın arka plandan dönme olaylarının işlenmesi.

### D. Kurulum ve lisans
- Kurulum betiği/projesi (Inno Setup, WiX, NSIS…), VEGAS sürüm seçimi mantığı, dosya listeleri, imzalama adımı.
- Lisans modülünün API'si ve Transform OFX'in onu nasıl çağırdığı. **Anahtar veya sır gerekmiyor**, yalnızca çağrı akışı.
- Release ve CI betikleri; geliştirici ve müşteri derlemesini nasıl ayırdığınız.

## 4. Kaynaklar gelince paylaşılabilecekler

- **Çekirdek** (`core/`): bağımlılıksız C++17 (yol modeli, belge, yay uzunluğu, zamanlama, poz, CPU render, motion blur planı, presetler, süre uyarlaması). Wron Transform'un projesine kitaplık olarak eklenebilir.
- **Motion blur**: Wron'un mevcut bulanıklık altyapısı daha uygunsa, `planFrame` yalnızca örnek zamanlarını ve pozları üretir. Örnekleri hangi renderer'ın biriktireceği değiştirilebilir.
- **Editör**: `createPathEditor(container, { host })`. Main Panel'in çerçevesine sarılabilir; dil tablolarının anahtarları korunarak Wron'un i18n sistemine taşınabilir.
- **OFX katmanı** (`ofx/src/`): Wron Transform'un yapısına (yardımcı sınıflar, lisans çağrısı, GPU yolu) uyarlanmalı. Parametre kimlikleri ilk sürümden önce dondurulmalı.
