# 03 — VEGAS Pro 2026 üzerinde doğrulama planı

Buradaki hiçbir madde bu depoda **doğrulanmadı**. Bu ortam Linux; VEGAS çalıştırılamıyor. Mock host testleri yalnızca OFX spesifikasyonuna uyumu gösterir.

## Hazırlık

1. Windows 11 x64 + VEGAS Pro 2026 (Build 189 veya güncel).
2. Geliştirici derlemesi: Visual Studio 2022 ile
   `cmake -S wron-motion-path -B build -G "Visual Studio 17 2022" -A x64` ve
   `cmake --build build --config Release`.
   CI'daki `WronMotionPath-DEV-build-not-for-distribution` artefaktı da kullanılabilir.
3. `build\WronMotionPath.ofx.bundle` klasörünü `C:\Program Files\Common Files\OFX\Plugins\` altına kopyalayın. **Yalnızca test makinesinde.** Bu derleme lisans kontrolü içermez ve dağıtılmamalıdır.
4. VEGAS'ı başlatın. Video FX listesinde "WronSVP › Wron Motion Path (DEV build - not for distribution)" görünmeli.
5. Her testten sonra efektin **Diagnostics › Host** satırını kaydedin. Host adını, sürümü, VEGAS bağlamını, frame range'i, fps'i, proje boyutunu, PAR'ı ve overlay desteğini gösterir.

## A. Yükleme ve temel davranış

| # | Adım | Beklenen | Not |
| --- | --- | --- | --- |
| A1 | Efekti bir video event'ine uygulayın | Görüntü, kare yüksekliğinin ortasındaki düz yolun sol ucuna (x = %20) taşınır. Path satırında "2 points, open, length …" yazar | Varsayılan Progress 0 |
| A2 | Progress'i 0 → 1 animasyonlayın (iki keyframe) | Görüntü soldan sağa sabit hızla gider; ortada kare merkezindedir | VEGAS 2026'da yeni envelope noktaları Linear olmalı |
| A3 | Herhangi bir zamana atlayın ve oynatmayı tekrarlayın | Aynı zamanda aynı görüntü (önceki kareye bağımlılık yok) | |
| A4 | Presets › Circle › Load Preset Path | Yol daireye döner; tek undo adımı (Ctrl+Z ile geri gelir) | Undo davranışını not edin |
| A5 | Reverse Path Direction | Daire aynı noktadan başlar, ters yönde döner | |
| A6 | Open Path Ends: Loop / Ping-Pong, Progress 0 → 3 | Loop: sonda başa atlar; Ping-Pong: git-gel; kapalı yolda seçenek devre dışı | |
| A7 | Easing: Ease In-Out, Back Out | Uçlarda yavaşlar; Back Out ile Extend seçiliyken uçtan dışarı taşar | |

## B. Kalıcılık ve örnekler

| # | Adım | Beklenen |
| --- | --- | --- |
| B1 | Yolu değiştirin, projeyi kaydedin, VEGAS'ı kapatıp açın | Yol ve tüm parametreler aynı; render aynı |
| B2 | Event'i kopyalayıp yapıştırın, kopyanın yolunu değiştirin | Orijinal event değişmez |
| B3 | Event'i çoğaltın (Ctrl+D veya sürükle-kopyala) | B2 ile aynı |
| B4 | Aynı event'e iki Wron Motion Path ekleyin | İki yol bağımsız |
| B5 | Efekti başka bir event'e preset olarak kaydedin ve yükleyin (VEGAS FX preset) | Yol presetle gelir. **Gizli string parametrenin preset'e girip girmediğini not edin** |
| B6 | Event'i bölün (S) | İki parça aynı yolu taşır; zaman eşleşmesini not edin |

## C. Görüntü ve alpha

| # | Adım | Beklenen |
| --- | --- | --- |
| C1 | Saydam PNG (yumuşak kenarlı) kaynak, rotation 30°, scale %50 | Kenarlarda siyah hale yok; kaynak alanı dışı saydam |
| C2 | 8 bit proje ve 32 bit floating point proje | İkisinde aynı konum; renk kanalları doğru (BGRA desteği) |
| C3 | Preview kalitesi Draft / Preview / Good / Best; Half / Quarter | Konum değişmez; Diagnostics'teki render scale ve kaliteyi not edin |
| C4 | PAR ≠ 1 proje (ör. 1440×1080, PAR 1.333) | Daire ekranda yuvarlak; 90° rotation izotropik |
| C5 | Interlaced proje (upper/lower field first), Progress hızlı | Alanlar karışmaz; hareket alan zamanlarına göre ilerler |
| C6 | 23,976 / 29,97 / 59,94 fps projeler | A2 ile aynı; keyframe'lerin kare olmayan zamanlarında sıçrama yok |

## D. Yönlenme ve hareket bulanıklığı

| # | Adım | Beklenen |
| --- | --- | --- |
| D1 | Orient to Path, Circle | Görüntü teğete döner; tur sonunda 180/−180 sıçraması görünmez |
| D2 | Zigzag + Orient, smoothing 0 / %5 | 0'da köşede anlık dönüş; %5'te yumuşak dönüş, kareden kareye titreme yok |
| D3 | Ping-Pong + Face: Travel / Path | Travel: dönüşte 180°; Path: hep yol yönü |
| D4 | Motion Blur açık, hızlı hareket | Yol boyunca iz; durağan karelerde render hızı bulanıklık kapalıyla aynı |
| D5 | Loop sınırındaki kare, blur açık | İki uçta kısmi kopyalar; ekranı boydan boya geçen iz yok |
| D6 | Max Samples 256, 4K proje | Render süresini ölçün (`05-STATUS.md` ile karşılaştırın) |

## E. Overlay (önizlemede doğrudan düzenleme)

| # | Adım | Beklenen | Kaydedin |
| --- | --- | --- | --- |
| E1 | Efekt penceresi açıkken Video Preview | Yol, noktalar ve kırmızı konum işareti çizilir | Diagnostics "overlays: yes/no" |
| E2 | Bir noktayı sürükleyin | Sürükleme sırasında yol çizgisi güncellenir, bırakınca **tek** undo adımı | VEGAS'ın V1 (OpenGL) mi V2 (Draw Suite) mi kullandığı |
| E3 | Preview Tool: Add Point → eğriye tıklayın | Eğri şekli bozulmadan nokta eklenir | |
| E4 | Delete tuşu (seçili nokta) | Nokta silinir. Boşluk, J/K/L gibi VEGAS kısayolları çalışmaya devam eder | Tuş olaylarının gelip gelmediği |
| E5 | Show Path in Preview kapalı | Overlay çizilmez, tıklamalar VEGAS'a gider | |

## F. Host yaşam döngüsü ve kaynaklar

| # | Adım | Beklenen |
| --- | --- | --- |
| F1 | Uzun timeline'ı birkaç kez oynatın, Process Explorer ile iş parçacığı ve bellek sayısını izleyin | Sürekli artış yok |
| F2 | Render sırasında iptal | Render hemen durur, VEGAS kararlı kalır |
| F3 | Projeyi kapatın / yeni proje açın / VEGAS'tan çıkın | Çökme yok; eklentiye ait iş parçacığı kalmaz |
| F4 | VEGAS'ı simge durumuna küçültüp geri getirin | Timeline ve önizleme kilitlenmez (eklenti UI'ı yalnızca overlay) |
| F5 | Bozuk yol: Main Panel olmadan test için betikle `wmpPathData`'ya geçersiz metin yazın | Render yolsuz devam eder; Path satırı "Invalid path data…" der; Reverse ve Preset veriyi ezmez |

## G. Main Panel ile (köprü yazıldıktan sonra)

`04-INTEGRATION.md` içindeki adaptör sözleşmesi uygulandıktan sonra:

| # | Adım | Beklenen |
| --- | --- | --- |
| G1 | Editörde nokta sürükleyin | VEGAS'a bırakınca bir yazma; önizleme güncellenir |
| G2 | VEGAS'ta Ctrl+Z | Editör yeniden yükler, yankı yazması yapmaz |
| G3 | Zamanlama preseti: 5 sn event'ten kaydedip 10 sn event'e Fit ile yükleyin | Keyframe'ler iki kat uzar; event dışı keyframe'ler göreli konumunu korur; curve tipleri korunur |
| G4 | Dar dock (≈320×180) | Tek satır araç çubuğu, kullanılabilir çalışma alanı, ayarlar çekmecesi |
| G5 | 8 dil | Metinler taşmadan görünür |
