# Changelog

## 1.1.0

- Aturan volume otomatis per aplikasi: volume turun saat aplikasi masuk panggilan lalu kembali sendiri.
- Preset siap pakai untuk Discord, Zoom, Teams, Google Meet, Skype, Slack, Webex, Telegram, dan WhatsApp Desktop.
- Deteksi berbasis proses, sesi audio, dan mikrofon; aplikasi yang sedang berbunyi diprioritaskan.
- Pemilih aplikasi dari daftar proses berjalan atau berkas `.exe` manual.
- Target volume bisa ke volume utama atau volume aplikasi itu sendiri, dengan jeda pemulihan 1-10 detik.
- Kunci volume ikut berpindah ke target aturan dan kembali ke posisi semula saat aturan selesai.
- Perubahan manual saat aturan aktif dihormati dan tidak ditimpa saat volume dipulihkan.
- Bila beberapa aturan aktif bersamaan, target terendah yang dipakai.
- Dasbor baru: status langsung, kartu aturan, simulasi, dan log aktivitas.
- Tema terang dan gelap mengikuti pengaturan Windows.
- Penyimpanan config di-debounce agar slider tetap mulus.

## 1.0.0

- Rilis publik pertama sebagai Hub Volume.
- Kunci volume dan status bisu untuk perangkat keluaran, mikrofon, dan sesi aplikasi.
- Penegakan kunci berbasis callback Core Audio, tanpa polling.
- Pemulihan otomatis setelah aplikasi, sesi, atau perangkat audio berubah.
- Mixer flyout dari ikon tray, autostart per pengguna, dan mode portable.
- Installer per pengguna tanpa hak administrator.
