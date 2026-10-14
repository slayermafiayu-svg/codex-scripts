# Model araştırması ve seçilen yaklaşım

Tarih: Ekim 2026. Rakamlar modellerin resmi GitHub sayfalarından, lisans
metinlerinden ve resmi kodlarından alındı. Her satırda kaynağı var. Bu oturumda
doğrulanamayanlar "doğrulanmadı" diye işaretli. Hugging Face, arXiv ve VEGAS
yardım sitesi geliştirme ortamından erişilemiyordu.

## 1. İki ayrı iş: hazır model çalıştırmak ve sıfırdan eğitmek

| | Hazır (önceden eğitilmiş) modeli yerelde çalıştırmak | Sıfırdan model eğitmek |
|---|---|---|
| Ne gerekir | Ağırlıkları indirmek (pin modu ~22 GB, kare modu +~30 GB) ve tek bir NVIDIA GPU | Binlerce saatlik lisanslı hareket verisi ve çok GPU'lu küme |
| Örnek | HY-Motion 1.0: Tencent'in yayımladığı 1.0B parametreli ağırlıklar | HY-Motion'ı Tencent eğitti: 3.000+ saat ön eğitim, 400 saat seçilmiş veriyle ince ayar, ardından pekiştirmeli öğrenme ([makale](https://arxiv.org/abs/2512.23464)) |
| Bu projede | **Yapılan bu.** Kurulum ağırlıkları D:'ye indirir, model sizin GPU'nuzda çıkarım (inference) yapar | **Yapılmadı, yapılamaz.** 1B'lik bir DiT'i 8–24 GB'lık tek kartta sıfırdan eğitmek gerçekçi değil |

Ara yol olarak bir modele ince ayar (LoRA vb.) yapmak teknik olarak mümkün olabilir. Ama
kendi lisanslı hareket veriniz gerekir. HY-Motion lisansı da Tencent modeli dışında bir
modeli eğitmek için çıktılarının kullanılmasını yasaklar (madde 5b). Bu projede ince ayar
yok.

## 2. Doğrulanan modeller

| Model | Ne üretir | Parametre | Lisans | Windows | Resmi bellek ihtiyacı |
|---|---|---|---|---|---|
| **HY-Motion 1.0** (Tencent, Ara 2025) | Metinden 3B iskelet hareketi (SMPL-H, 30 fps, en fazla 360 kare = 12 sn) | DiT **1.0B** (Lite 0.46B); metin kodlayıcı **Qwen3-8B** + CLIP ViT-L/14 | Tencent HY-Motion 1.0 Community License: **AB, Birleşik Krallık ve Güney Kore'de geçersiz**; 1 milyon üzeri aylık kullanıcısı olanlar ayrı lisans almalı; çıktılarla başka bir modeli iyileştirmek yasak | README: "supports macOS, Windows, and Linux" | En az **26 GB** (Lite 24 GB); prompt-rewrite LLM'i hariç |
| **Wan2.2-TI2V-5B** (Alibaba Wan) | Metin + görselden video, 720P, 24 fps | **5B** yoğun | **Apache-2.0** | README'de Windows notu yok | En az **24 GB**; RTX 4090'da 720P I2V: **524.8 sn, tepe 22.8 GB** (`--offload_model --convert_model_dtype --t5_cpu`) |
| Wan2.2-I2V-A14B | Görselden video | 27B toplam / 14B etkin (MoE) | Apache-2.0 | Not yok | Tek GPU'da en az **80 GB**; 480P tek GPU tepe 41.0 GB |
| Wan2.2-Animate-14B | Referans görsel + **sürücü video** → karakter animasyonu | ~14B (adından) | Apache-2.0 | Not yok | Belirtilmemiş |
| Wan-Animate-2 (Ağu 2026) | Referans görsel + sürücü video; metinle yalnızca kamera açısı | 14B (checkpoint adından) | Apache-2.0 | Linux komutları | Belirtilmemiş; varsayılanlar 8×A800, 480P 2×A800'de test edilmiş |
| SCAIL-2 (Z.ai, Haz 2026) | Referans görsel + poz/sürücü video → animasyon | Wan 2.1 14B tabanlı | Apache-2.0 (GitHub README) | Not yok | Belirtilmemiş |
| HunyuanVideo 1.5 (Kas 2025) | Metin/görselden video | 8.3B | Tencent Hunyuan Community License (AB/BK/GK hariç) | README'de işletim sistemi: Linux | En az 14 GB (offload açık) |
| LTX-2.5 (Lightricks, Ağu 2026) | Ses + video, metin/görselden | ~22B (dosya adı "22b"), metin kodlayıcı Gemma 4 12B | LTX-2.x Community License: yıllık geliri **10 milyon $ ve üzeri** şirketler ücretli lisans almalı | `natten` yalnızca Linux; Windows'ta yedek yola düşer | Resmi VRAM rakamı yok; tam indirme ~66 GiB; FP8 + offload öneriliyor |

Kaynaklar: [HY-Motion-1.0 GitHub](https://github.com/Tencent-Hunyuan/HY-Motion-1.0)
(README, `License.txt`, `ckpts/README.md`, kod); [Wan2.2 GitHub](https://github.com/Wan-Video/Wan2.2)
(README, `LICENSE.txt`, `assets/comp_effic.png`); [Wan-Animate-2](https://github.com/Wan-Video/Wan-Animate-2);
[SCAIL-2](https://github.com/zai-org/SCAIL-2); [HunyuanVideo-1.5](https://github.com/Tencent-Hunyuan/HunyuanVideo-1.5)
(README, `LICENSE`); [LTX-2](https://github.com/Lightricks/LTX-2) (README, `LICENSE`, `LICENSE-2_x`).

Notlar:
* **HY-Motion'ın kodundan doğrulananlar:** Metin kodlayıcı `Qwen/Qwen3-8B` (bf16, son gizli
  katman) ve `openai/clip-vit-large-patch14`. DiT resmi kodda FP32 çalışır. Çıktı 22 gövde
  eklemi (52 eklemli SMPL-H, 30 fps). Dikkat attention'ı PyTorch SDPA ile yapıyor;
  flash-attn gerekmez, bu Windows için önemli. `stats/` eksikse resmi kod sessizce boş
  normalizasyona düşer. Bizim kodumuz bu durumda durur.
* Qwen3-8B'nin tam parametre sayısını (≈8.2B, bf16 ≈ 16.4 GB) bu oturumda Hugging Face'ten
  yeniden doğrulayamadım. Tablodaki 15.1 GB, LM başı yüklenmeden gövde için yapılmış bir
  **hesap**.
* Sürücü video isteyen modeller (Wan-Animate, Wan-Animate-2, SCAIL-2) yalnızca metinle yeni
  hareket üretmez. Hareketin bir videodan gelmesi gerekir.

## 3. Gerçek FP16 bellek ihtiyacı

Burada üç tür rakam var ve karıştırılmamalı:

| | Kaynak | HY-Motion 1.0 | Wan2.2-TI2V-5B |
|---|---|---|---|
| Resmi | Yazarların README'si | ≥26 GB (Qwen3-8B bf16 + DiT FP32 aynı anda GPU'da) | 22.8 GB tepe (RTX 4090, 720P) |
| Hesaplanan (yalnız ağırlık) | parametre × 2 bayt | DiT FP16: 2.0 GB; Qwen3-8B gövdesi bf16: ~15.1 GB | Transformer FP16: ~10 GB |
| Ölçülen | `selftest.bat` sizin kartınızda | **Henüz yok.** Bu geliştirme ortamında GPU olmadığı için ölçülemedi | **Henüz yok** |

Profiller resmi tabloları aşağıya çekmek için şunları yapar:

1. Qwen3-8B'yi **önce** çalıştırır, sonucu D:'ye önbelleğe alır ve bellekten çıkarır.
2. Ardından DiT'i FP16'da yükler.

Qwen3 kartınıza sığmazsa GPU, RAM ve D: diski arasında bölünür (accelerate). Ağırlıklar aynı
bf16 ağırlıklardır, sonuç değişmez, yalnızca yavaşlar.

| Profil | Metin kodlayıcı | Hareket DiT | Kare modu (Wan2.2-TI2V-5B) |
|---|---|---|---|
| 8 GB | 5 GB GPU + RAM (+ D:) | FP16, GPU | Kapalı (deneysel: katman katman offload, 832×480, 49 kare) |
| 16 GB | 12 GB GPU + RAM | FP16, GPU | 960×544, 81 kare, model offload (tahmini; ölçün) |
| 24 GB | Tamamı GPU, bf16 | FP16, GPU | 1280×704, 121 kare, model offload (resmi ölçüme en yakın yapılandırma) |

FP16 hakkında: DiT'i FP16'da çalıştırıyoruz (ağırlıklar FP16, matris çarpımları FP16
autocast, ODE çözücü FP32). Resmi kod FP32 kullanır. `selftest.bat`, aynı seed ile FP32 ve
FP16 çıktılarını eklem eklem karşılaştırır ve farkı santimetre olarak yazar. Qwen3-8B ve
umT5 metin kodlayıcılarını resmi kodlarındaki gibi **bf16** tutuyoruz. Bu kodlayıcıların
FP16'da doğruluğu yazarlarınca verilmemiş. Komut başına yalnızca bir kez çalıştıkları için
hız açısından fark yaratmazlar. bf16 desteklemeyen kartlarda (RTX 20xx) metin kodlayıcı
CPU'da çalışır.

## 4. Çıktı: video kareleri mi, düzenlenebilir pin hareketleri mi?

**Ana çıktı düzenlenebilir pin hareketleridir.** Kare modu ikinci bir seçenek olarak durur.

### Pin modu (HY-Motion 1.0 → Puppet Pin)

1. HY-Motion, tarifinizden 3B bir iskelet hareketi **üretir**. Hazır preset kullanılmaz.
2. Hareket, karakterinizin görünüm açısına (önden, yandan, 3/4) göre 2B'ye izdüşürülür.
3. Karakterin kendi kemik boyları korunarak eklemlere aktarılır (retarget). Yere basan ayak
   kaymasın diye ayak kilidi uygulanır.
4. Sonuç, PuppetPin OFX efektinin pinlerine her kare için bir anahtar olarak yazılır.

Neden ana çıktı bu:

* **Görünüm birebir korunur.** Ekrandaki pikseller sizin resminizdir; model resmi yeniden
  çizmez.
* **VEGAS'ta düzenlenebilir.** Pinler, elle konmuş pinler gibi taşınır ve anahtarlar
  değiştirilebilir. Mevcut pin/mesh aracınız (PuppetPin) olduğu gibi kullanılır.
* **Oynatma hafiftir.** Ağır AI işi yalnızca üretimde, ayrı bir süreçte çalışır. VEGAS
  oynatırken sadece ARAP bükmesi hesaplanır.
* **8 GB'ta da çalışır.**

Sınırlar (tek resmi bükmenin doğasından gelir):

* **Yeni görünüm oluşmaz.** Arkasını dönme, profilden önden görünüme geçme gibi hareketler
  doğru görünmez. Resimde çizilmeyen taraf ortaya çıkamaz.
* **Kol ya da bacak gövdenin önünden geçtiğinde örtüşme sırası değişmez.** Bunun için
  karakteri **parça katmanlarına** ayırın (uzak kol, uzak bacak, gövde, yakın bacak, yakın
  kol; SAM aracınızla). Araç her parçayı ayrı PuppetPin ile ayrı VEGAS izine koyar.
* Önden görünümde kameraya doğru yürüme yerinde yürüme olarak görünür (ortografik izdüşüm).
  Sahne boyunca yürümek için yandan çizilmiş karakter kullanın.
* Oturma hareketinde sandalye yoktur; kalça iner, sandalyeyi siz eklersiniz.
* Model yalnızca **metni** görür. Resim, iskeleti ve görünümü sağlar ama hareketi
  etkilemez.
* Tarifler İngilizce olmalıdır. Resmi örnek komutlar İngilizce.

### Kare modu (Wan2.2-TI2V-5B)

Model resmi ve metni birlikte görür, yeni kareler çizer. Dönme, kıyafet ve saç hareketi
gibi şeyleri yapabilir. Sınırları:

* Karakterin yüzü, kıyafeti ve oranları birkaç saniyede **kayabilir**.
* Çıktı pikseldir; pinlerle düzenlenemez.
* 16–24 GB gerekir ve 720P 5 saniye RTX 4090'da yaklaşık 9 dakika sürer (resmi ölçüm).

Arka plan yeşil perdeyle verilir ve sonuç saydam PNG kare dizisi olarak VEGAS'a eklenir.

### Bilinçli olarak yapılmayanlar

* **Preset yok.** Model yoksa ya da ağırlıklar eksikse üretim, neyin eksik olduğunu
  söyleyip durur. Hiçbir şeye sessizce düşmez.
* Her sonuç kaynağını taşır: model, kontrol noktasının SHA-256'sı, seed, komut, dtype,
  süreler, tepe VRAM. VEGAS'taki take adı "AI: tencent/HY-Motion-1.0 (HY-Motion-1.0)
  seed=…" olur.
* Test verisi önizlemede ve VEGAS'ta **"TEST - AI DEĞİL"** diye işaretlenir.

## 5. VEGAS oynatılırken ağır iş yok

* VEGAS uzantısı (`AIMotionBridge.dll`) oynatma ve render durumunu saniyede 4 kez
  `D:\AIMotion\state\vegas_state.json` dosyasına yazar.
* Üretim süreci her model adımından önce bu durumu kontrol eder. VEGAS oynatıyor veya
  render alıyorsa iş duraklar ve hiçbir hesap yapılmaz.
* VEGAS açık ama uzantı yoksa ya da durum eskiyse de iş bekler. Uzantının olmaması bu
  korumayı sessizce kapatmaz.
* Üretim ayrı bir süreçte, düşük CPU önceliğiyle çalışır. İş bitince süreç kapanır ve GPU
  belleği VEGAS'a geri döner.
* Bilinen sınır: Duraklama, o anda yüklü ağırlıkları VRAM'den boşaltmaz (DiT için ~2 GB).
  Bir model yüklemesi sürerken oynatma başlarsa iş, yükleme bitince duraklar.

## 6. Bu oturumda neyin gerçekten çalıştığı

| Ne | Durum |
|---|---|
| Sizin bilgisayarınızda gerçek üretim | **Yapılamadı.** Geliştirme ortamı bulut Linux kapsayıcısıydı: bilgisayarınıza erişim yok, GPU yok, Hugging Face erişimi kapalı. Gerçek ilk üretim için `first_generation.bat` hazır. |
| HY-Motion'ın resmi kodu + bizim arka uç | Küçük, **rastgele ağırlıklı** bir ağla CPU'da uçtan uca çalıştı: yapılandırma, kontrol noktası ve istatistik denetimleri, koruma ve ilerleme kancaları, aday toplu işleme, 52 eklemli çıktı, retarget. Hareketin kendisi anlamsız; amaç tesisatı sınamaktı. |
| Metin kodlayıcı yolu | HY-Motion'ın `HYTextModel`'i, minik rastgele Qwen3/CLIP ile yerleşim ayarları ve D: önbelleği dahil çalıştı. |
| Puppet Pin önizlemesi | `puppet_render` PuppetCore.h ile derlendi. Kimlik, öteleme ve parça katmanı testleri geçti. |
| VEGAS betiği ve uzantısı | C# 5 olarak sahte (stub) bir API'ye karşı derlendi ve sahte zaman çizelgesinde içe aktarma sınandı. **Gerçek ScriptPortal.Vegas API'siyle derleme sizin makinenizde `build_extension.bat` ile olacak.** Emin olunamayan üyeler yansıma (reflection) ile çağrılıyor. |
