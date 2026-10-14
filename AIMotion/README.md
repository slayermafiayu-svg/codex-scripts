# AI Motion – VEGAS Pro için yerel AI hareket üretimi

Bir karakter resmi seçersiniz ve hareketi yazarsınız ("A person walks forward, then sits
down."). Bilgisayarınızdaki önceden eğitilmiş bir model hareketi **üretir**. Sonucu
önizler ve VEGAS'a aktarırsınız.

* **Pin modu (ana mod):** [HY-Motion 1.0](https://github.com/Tencent-Hunyuan/HY-Motion-1.0)
  (Tencent, 1.0B parametre) metinden 3B iskelet hareketi üretir. Araç bu hareketi
  karakterinize uyarlar ve **Puppet Pin** (PuppetPinOFX) pin anahtarları olarak VEGAS'a
  yazar. Görünüm birebir korunur ve pinler VEGAS'ta düzenlenebilir.
* **Kare modu:** [Wan2.2-TI2V-5B](https://github.com/Wan-Video/Wan2.2) (Apache-2.0) resim +
  metinden video kareleri çizer. Dönme gibi yeni görünüm isteyen hareketler içindir;
  16–24 GB gerekir ve kareler düzenlenemez.

Neden bu yaklaşım seçildi, modellerin parametre/lisans/Windows/VRAM bilgileri ve
sınırlar: **[docs/ARASTIRMA.md](docs/ARASTIRMA.md)**.

Hazır hareket preseti **yoktur**. Model ya da ağırlık eksikse araç neyin eksik olduğunu
söyler ve durur. Her sonuç hangi modelin, hangi kontrol noktası ve seed ile ürettiğini
taşır.

## Kurulum (Windows, NVIDIA)

Gerekenler:

* Python 3.10–3.12
* Git ve Git LFS
* CUDA 12.4'ü destekleyen NVIDIA sürücüsü
* D: sürücüsünde yer: pin modu için yaklaşık 30 GB (venv + modeller), kare modu için
  ek ~35 GB

```powershell
powershell -ExecutionPolicy Bypass -File install.ps1            # pin modu
powershell -ExecutionPolicy Bypass -File install.ps1 -WithWan   # + kare modu
```

`install.ps1` şunları kurar ve hepsini D:'ye koyar:

* `D:\AIMotion\venv`: Python ortamı (torch 2.5.1 + CUDA 12.4)
* `D:\AIMotion\repos\HY-Motion-1.0`: HY-Motion'ın resmi kodu (Git LFS ile)
* `D:\AIMotion\models\…`: resmi Hugging Face depolarından ağırlıklar
  * `tencent/HY-Motion-1.0`
  * `Qwen/Qwen3-8B`
  * `openai/clip-vit-large-patch14`
  * (`-WithWan` ile) `Wan-AI/Wan2.2-TI2V-5B-Diffusers`
* `D:\AIMotion\cache`: tüm Hugging Face, PyTorch, pip, Triton ve CUDA önbellekleri. Kullanıcı
  ortam değişkenleri de buraya yönlendirilir (`AIMOTION_ROOT`, `HF_HOME`, `PIP_CACHE_DIR`,
  `TORCH_HOME`).
* VEGAS uzantısı ve içe aktarma betiği (`vegas\build_extension.bat`)

Başka bir kök için `-Root "D:\Baska\Yer"` verin. Kök D: dışındaysa araç çalışmayı reddeder;
`aimotion.json` içindeki `required_drive` ile değiştirilebilir.

Önizleme, PuppetPin'in kendi çekirdeği (PuppetCore.h) ile derlenen `bin\puppet_render.exe`
ile yapılır. Böylece önizleme, VEGAS'taki bükmeyle aynıdır. Exe yoksa kurulum CMake ve MSVC
ile derler. O da yoksa GitHub Actions'taki **AIMotion-Win64** paketinden kopyalayın.

## İlk kullanım

1. **`selftest.bat`**: Kartınızda gerçek tepe VRAM'i ölçer. Qwen3 yerleşimini ve DiT'in
   FP32 ve FP16 hallerini dener, FP16 ile FP32 arasındaki farkı santimetre cinsinden yazar.
   Sonuç `D:\AIMotion\selftest.json` dosyasına gider ve arayüzde görünür.
2. **`first_generation.bat`**: Örnek yandan karakter
   ([examples/demo_side](examples/demo_side), parça katmanlarıyla) için HY-Motion iki aday
   üretir, önizlemeleri yazar ve ilkini VEGAS'a hazırlar. VEGAS'ta **Tools → Extensions →
   AI Motion: Import Latest Result** komutunu çalıştırın.
3. **`start_ui.bat`**: Arayüzü açar: <http://127.0.0.1:7860>. Arayüze yalnızca bu
   bilgisayardan erişilebilir.

## Arayüz

1. **Görsel seç:** Saydam PNG en iyisidir. Saydamlık yoksa maske verin.
   * İsteğe bağlı olarak **parça katmanları** ekleyin. Bunlar karakterle aynı boyutta
     PNG'lerdir ve ada göre arkadan öne sıralanır, örneğin `00_uzak_kol`, `01_uzak_bacak`,
     `02_govde`, `03_yakin_bacak`, `04_yakin_kol`.
   * Eklemleri sürükleyerek düzeltin, ya da mevcut DWPose aracınızın **OpenPose JSON**
     çıktısını içe aktarın. 133 noktalı COCO-WholeBody dizisi de okunur.
2. **Hareketi tarif et:** İngilizce yazmanız önerilir. Burada şunları seçersiniz:
   * süre (0.7–12 sn) ve aday sayısı
   * VRAM profili (otomatik / 8 / 16 / 24 GB)
   * görünüm (otomatik / önden / yandan / 3/4)
   * sahnede yer değiştirme ve ayak kilidi
3. **Üret:** Üretim ayrı bir süreçte çalışır. **VEGAS oynatırken veya render alırken
   duraklar.**
4. **Önizle:** Her adayın videosu ve kaynak etiketi gösterilir. Görünüm/ayak ayarlarını
   değiştirip modeli yeniden çalıştırmadan uygulayabilirsiniz.
5. **VEGAS'a aktar:** Sonuç `D:\AIMotion\outbox` klasörüne yazılır. VEGAS'ta içe aktarma
   komutu şunları yapar:
   * sahne resmini imleçte yeni bir parçaya koyar,
   * Puppet Pin efektini ekler,
   * pinleri karaktere yapıştırır,
   * her kare için pin anahtarı yazar.

   Parça katmanları varsa her parça kendi izine ve kendi Puppet Pin'ine konur. "PNG
   kareleri de yaz" seçilirse Puppet Pin olmadan kullanılacak saydam bir kare dizisi de
   üretilir.

## VEGAS tarafı

* `vegas\AIMotionBridge.cs` bir uygulama uzantısıdır:
  * Oynatma ve render durumunu `D:\AIMotion\state\vegas_state.json` dosyasına yazar.
    Üretim bu dosyaya bakarak duraklar.
  * Tools → Extensions'a iki komut ekler.
* `vegas\AIMotion_Import.cs` aynı içe aktarmayı uzantı olmadan yapar: **Tools → Scripting →
  Run Script**.
* `build_extension.bat`, VEGAS'ınızın `ScriptPortal.Vegas.dll` dosyasına karşı .NET
  Framework'ün `csc.exe`'si ile (C# 5) derler. Uzantıyı
  `Documents\Vegas Application Extensions` klasörüne, betiği
  `Documents\Vegas Script Menu` klasörüne kopyalar.
* VEGAS sürümleri arasında farklılık gösterebilecek API üyeleri (oynatma olayları,
  `Transport.IsPlaying`, eklenti ağacı, `AddImageSequence`) yansıma (reflection) ile
  çağrılır; yoksa derleme yine başarılı olur.
* Oynatma durumu hiç okunamıyorsa uzantı `"playing": null` yazar ve üretim, VEGAS açık
  olduğu sürece bekler.

## Komut satırı

```bat
python -m aimotion.cli doctor
python -m aimotion.cli pins --image karakter.png --rig dwpose.json --prompt "A person waves with the right hand." --duration 3 --seeds 1,2 --export
python -m aimotion.cli frames --image karakter.png --prompt "The character turns around and waves." --export
python -m aimotion.download --dry-run
python -m aimotion.selftest --wan-compare
```

## Klasörler

```
aimotion/            Python paketi
  paths.py           D: düzeni, önbellek ortam değişkenleri
  profiles.py        8/16/24 GB profilleri (resmi / hesaplanan / ölçülen rakamlar ayrı)
  guard.py           VEGAS oynatma/render koruması
  backends/          HY-Motion ve Wan2.2 çalıştırıcıları (gerçek ağırlıklar, yedek yol yok)
  retarget.py        3B hareket -> karakterin 2B eklemleri (kemik boyu korunur, ayak kilidi)
  layers.py          parça katmanlarına pin atama (gövde/kol/bacak)
  pins.py            Puppet Pin anahtarları (pinNN_pos / pinNN_anchor / pinNN_on)
  preview.py         puppet_render ile önizleme ve PNG kare dizisi
  jobs.py, worker.py iş klasörleri, ayrı süreçte üretim
  server.py, web/    yerel arayüz
  cli.py, download.py, selftest.py
native/puppet_render.cpp   PuppetCore.h ile önizleme işleyicisi
vegas/               VEGAS uzantısı, içe aktarma betiği, derleme betiği
examples/            çizilmiş örnek karakterler (resim + eklemler + parça katmanları)
tests/               testler (GPU/ağırlık gerektirmez)
```

## Lisanslar

Bu proje model ağırlıklarını dağıtmaz; kurulum onları resmi depolardan indirir ve her model
kendi lisansıyla gelir. HY-Motion 1.0'ın lisansı AB, Birleşik Krallık ve Güney Kore'de
geçerli değildir ve 1 milyon üzeri aylık kullanıcısı olanlardan ayrı lisans ister.
Ayrıntılar: [docs/ARASTIRMA.md](docs/ARASTIRMA.md).

## Testler

```bash
pip install -r requirements-dev.txt
python -m pytest            # cmake + C++ derleyici varsa puppet_render da derlenip sınanır
# HY-Motion'ın resmi koduyla tesisat testi (küçük, rastgele ağırlıklı ağ; torch gerekir):
AIMOTION_TEST_HYMOTION_REPO=/yol/HY-Motion-1.0 python -m pytest tests/test_hymotion_plumbing.py
```
