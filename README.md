# PS3 File Manager — by [Cruyfford](https://github.com/Cruyfford)

**صاحب المشروع ومطوّره (Project Creator & Developer): [Cruyfford](https://github.com/Cruyfford)**

**Repository:** [Cruyfford/PS3-FileManager](https://github.com/Cruyfford/PS3-FileManager)

مدير ملفات عصري لـ PS3 (HEN و CFW) يقرأ كل الأقراص ويثبّت ملفات PKG.

## الأقراص المدعومة
| القرص | نظام الملفات |
|---|---|
| فلاش / هارد خارجي | **FAT32** (مباشر)، **exFAT** (FatFs)، **NTFS** و **ext2/3/4** (libntfs_ext) |
| القرص الداخلي | `/dev_hdd0` بالكامل + اختصار `packages` |
| النظام | `/dev_flash` `/dev_flash2` `/dev_flash3` `/dev_hdd1` `/dev_blind` (إن وُجد) |
| أخرى | `/dev_bdvd` (البلوراي) و `/app_home` |

الأقراص الخارجية تُكتشف تلقائياً عند توصيلها (FAT32)، و R3 أو "تحديث الأجهزة" لإعادة فحص NTFS/exFAT.

## العمليات
- نسخ / قص / لصق بين أي قرصين (مثلاً من NTFS إلى الهارد الداخلي)، مع شريط تقدّم وسرعة ووقت متبقٍ وإلغاء.
- حذف (مع تحذير خاص لملفات النظام)، إعادة تسمية، مجلد جديد (بلوحة مفاتيح PS3).
- تحديد متعدد (□) وتحديد الكل (SELECT).
- خصائص الملف، ولملفات PKG: الاسم الحقيقي والصورة و Content ID والإصدار والنوع، وهل الحزمة مجانية أم تحتاج RAP.
- تثبيت PKG من أي قرص (حتى NTFS/exFAT): تثبيت تلقائي عبر XMB أو نسخ إلى مجلد packages.
- **فك PKG**: يستخرج كل ملفات الحزمة (retail أو debug) إلى مجلد باسم Content ID.
- **محرر PARAM.SFO**: تعديل TITLE و TITLE_ID و APP_VER وغيرها (ضمن الطول المسموح لكل حقل).
- **بناء PKG** من أي مجلد فيه PARAM.SFO: ينتج PKG بصيغة retail مطابقة بايت ببايت لأدوات PSL1GHT (pkg.py + package_finalize)، قابل للتثبيت على HEN.
  - Content ID يؤخذ من اسم المجلد إن كان Content ID صحيحاً، وإلا يُصنع من TITLE_ID.
  - ملاحظة: إن عدّلت EBOOT.BIN يجب أن يكون موقّعاً (NPDRM) قبل البناء؛ البرنامج ينسخه كما هو.
  - FAT32 لا يقبل ملفات أكبر من 4GB؛ للحزم الكبيرة ابنِ إلى الهارد الداخلي أو NTFS/exFAT.
- إنجليزي افتراضياً + العربية (الواجهة تنعكس بالكامل).

## التحكم
| الزر | الوظيفة |
|---|---|
| ✕ | فتح / تثبيت PKG / خصائص |
| ○ أو ← | رجوع (وعند جذر القرص يذهب لقائمة الأجهزة) |
| △ | قائمة الخيارات (نسخ، قص، لصق، حذف، تسمية…) |
| □ | تحديد |
| SELECT | تحديد الكل |
| L1 / R1 | تبديل القرص |
| L2 / R2 | تمرير سريع |
| R3 | إعادة فحص الأقراص |
| START | الإعدادات |

## إضافات إدارة الحفظ وسلامة الملفات (نسخة تجريبية)

- من **Tools → Save Data** اضغط **△** لنسخ الحفظ المحدد إلى مجلد تحرير مستقل تحت `/dev_hdd0/CRUYFFORD_SAVE_WORKSPACE/edit_.../`. يفتح البرنامج النسخة بعد اكتمال النسخ، دون تعديل ملفات الحفظ الأصلية.
- استخدم **Hex editor** أو محرر النصوص على الملفات المنسوخة فقط. النسخ الاحتياطية الحالية تظل متاحة.
- عند العثور على `PARAM.PFD` يعرض البرنامج تحذيرًا: **لم يتم دمج فك التشفير أو إعادة التشفير/توقيع PFD بعد**. لا تحاول إعادة الحفظ المعدّل للعبة المحمية قبل إضافة المحرك والتحقق من البصمات وSecure File ID.
- من متصفح الملفات، اختر ملفًا ثم **△ → Calculate CRC32** لحساب قيمة تحقق في الخلفية. يظهر الناتج عند اكتمال القراءة.
- ميزة CRC32 تكشف الاختلافات العرضية فقط، وليست توقيعًا أمنيًا أو بديلًا عن SHA-256.

### خطة الإضافات المطلوبة وحالة التنفيذ

| الإضافة | الحالة |
|---|---|
| مساحة آمنة لتحرير ملفات الحفظ مع نسخة أصلية سليمة | أضيفت (تحتاج اختبار PS3 فعلي) |
| حساب CRC32 بالخلفية | أضيف (تحتاج اختبار PS3 فعلي) |
| فك تشفير PFD / تعديل الملف ثم إعادة التشفير والتوقيع | لم يُنفذ بعد |
| EBOOT.BIN ↔ ELF (SELF decrypt/sign) | لم يُنفذ بعد؛ يتطلب مفاتيح/إعدادات توافق مناسبة |
| مدير تنزيل مع استكمال التحميل | لم يُنفذ بعد |
| متصفح ثنائي اللوحات | لم يُنفذ بعد |
| التحكم المتقدم من الهاتف | لم يُنفذ بعد (يوجد ربط هاتف أساسي سابقًا) |
| متجر Homebrew | لم يُنفذ بعد |
| مراقبة الحرارة | لم يُنفذ بعد |
| شبكة SMB | لم يُنفذ بعد |
| SHA-256 | لم يُنفذ بعد |
| النسخ الاحتياطي الشامل | لم يُنفذ بعد (النسخ الحالي للحفظ موجود) |
| سلة محذوفات | لم يُنفذ بعد |

## ARCADIA Store V2 — English-only catalog

The Store requires **ARCADIA_PS3_CATALOG.tsv V2** (the old V1 file with broad GAMES/TOOLS categories is no longer accepted). Copy the TSV file from the English V2 ZIP to the root of a FAT32 USB drive, or copy to `/dev_hdd0/ARCADIA_PS3_CATALOG.tsv`, or to `/dev_hdd0/game/CRPK00001/USRDIR/ARCADIA_PS3_CATALOG.tsv`.

Open **Tools → ARCADIA Store**. Use `L1/R1` to switch between **All / PS3 / PS2 / PSP / PS1 / Apps / Themes / DLC**. Use `□` to search by English title or Title ID, `L2/R2` to change pages, and `R3` to retry a missing cover. Names/sections in the Store are English, regardless of application language.

**Downloads:** Press `✕` on a supported *direct* `.pkg` or `.p3t` link. A progress bar and detailed failure message remain above the controls, inside the PS3's 848×512 screen. Downloads are saved under `/dev_hdd0/packages/ARCADIA_XXXXXXXX.pkg` or `.p3t`; `△` cancels, and `SELECT` opens the completed file location. PKG/P3T signature checks reject HTML error pages and invalid files. No auto-install or DRM/activation bypass is performed. Sources classified `external_page` are **not** direct downloads; MediaFire links require opening the page outside the PS3 and resolving the real link there.

**Covers:** The Store tries local `USRDIR/covers/<TITLE_ID>.jpg` or `.png` first, then attempts PS3 artwork (GameTDB) or PS1/PS2 artwork from platform-appropriate repositories. You can manually place matching artwork in that covers folder. A generic platform tile is displayed when no artwork is available. Network artwork is best-effort, not guaranteed for PSN, PSP, Apps, Themes and DLC. Background downloads require image fetching to be enabled in Settings.

**Troubleshooting:** HTTP 403/404 indicates a server refusing/removing the file; a cURL certificate/network error means a connection problem. Check console clock/internet and verify the link on a computer before retrying. Old Sony CDN links are not guaranteed to remain live. Interrupted downloads do not currently resume. User-supplied URLs are not verified individually; download and install only content you are legally entitled to use.


## إصلاح استعادة ملفات الحفظ على PS3 HEN

عالج هذا التحديث مسار **Tools → Save Data Backup → USB saves → ✕ Restore** لحماية ملف الحفظ الموجود عند تعطل أو انقطاع النقل.

- عند اختيار تبويب USB، تظهر **Restore to PS3 user: NNNNNNNN**. يمكن اختيار حساب PS3 المستهدف باستخدام يسار/يمين. لم يعد البرنامج يخمّن الحساب بحسب عدد ملفات الحفظ.
- **اضغط ✕ مرتين:** الأولى لتأكيد اسم الحساب، والثانية لبدء الاستعادة. العملية تنطلق بخيط نقل في الخلفية وتعرض تقدم النسخ.
- تُنسخ ملفات الحفظ أولًا إلى `/dev_hdd0/CRUYFFORD_RESTORE_STAGING/`، ثم تُفحص الملفات وأحجامها مقارنة بالمصدر. إذا فشلت القراءة أو فصل USB، **لا يمس البرنامج ملفات الحفظ الأصلية**.
- عند وجود حفظ بنفس الاسم، يحتفظ البرنامج بالحفظ السابق خارج مجلد saves في `/dev_hdd0/CRUYFFORD_SAVE_RESTORE_BACKUPS/<USER_ID>/`، ثم ينقل النسخة المحقّق منها إلى مكانها. في حال فشل النقل النهائي يحاول استرجاع الحفظ السابق تلقائيًا، ولا يحذف النسخة الاحتياطية.
- لن تستعرض الواجهة ملفات USB بالتزامن مع نقلها في الخلفية، تجنبًا للتعارض المحتمل مع نظام ملفات FAT/exFAT.
- من الممكن أن تبقى ملفات في مجلد staging إن فشلت العملية؛ لا تحذفها قبل مراجعة نتيجة النقل وحفظ النسخ الأصلية.
- **المهم:** هذه استعادة *مطابقة للبايتات* لا تفك تشفير `PARAM.PFD` ولا تعيد توقيعه لحساب جديد. إذا حوّلت حفظًا محميًا بين حسابين مختلفين فقد ترفضه اللعبة. قم أولًا بتجربة الحفظ على الحساب الأصلي، واستخدم النسخة الاحتياطية الموجودة لاسترجاعه عند الحاجة.
- نجح بناء حزمة PS3 على GitHub Actions، لكن يجب اختبار الاستعادة فعليًا على جهاز PS3 HEN. اختبر أولًا حفظًا غير مهم واحتفظ بنسخة USB منفصلة.

## Save Data Manager — مثلث للنسخ والحذف

تم تحديث **Tools → Save Data Manager** (القسم الذي كان اسمه Save Data Backup).

- اختر حفظًا من **HDD saves** أو **USB saves**، ثم اضغط **△** لفتح قائمة الإجراءات: `Copy to USB (Backup)` أو `Copy to PS3 (Restore)` حسب التبويب، و`Copy to Editing Workspace`، و`Delete (Move to Trash)`، و`Cancel`.
- نسخ HDD → USB يستخدم المسار القياسي `USB:/PS3/SAVEDATA`، ويرفض خيار النسخ من قائمة المثلث إذا كانت نسخة بنفس الاسم موجودة بالفعل كي لا يستبدلها دون تحذير.
- نسخ USB → PS3 يستخدم عملية الاستعادة الآمنة السابقة: حساب PS3 المستهدف قابل للاختيار، ويُشترط تأكيد إضافي، وتنقل الملفات من مساحة staging على HDD بعد فحص أحجامها.
- **Delete لا يمسح بشكل دائم:** يزيل الحفظ المحدد من قائمة الملفات عن طريق نقل مجلده كاملًا إلى سلة محذوفات على القرص نفسه، وبزر ✕ ثانٍ للتأكيد. يمكن استرجاعه يدويًا عبر مدير الملفات:
  - حفظ HDD: `/dev_hdd0/CRUYFFORD_SAVE_TRASH/<USER_ID>/`
  - حفظ USB: `<USB_ROOT>/CRUYFFORD_SAVE_TRASH/`
- الملفات في سلة المحذوفات تحتفظ بحجمها؛ لا يُفرغها البرنامج تلقائيًا. يُمكن إعادة المجلد إلى مكان الحفظ الصحيح بعد التحقق من حساب PS3 وطبيعة الحفظ.
- `Copy to Editing Workspace` تنشئ نسخة بعيدة عن ملفات الحفظ الحية لاستخدام محرر HEX، لكنها **لا تفك تشفير PARAM.PFD ولا تعيد توقيعه**. تعديل ملفات حفظ محمية بلا مفاتيح وتوقيع صالح قد يجعلها غير قابلة للتحميل.
- راجع الحفظ قبل حذفه واختبر الميزة أولًا بحفظ غير مهم على PS3 HEN. نجاح البناء البرمجي لا يثبت تلقائيًا نجاح تشغيل كل نظام ملفات على الجهاز.

## Save Data Manager — PS3 save operations

Select a game save under **Tools → Save Data Manager**, then press **✕** to open the five-action menu:

| Menu | Current behavior |
| --- | --- |
| **Decrypt Save Data** | Read-only PARAM.PFD v3/v4 format check, not yet able to decrypt game-protected files. Reports the PFD version/count or missing format, without modifying the save. |
| **Edit Save Data** | Creates an isolated backup under `/dev_hdd0/CRUYFFORD_SAVE_WORKSPACE/`, opens it in the integrated file browser; editing raw bytes is available through the existing HEX editor. |
| **Encrypt Save Data** | Disabled for protected game data until the corresponding per-file Secure File ID and matching crypto implementation can be verified. |
| **Resign Save Data** | **Limited real operation:** for saves whose PFD contains *only* a `PARAM.SFO` entry, double-confirm ✕ to copy the entire save into `/dev_hdd0/CRUYFFORD_SAVE_CRYPTO/resign_...`, recalculate PFD signatures, verify PFD top/bottom integrity and open the resulting copy. All other saves are rejected. **It does not modify the live game save or silently restore the new copy.** |
| **Backup / Restore** | Existing real backup and protected transactional restoration, with double-confirm on USB → PS3. |

Press **△** on an individual save for Copy, Editing Copy, and Move to recoverable Trash.

**Scope and limitations:** The PFD signer for metadata-only saves is derived from the MIT-licensed [SaveSync](https://github.com/Ganitzsh/ps3-savesync) by Julien Ganitzsh and includes the original MIT notice at `third_party/savesync_pfd/LICENSE`. It uses the existing PSL1GHT `polarssl` dependency; no copy of Apollo's GPLv3 implementation was included in the GPLv2 codebase.

This metadata-only signer has been cross-compiled for PS3 but **not tested on a real HEN console**. A matching console profile and game-specific protection might still prevent a resign result from loading in a game. Always keep the original save and use disposable test data. The signer never touches the original, only a working copy. Never use a modified save for actual play until independently verified.

**Encrypted game files are NOT yet supported.** A Secure File ID may differ for each file within the same saved game; we must integrate the full AES encryption/HMAC update backend, correct secure-file keys and regression test vectors before unlocking Decrypt/Encrypt/Resign for those games. The UI never reports false success or silently overwrites game saves.


## البناء عبر GitHub Actions
1. أنشئ مستودعاً جديداً وارفع محتويات هذا المجلد كما هي (بما فيها `.github` و `third_party`).
2. افتح تبويب **Actions** → آخر تشغيل → حمّل الـ artifact، وفيه `FileManager.pkg` (ثبّته على HEN).

## مصادر
- libntfs_ext (Estwald) و libfatfs/exFAT (lmirel): من prepISO في مشروع webMAN MOD.
- سجلات PDB للتثبيت ولوحة المفاتيح: مقتبسة من PKGi PS3 (bucanero).
- الخط DejaVu Sans. الرخصة: GPL-2.0.
