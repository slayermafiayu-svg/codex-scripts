# 02 — Davranış sözleşmeleri

Bu belge eklentinin ve editörün **tanımlı** davranışını anlatır. Wron Transform OFX ile hizalanması gereken noktalar **[Wron ile doğrula]** etiketiyle işaretlidir.

## 1. Koordinatlar

| Uzay | Tanım | Kullanım |
| --- | --- | --- |
| **N** (normalize) | (0,0) karenin sol üstü, (1,1) sağ altı, (0.5,0.5) merkez. **+X sağa, +Y aşağı.** [0,1] dışındaki değerlere izin var (±10⁴) | Kullanıcıya görünen tüm noktalar: yol noktaları, kollar, pivot, ofset |
| **D** (görüntüleme) | x_D = u · en-boy oranı, y_D = v. Birimi kare yüksekliği; izotropik | Yay uzunluğu, sabit hız, açılar, kol eşdoğrusallığı |
| **C** (OFX canonical) | Host'un koordinatı: Y yukarı, orijin sol alt, PAR dahil | Eklenti ile host arası |
| **P** (piksel) | P = (C.x · renderScale.x / PAR, C.y · renderScale.y) | Render |

- En-boy oranı = proje genişliği × PAR / proje yüksekliği. Değerler OFX `ProjectSize`/`ProjectOffset`/`ProjectPixelAspectRatio` özelliklerinden okunur; host bunları vermezse kaynak RoD kullanılır.
- Çözünürlük, render scale veya PAR değişse de yol aynı göreli yerde kalır: (0.2, 0.5) her zaman soldan %20, dikeyde ortadır. Mock host testleri bunu doğrular: render scale 1/0.5/0.25, PAR 1/2/0.9.
- En-boy oranı **değişirse** (ör. 16:9'dan 9:16'ya), normalize noktalar yerinde kalır; daire şeklindeki bir yol elipse döner. Bu bilinçli bir tercih: kenar ve merkez ilişkileri korunur. Presetler oluşturuldukları andaki en-boy oranına göre yuvarlak kurulur. **[Wron ile doğrula]**

## 2. Transform sırası ve rotation yönü

```
çıktı = P + R(θ) · S(sx, sy) · (kaynak − pivot)          (D uzayında)
P     = yol noktası(progress) + konum ofseti
θ     = Rotation + (Orient to Path ? yol açısı + Rotation Offset : 0)
```

1. Kaynak pivotu orijine taşınır.
2. Kaynağın kendi eksenlerinde X/Y ölçek uygulanır.
3. Döndürülür.
4. Pivot, yol noktası + ofset konumuna yerleştirilir.

