# Panduan publish

Repo ini sudah bersih dari nama lama dan siap di-push.
Semua nama di bawah memakai asumsi repo `thewoldaa/hub-volume`; ganti bila memakai
nama lain (lihat daftar berkas yang perlu disesuaikan di bagian akhir).

## 1. Buat repo di GitHub

1. Buka <https://github.com/new>.
2. Nama repo: `hub-volume`.
3. **Jangan** centang "Add a README", "Add .gitignore", atau "Choose a license".
4. Buat repo, lalu dari folder ini:

```powershell
git init
git add .
git commit -m "Hub Volume 1.0.0"
git branch -M main
git remote add origin https://github.com/thewoldaa/hub-volume.git
git push -u origin main
```

CI akan berjalan otomatis dan menyimpan `HubVolume-1.0.0-portable.zip`,
`HubVolume-Setup-1.0.0.exe`, dan `SHA256SUMS.txt` sebagai artifact.

## 2. Publish release 1.0.0

Cara termudah: push tag, biarkan workflow yang membuat release.

```powershell
git tag v1.0.0
git push origin v1.0.0
```

Workflow akan build ulang dari source, lalu membuat release `v1.0.0` dengan ketiga
berkas terlampir. Setelah selesai, buka <https://github.com/thewoldaa/hub-volume/releases>
dan periksa judul serta catatan rilisnya.

Alternatif manual: buka **Releases > Draft a new release**, pilih tag `v1.0.0`, unggah
isi folder `release/` hasil `.\scripts\build.ps1`.

## 3. Aktifkan sponsor

Repo sudah punya `.github/FUNDING.yml`. Isinya:

```yaml
patreon: Craftkal
```

- **Patreon**: halaman <https://www.patreon.com/c/Craftkal> terhubung lewat nilai
  `patreon: Craftkal`. Tombol **Sponsor** di halaman repo akan mengarah ke sana.
  Kalau URL halaman berubah, sesuaikan satu baris di `FUNDING.yml` dan dua tautan
  Patreon di `README.md` serta `README.id.md`.

## 4. Setelah publish

- Isi deskripsi repo: `Kunci volume perangkat, mikrofon, dan aplikasi di Windows.`
- Tambahkan topics: `windows`, `audio`, `wasapi`, `volume-mixer`, `cpp`, `win32`.
- Aktifkan **Discussions** bila ingin menampung pertanyaan.
- Unggah `assets/screenshot.png` sebagai social preview di **Settings > General**.
- Peringatan SmartScreen pada installer/exe belum ditandatangani adalah normal;
  code signing bisa ditambahkan nanti.

## Berkas yang menyebut nama repo atau akun

Cari dan ganti bila memakai nama berbeda:

```powershell
Select-String -Path README.md,README.id.md,.github/FUNDING.yml -Pattern 'thewoldaa|hub-volume'
```
