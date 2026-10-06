# Vegas 3D — Aşama 0: Depo İncelemesi ve Kaynak İhtiyacı

Tarih: 2026-10-06
Dal: `claude/magical-dirac-zmgccx`
Durum: **DURDURULDU — ürün kaynak kodu bu depoda yok.**

## 1. Ne incelendi

| Yer | İçerik | Sonuç |
| --- | --- | --- |
| `slayermafiayu-svg/codex-scripts` (`main` ve bu dal) | Tek dosya: `README.md` (`# codex-scripts`) | Vegas 3D kodu yok |
| `slayermafiayu-svg/slayermafiayu-svg.github.io` (erişilebilen tek diğer depo) | `index.html` + `icon-512.png` ("Sky Stack" oyun tanıtım sayfası) | Vegas 3D ile ilgisiz |

Git geçmişi tek commit ("Initial commit"). Başka dal, tag veya alt modül yok.

Brifteki talimat gereği ("kaynak dosyalar bu depoda yoksa durup hangi dosyaların
gerektiğini söyle; bağımsız bir demo oluşturma") ürün kodu **yazılmadı**, demo veya
mockup **üretilmedi**. Bu dosya yalnızca devamlılık kaydıdır (brif §27).

## 2. Aşama 1'e başlamak için gereken kaynaklar

Aşağıdakiler mevcut ürünün gerçek dosyalarıdır; adlar tahmindir, gerçek adlar
farklı olabilir. İlgili klasörleri olduğu gibi (git geçmişiyle birlikte tercih
edilir) bu depoya ya da erişim verilen ayrı bir depoya ekleyin.

### A. Arayüz (web UI) — P1 işinin ana hedefi
- UI proje kökü: `package.json`, lock dosyası, bundler/TS ayarları (`vite.config.*`,
  `tsconfig.json` vb.), varsa `.eslintrc`/`prettier` ayarları.
- Panel bileşenleri: workspace/dock düzeni, hiyerarşi, inspector, viewport, araç çubuğu.
- **Timeline + keyframe + graph editor** kaynakları (zaman↔kare dönüşümü, seçim,
  sürükleme, zoom/kaydırma, eğri düzenleme, undo/redo).
- Çeviri/i18n kaynakları (TR/EN sözlükleri ve kullanım biçimi).
- Mevcut stil sistemi (tema değişkenleri, ikon seti) ve varsa tasarım dokümanı.
- UI test altyapısı (varsa): birim testleri, e2e ayarları.

### B. Sahne verisi ve motor
- Scene document şeması (nesne, materyal, kamera, ışık, hiyerarşi, keyframe/curve).
- Kayıt/yükleme ve sürüm göçü kodu; **eski formatta en az 2 gerçek örnek sahne**
  (biri animasyonlu, biri parent zincirli) — regresyon testi için.
- Preset biçimi ve birkaç fabrika preseti.
- Animasyon evaluation kodu (interpolasyon, Bézier, retiming).

### C. VEGAS bağlantısı
- VEGAS uzantısı kaynakları (C#/.NET proje dosyaları `*.csproj`, `*.sln`),
  WebView/host köprüsü ve mesaj protokolü.
- Event ekleme/güncelleme/tekrar açma akışı, host sürüm tespiti (VEGAS Pro 14–22
  uyumluluk matrisi hangi dosyada tutuluyorsa).
- Lisans kontrolü kodu (anahtarlar ve sırlar **olmadan**; yalnızca akış).

### D. Native OFX renderer
- C/C++ kaynakları, `CMakeLists.txt`/`.vcxproj`, OFX SDK bağımlılık bilgisi.
- Zaman birimi dönüşümü (OFX time ↔ saniye ↔ kare), alpha/pixel format sözleşmesi,
  render snapshot ve cache katmanı.
- Varsa mevcut native testler veya referans render görüntüleri.

### E. Derleme, kurulum ve belgeler
- Kurulum betikleri (hedef VEGAS sürümlerine kopyalama), CI yapılandırması.
- Mevcut README/geliştirici notları, bilinen hata listesi.
- Hangi parçaların Windows'a özgü araç gerektirdiğinin kısa listesi.

Hepsinin tek seferde gelmesi şart değil. **A + B** gelirse Aşama 1 (yeni arayüz,
temel sahne akışı, timeline doğruluğu) başlayabilir; **C + D** host doğrulaması ve
renderer kontrolleri için gerekir.

## 3. Bulut ortamı sınırları (şimdiden bilinmesi gerekenler)
- Ortam Linux'tur; VEGAS Pro, Windows SDK veya MSBuild yok. C#/OFX derlemesi ve
  "VEGAS'ta test edildi" doğrulaması burada yapılamaz; yerel doğrulama adımları
  hazırlanır, gerçek host testi kullanıcı makinesinde yapılır.
- Web UI, saf hesaplama (zaman/kare, interpolasyon, retiming) ve mock host testleri
  burada çalıştırılabilir (Node, Chromium/Playwright mevcut).

## 4. Kaynaklar geldiğinde Aşama 1 planı (kısa)
1. Mevcut durum haritası: çalışan özellikler, doğrulanmış sorunlar, eksikler (kod üzerinden).
2. Timeline matematiği: FPS pay/payda, `[S, E)` sınırı, kare snapping, çakışma
   politikası, ana keyframe korunumu — tekrarlanabilir testlerle.
3. Yeni workspace: viewport öncelikli, dar panelde kullanılabilir dock düzeni,
   gerçek bileşenlere bağlı tasarım sistemi; animasyonlar kısa ve kesilebilir.
4. Temel akış: nesne oluştur → seç → düzenle → animasyon → kaydet → tekrar aç
   (mock host ile), lisans ve eski kayıt uyumluluğu korunarak.
5. Rapor: değişiklikler, çalıştırılan kontroller, kalan sorunlar, sonraki aşama.