- **Pozitif rotation, ekranda saat yönündedir** (Y aşağı olduğu için +X ekseni +Y'ye döner).
- Yol konumu, kaynak pivotu ve konum ofseti üç ayrı girdidir.
- Kullanılabilir bir yol yoksa P = pivot olur; nötr ayarlarda görüntü değişmez ve host'a "identity" bildirilir.
- **[Wron ile doğrula]** Wron Transform'un sırası, rotation yönü ve pivot birimi. Farklıysa tek yerden değişir: `core/src/motion.cpp: poseToCanonical`.

## 3. Alpha ve kenarlar

- Kaynak her render'da premultiplied float RGBA dokuya çevrilir. Straight girdi premultiply edilir, opaque girdinin alpha'sı 1 kabul edilir.
- Tüm filtreleme ve motion blur toplaması premultiplied değerler üzerinde yapılır. Saydam pikseller renk katkısı yapmaz, kenarda siyah hale oluşmaz (testi: `subpixel_edges_have_no_dark_halo`, `ofx_bgra_byte_straight_alpha`).
- Kaynak bölgesinin dışı saydam siyahtır (0,0,0,0); kenar pikselleri uzatılmaz.
- Çıktı, çıktı görüntüsünün bildirdiği biçimde yazılır: premultiplied veya straight; 8/16/32 bit; RGBA veya BGRA (VEGAS uzantısı).
- Clip tercihlerinde opaque girdi premultiplied çıktı olarak bildirilir, çünkü hareket saydam alan açar.
- Küçültmede mip seviyesi kısa eksene göre seçilir; uzun eksen boyunca en çok 8 örnek alınır (basit anizotropik filtre). Yalnızca tek eksende küçültmede (ör. Y %25) diğer eksen keskin kalır. Alan render'ında iki alan karıştırılmaz.

## 4. Progress ve zamanlama

```
e = lapEase(Progress)          // her tam tur için ayrı easing: floor(p) + E(frac(p))
q = e + Start Offset
q = Reverse Travel ? 1 − q : q
u = uç davranışına göre (aşağıda)
mesafe = u · L                 // Constant Speed (yay uzunluğu)
       | segment başına eşit pay // Equal Time per Segment
```

| Uç davranışı (açık yol) | Davranış |
| --- | --- |
| Clamp | 0 ve 1'de durur |
| Extend | Uç teğeti boyunca düz devam eder (aşma yapan easing için) |
| Loop | Sona varınca başa atlar. **0'dan büyük tam sayılar sondadır:** Progress 1 sonda, 1+ε başta |
| Ping-Pong | Kesintisiz git-gel |
| Kapalı yol | Her zaman kesintisiz döngü; uç davranışı yok sayılır (arayüzde devre dışı) |

- Tüm hesaplama, o andaki parametre değerlerinin saf bir fonksiyonudur. Önceki karelere bağlı durum yoktur; herhangi bir zamana atlamak aynı sonucu verir (`progress_is_deterministic_for_random_access`).
- Bézier parametresi mesafe olarak **kullanılmaz**. Yay uzunluğu, uyarlamalı 5 noktalı Gauss–Legendre ile hata sınırlı hesaplanır (varsayılan 10⁻⁷ kare yüksekliği). Ters çevirme, korumalı Newton/bisection ile yapılır. Arc-length tabloları yol verisi ve en-boy oranına bağlı **anlık görüntülerde** tutulur; yalnızca bunlar değişince yeniden kurulur.
- Easing presetleri CSS cubic-bezier eğrileridir (x1, x2 ∈ [0,1], y serbest). "Back Out" ve özel eğriler 1'i aşabilir; aşma davranışı uç davranışına göre tanımlıdır.
- Keyframe'ler host'ta kalır. Eklenti ara keyframe yazmaz; ara değerler OFX içinde `paramGetValueAtTime` ile hesaplanır.

## 5. Yola göre yönlenme

- Yol açısı = atan2(teğet.y, teğet.x), D uzayında (Y aşağı, saat yönü pozitif).
- **Face = Travel Direction:** hareket ters dönünce (Reverse Travel, ping-pong dönüşü) görüntü 180° döner. Dönüş anı tam dönüm noktasıdır; dönüm karesinde ileri yön kullanılır.
- **Face = Path Direction:** her zaman yolun kendi ileri yönüne bakar.
- **Keskin köşe:** yumuşatma kapalıyken yön tam köşe noktasında, hareketin gittiği segmentin yönüne geçer. Bu anlık bir geçiştir; yuvarlama hatasına karşı köşe toleransı 10⁻¹² · L'dir.
- **Sıfır teğet** (kolları çekilmiş köşeler, sıfır uzunluklu segmentler, üst üste noktalar): sırayla B', ±B'', B''' ve kiriş denenir. Segment içinde tanımsızsa hareket yönündeki komşu segmentin teğeti alınır. Hiçbiri yoksa (tek nokta) açı 0'dır.
- **Yön yumuşatma (%):** yol üzerinde ±σ penceresinde üçgen ağırlıklı ortalama teğettir. Teğet konumun türevi olduğu için bu, sağ ve sol yarı pencerelerin ortalama konum farkına eşittir. Yalnızca konumlardan hesaplanır, mesafeyle süreklidir ve **önceki karelere bağlı değildir**. Köşeleri 2σ mesafeye yayar.
- ±180° sınırı: açılar yalnızca dönüşüm matrisine girer; hiçbir yerde açı interpolasyonu yapılmaz (motion blur her örnek için matrisi yeniden kurar). Çember üzerinde ardışık matrislerin sürekli olduğu test ediliyor.

## 6. Hareket bulanıklığı

- Deklanşör aralığı (kare cinsinden): `[t + phase/360, t + (phase + angle)/360]`. 180/−90 ortalanmış yarım karedir.
- N örnek, N eşit alt aralığın merkezlerindedir. Her örnekte **tüm** animasyonlu parametreler o alt-kare zamanında yeniden okunur (Progress, easing, rotation, scale, opacity). Örnekler premultiplied olarak ortalanır.
- **Uyarlamalı mod:** kaynak köşelerinin ekranda (çıktı pikseli) kat ettiği yol 16 prob aralığıyla ölçülür. N = ⌈mesafe / aralık⌉, [2, Max Samples] ile sınırlanır. Aralık Best için 0,75 px, Good için 1, Preview için 2, Draft için 4 pikseldir. 0,05 pikselden az hareket tek örnekle render edilir; sabit modda da durağan görüntüye boşuna örnek harcanmaz.
- **Süreksizlik:** loop atlaması ve yön dönüşü ölçülmez. Ölçüm, süreklilik anahtarı farklı olan prob aralığını ikiye bölerek iki yandaki gerçek hareketi bulur. Örnekler iki uçta gerçek konumlarında render edilir; iki uç arasında iz çizilmez (`loop_wrap_inside_shutter_draws_no_streak`).
- **Önizleme:** host Draft/Preview kalitesi bildirirse veya render scale < 1 ise "Preview Blur" ayarı uygulanır: Full, Reduced (sabit örnekte ¼, uyarlamalıda daha geniş aralık) veya Off.
- Üst sınır 256'dır. Maliyeti `05-STATUS.md` içinde ölçüldü. Kalite yalnızca örnek sayısına bırakılmadı: premultiplied birikim, mip ve anizotropik filtre, ölçülen piksel hareketine göre örnek seçimi ve süreksizlik farkındalığı var.

## 7. Veri formatı ve kalıcılık

- OFX parametresi `wmpPathData`: string, gizli, animasyonsuz, kalıcı. **Örnek başına bir belge** tutar; global durum yoktur.
- Biçim v1:

```json
{ "format": "wron.motionpath", "version": 1,
  "path": { "closed": false, "points": [
    { "id": "p1", "p": [0.2, 0.5], "in": [0, 0], "out": [0.1, -0.05], "mode": "smooth" } ] },
  "editor": { "grid": true, "snap": false, "units": "norm" } }
```

- Kollar noktaya göredir. `mode` (corner/smooth/free) yalnızca bir düzenleme kuralıdır; geometri her zaman kayıtlı kol vektörlerinden hesaplanır.
- **Bilinmeyen alanlar korunur:** üst düzey ve nokta düzeyinde. Daha yeni bir editörün yazdığı veri, eklentinin yeniden yazmasıyla (preset/ters çevirme) kaybolmaz.
- **Bozuk veri:** render yol olmadan yapılır (nötr ayarlarda görüntü değişmez). Durum etiketi sebebi gösterir. Veri **değiştirilmez**; düğmeler ve editör üzerine yazmayı reddeder.
- **Gelecek sürüm** (version > 1): okunmaz ve üzerine yazılmaz.
- **Geçiş:** zincirleme `Migration` tablosu (`from` → `from+1`) var. v1 ilk sürüm olduğu için üretim tablosu boş; mekanizma test tablosuyla doğrulanıyor. Geçiş yalnızca bellekte yapılır; veri, kullanıcının bir sonraki düzenlemesinde yeni sürümle yazılır.
- Sınırlar: 8 MB, 4096 nokta, iç içe derinlik 64, koordinat ±10⁴. JSON okuma ve yazma yerel ayardan bağımsızdır; Türkçe yerel ayarda bile ondalık ayıracı nokta kalır.
- **İş parçacığı modeli:** her örneğin `GeometryCache`'i var. Render iş parçacıkları değişmez (`shared_ptr<const>`) anlık görüntüler paylaşır. String parametre, örnek başına bir kilit altında **hemen kopyalanır**; host'a yazarken kilit tutulmaz (host `paramSetValue` içinden `instanceChanged` çağırabilir).

## 8. Presetler ve süre uyarlaması

- Hazır yollar: Straight, Arc, Circle, Ellipse, S-curve, Zigzag, Spiral. Hepsi düzenlenebilir nokta olarak açılır. OFX'te "Load Preset Path" düğmesiyle (tek undo adımı), editörde ise önizleme kartlarıyla yüklenir.
- Kullanıcı presetleri (editör, host depolaması gerekir) üç seviyede kaydedilir:
  - `geometry`: yalnızca yol.
  - `look`: yol + transform, yönlenme ve bulanıklık değerleri.
  - `timing`: yol + görünüm + zamanlama ayarları + animasyonlu parametrelerin keyframe'leri ve **kaynak event süresi**.
- **Fit to event:** `t' = t · hedef / kaynak`. Event dışındaki keyframe'ler göreli konumlarını korur ve içeri sıkıştırılmaz. Manuel eğimler aynı oranla bölünür.
- **Preserve timing:** zamanlar aynen kalır.
- Her uygulama **kaynak zamanlamadan** yeniden hesaplanır; tekrar tekrar uygulamak sürüklenme üretmez (500 tur testi). Kullanıcı arada keyframe'leri düzenlediyse yeni kaynak, mevcut keyframe'ler ve süre olur (`reapplyTiming`).
- Görünüm değerleri animasyonlu parametrelere statik olarak yazılmaz; atlanır ve kullanıcıya bildirilir.
- Event süresini OFX'ten okumak **varsayılmadı**. Süreyi host adaptörü (Main Panel köprüsü) sağlamalı; sağlamıyorsa zamanlama seviyesi devre dışıdır ve nedeni gösterilir.

## 9. Parametreler (dondurulacak kimlikler)

`ofx/src/params.h` dosyasına bakın. Kimlikler ve seçim değerlerinin sırası projelerde saklanır: yeniden adlandırılmaz, sırası değişmez, yalnızca sona eklenir. `ofx_describe_and_param_freeze` testi bu listeyi kilitler. Plugin identifier `com.wronsvp.ofx.MotionPath` **geçicidir** ve ilk sürümden önce Wron'un ad alanıyla hizalanmalıdır. **[Wron ile doğrula]**

## 10. Klavye ve odak (editör)

- Kısayollar yalnızca editörün kökü odaktayken ve bir metin alanı odakta değilken çalışır. Yalnızca işlenen tuşlarda `preventDefault` çağrılır.
- **Boşluk tuşu, J/K/L ve diğer tüm tuşlar host'a bırakılır.**
- Kısayollar: V (seç), P (ekle), X (sil), Delete, Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z, Ctrl+A, Esc, ok tuşları (Shift ile ×10; ardışık itmeler tek undo adımı), 1/2/3 (köşe/yumuşak/bağımsız), F (yola sığdır), Shift+F veya Home (kareye sığdır), G (ızgara), C (kapalı yol), +/−.
- Sürükleme sırasında Ctrl yakalamayı kapatır, Alt kolu bağımsız yapar, Shift eksene kilitler veya 15° adımla döndürür.
