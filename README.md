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

## البناء عبر GitHub Actions
1. أنشئ مستودعاً جديداً وارفع محتويات هذا المجلد كما هي (بما فيها `.github` و `third_party`).
2. افتح تبويب **Actions** → آخر تشغيل → حمّل الـ artifact، وفيه `FileManager.pkg` (ثبّته على HEN).

## مصادر
- libntfs_ext (Estwald) و libfatfs/exFAT (lmirel): من prepISO في مشروع webMAN MOD.
- سجلات PDB للتثبيت ولوحة المفاتيح: مقتبسة من PKGi PS3 (bucanero).
- الخط DejaVu Sans. الرخصة: GPL-2.0.
