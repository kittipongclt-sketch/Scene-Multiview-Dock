# Scene Multiview Dock (ปลั๊กอิน OBS)

Dock แสดงพรีวิวหลายซีนใน OBS แบบ Multiview พร้อมแถว **PREVIEW / PROGRAM** ด้านบน ใช้งานได้โดยไม่ต้องเปิด Studio Mode
รองรับ OBS 31+ (ทดสอบบน 33.0.1) เฉพาะ Windows 64 บิต

## ติดตั้ง
1. ดาวน์โหลด `scene-multiview-X.Y.Z-setup.exe` จากหน้า [Releases](../../releases/latest)
2. **ปิด OBS ก่อน** แล้วรันตัวติดตั้ง (ติดตั้งที่ `C:\ProgramData\obs-studio\plugins\scene-multiview`)
3. เปิด OBS > เมนู **Docks** > **Scene Multiview**

ติดตั้งเอง: คัดลอกโฟลเดอร์ `bin` และ `data` จากไฟล์ zip ไปไว้ที่
`C:\ProgramData\obs-studio\plugins\scene-multiview\` (ไม่ใช่ `C:\Program Files\obs-studio\plugins`)

> Chrome อาจบล็อกไฟล์ที่ไม่มีลายเซ็น ให้กด Keep หรือโหลดด้วย Edge

## วิธีใช้
- **คลิกซีน** = ส่งไป Preview, **ดับเบิลคลิก** = ขึ้น Program ทันที (CUT)
- **CUT** / **FADE** (ตั้งเวลา FADE ได้ในช่องข้างปุ่ม) ส่ง Preview ขึ้น Program แล้ว Preview จะสลับเป็นซีนเดิมของ Program
- **Rows** 1–8 และ **Columns** 2–4 ปรับจำนวนและการเรียงซีน, ติ๊ก **Choose scenes** เพื่อเลือกซีนในแต่ละช่อง (ซีนที่เลือกแล้วจะไม่แสดงซ้ำ)
- ซีนที่มีแหล่งภาพไม่มีสัญญาณ (เช่น กล้องหลุด) จะขึ้น **⚠ ไม่มีสัญญาณ** สีส้มที่ชื่อซีน (เตือนอย่างเดียว ไม่บล็อก)
- **Layout / Presets**: บันทึกชุดซีนเป็นพรีเซ็ต เช่น "ประชุมใหญ่" "อบรม" สลับได้ในคลิกเดียว เปลี่ยนชื่อ/อัปเดต/ลบได้ในเมนู Presets
- **Window** หรือคลิกขวา: เปิดหน้าต่าง Multiview (F11 เต็มจอ, Esc ปิด) แสดงซีนแถวละ 4
- ค่าทั้งหมดบันทึกไปกับ Scene Collection และเปลี่ยนชื่อซีนใน OBS แล้วชื่อตามอัตโนมัติ

## ออก Release ใหม่ (ผู้ดูแล)
แก้ `version` ใน `buildspec.json` แล้ว push แท็กเลขล้วน เช่น
`git tag 1.1.0 && git push origin 1.1.0` — GitHub Actions จะสร้าง Release พร้อม setup.exe ให้อัตโนมัติ
